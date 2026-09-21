// Executor 生命周期 owner（设计第 8.2 节，EXEC-01/EXEC-07，AGENTS 规则 7/8）。
//
// Aki 进程内每个 executor 生命周期有且仅有一个 owner：本类以独立实例持有 pinned
// executor 的 Executor facade（非单例，资源隔离），显式 initialize_ex，受控关闭。
// 依赖经构造参数或明确 context 传递；blocking worker 句柄由 owner 注册并持有。
// 禁止其他组件隐藏 Executor 生命周期或私调 enable_monitoring 切换。
//
// 线程契约：initialize() / shutdown() 只能由 owner 线程（非池 worker）调用；
// shutdown 从池 worker 内调用会得到 RequestedFromWorker 且不完成 teardown（禁止）。
// executor() 访问器的前置条件是已初始化——facade 对未初始化实例的首次提交会以
// 默认配置懒初始化，绕过 owner 纪律，调用方必须先 initialize()。
#pragma once

#include <executor/blocking_io.hpp>
#include <executor/config.hpp>
#include <executor/executor.hpp>
#include <executor/types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

namespace aki::app {

// 关闭证据（EXEC-01 五步 + EXEC-06 观测；设计第 8.2 节）。
struct ExecutorShutdownReport {
    // 步骤 1：应用注入的生产者停止钩子已执行
    // （停 Heyaki 投递 → drain/close comm 通道 → 停快照发布，设计 10.1 硬约束 3）。
    bool producers_stopped = false;
    // 步骤 2：request_stop（非阻塞置位 + 唤醒）的 worker 数。
    std::size_t blocking_workers_requested = 0;
    // 步骤 3：stop（request + wakeup + join）回收的 worker 数。
    std::size_t blocking_workers_stopped = 0;
    // 步骤 4：owner 预算内有界等待的结果（只覆盖默认异步 future 型任务）。
    bool completion_wait_completed = false;
    bool completion_wait_timed_out = false;
    std::chrono::milliseconds completion_wait_budget{0};
    // 步骤 5：shutdown(true) 返回 Completed（非 worker 线程执行）。
    bool executor_shutdown_completed = false;
    // 关闭证据（EXEC-06）：快照生命周期 + 等待超时计数。
    executor::ExecutorLifecycleState lifecycle_after = executor::ExecutorLifecycleState::Created;
    std::uint64_t wait_timeout_count = 0;

    // 干净关闭：五步全部完成且无等待超时。预算耗尽时本值为 false（证据不伪造），
    // 但 shutdown 本身仍按上表字段如实记录。
    [[nodiscard]] bool fully_stopped() const noexcept {
        return producers_stopped && executor_shutdown_completed
            && lifecycle_after == executor::ExecutorLifecycleState::Stopped
            && wait_timeout_count == 0;
    }
};

// 构造选项置于命名空间作用域：类内默认实参引用嵌套类型的 NSDMI 在 GCC 下非法
// （同 app_state_owner.hpp 的 AppStateOwnerOptions 处理）。
struct ExecutorOwnerOptions {
    // 透传 pinned executor 配置；enable_monitoring 默认开启（config.hpp），
    // 随 initialize_ex 传入，运行期切换（如有）归 owner。
    executor::ExecutorConfig executor_config{};
    // 步骤 4 的 owner 等待预算：区别于库内 shutdown 的 300s 不可配内部上限，
    // 业务等待策略由 owner 显式预算（production-readiness：budget every wait）。
    std::chrono::milliseconds completion_wait_budget{3000};
};

class ExecutorOwner {
public:
    // 应用注入的“停止任务生产者”钩子（EXEC-01 步骤 1）。M1-05/M1-06 起由宿主
    // 组合：停 Heyaki 投递 → drain/close comm 通道 → 停快照发布。钩子异常向上
    // 传播（失败可见，不静默），此时后续步骤不执行、可再次 shutdown 重试。
    using StopProducersHook = std::function<void()>;

    using Options = ExecutorOwnerOptions;

    explicit ExecutorOwner(ExecutorOwnerOptions options = {}) : options_(std::move(options)) {}

    ExecutorOwner(const ExecutorOwner&) = delete;
    ExecutorOwner& operator=(const ExecutorOwner&) = delete;

    // 兜底：生产代码必须显式 shutdown()；析构仅在 owner 线程遗漏时执行幂等关闭。
    ~ExecutorOwner() {
        if (!shutdown_done_) {
            (void)shutdown();
        }
    }

    // 显式初始化（唯一初始化入口）。重复初始化或关闭后重建返回 false
    // （pinned executor：关闭后 initialize_ex 返回 AlreadyShutdown，不可重建）。
    [[nodiscard]] bool initialize() {
        if (initialized_ || shutdown_done_) {
            return false;
        }
        const auto result = executor_.initialize_ex(options_.executor_config);
        initialized_ = result.ok;
        return initialized_;
    }

    // EXEC-01 五步受控关闭（设计第 8.2 节）。重复调用幂等，返回上一次报告。
    [[nodiscard]] ExecutorShutdownReport shutdown(StopProducersHook stop_producers = {}) {
        if (shutdown_done_) {
            return last_report_;
        }

        ExecutorShutdownReport report;

        // EXEC-01 步骤 1：停止任务生产者（钩子异常则向上传播，后续步骤不执行）。
        if (stop_producers) {
            stop_producers();
        }
        report.producers_stopped = true;

        // EXEC-01 步骤 2：发出取消/停止请求——blocking worker request_stop()
        // 为 noexcept 非阻塞（置位停止标志 + wakeup 后立即返回，不 join）；
        // 定时任务与运行中任务的取消由持有句柄的 Manager 在步骤 1 的钩子前发起
        // （EXEC-07）。
        for (auto& handle : blocking_workers_) {
            handle.request_stop();
        }
        report.blocking_workers_requested = blocking_workers_.size();

        // EXEC-01 步骤 3：回收 blocking worker——stop() = request + wakeup + join，
        // 重复停止安全；worker run() 须满足 wakeup 可解除阻塞契约（设计 8.2 节）。
        for (auto& handle : blocking_workers_) {
            handle.stop();
        }
        report.blocking_workers_stopped = blocking_workers_.size();

        // EXEC-01 步骤 4：owner 预算内有界等待有限任务（只覆盖默认异步 future 型
        // 任务；blocking worker 已在步骤 3 join）。超时记录 WaitResult 作证据，
        // 不在此处静默转非等待路径。
        const auto wait =
            executor_.wait_for_completion_ex(options_.completion_wait_budget);
        report.completion_wait_completed = wait.completed;
        report.completion_wait_timed_out = wait.timed_out;
        report.completion_wait_budget = options_.completion_wait_budget;

        // EXEC-01 步骤 5：非 worker 线程执行最终 shutdown(true)。返回 Completed
        // 才算关闭完成；RequestedFromWorker 表示从池 worker 内调用（禁止）。
        const auto result = executor_.shutdown(true);
        report.executor_shutdown_completed = (result == executor::ShutdownResult::Completed);

        // 关闭证据读取：状态记账（lifecycle、failure 计数）相对 shutdown 返回值
        // 异步收敛，owner 以短预算轮询至 Stopped 或预算耗尽；读不到 Stopped 时
        // 如实记录最后一次观测（证据不伪造）。
        const auto evidence_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        for (;;) {
            const auto snapshot = executor_.get_snapshot();
            report.lifecycle_after = snapshot.lifecycle;
            report.wait_timeout_count = executor_.get_failure_status().wait_timeout_count;
            if (snapshot.lifecycle == executor::ExecutorLifecycleState::Stopped
                || std::chrono::steady_clock::now() >= evidence_deadline) {
                break;
            }
            std::this_thread::yield();
        }

        shutdown_done_ = true;
        last_report_ = report;
        return last_report_;
    }

    // M1-05/M2 预留：blocking worker 注册（EXEC-04/EXEC-07）。句柄归 owner，
    // 关闭顺序中由 owner 统一 request_stop/stop；业务侧只经 spec.worker 轮询。
    [[nodiscard]] bool start_blocking_worker(executor::BlockingWorkerSpec spec) {
        auto handle = executor_.start_worker(std::move(spec));
        if (!handle.started()) {
            return false;  // 启动 admission 失败对调用方可见（RULE-09）。
        }
        blocking_workers_.push_back(std::move(handle));
        return true;
    }

    // 前置条件：initialize() 已成功（见类注释的懒初始化禁令）。
    [[nodiscard]] executor::Executor& executor() noexcept { return executor_; }

    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }
    [[nodiscard]] bool is_shutdown() const noexcept { return shutdown_done_; }
    [[nodiscard]] std::size_t blocking_worker_count() const noexcept {
        return blocking_workers_.size();
    }
    [[nodiscard]] const ExecutorShutdownReport& last_report() const noexcept {
        return last_report_;
    }

private:
    Options options_;
    executor::Executor executor_;
    std::vector<executor::WorkerHandle> blocking_workers_;
    bool initialized_ = false;
    bool shutdown_done_ = false;
    ExecutorShutdownReport last_report_{};
};

}  // namespace aki::app
