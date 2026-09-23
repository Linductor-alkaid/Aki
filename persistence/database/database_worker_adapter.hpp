// DatabaseWorker 注册侧接线头（M2-05；设计第 8.2 节 / 第 11.1 节 ③）。
//
// 本头文件是注册侧“接线”层：DatabaseWorkerRunnable 实现
// executor::IBlockingIoWorker（executor 类型按设计第 8.2 节允许存在于该
// 接线层；RULE-10 守卫的公开面是 database_worker.hpp 及仓储/迁移公开头，
// 均不含 executor/sqlite 类型）。
//
// 宿主组合（EXEC-07，设计第 11.1 节 ③）：
//   auto control = std::make_shared<DatabaseWorkerControl>(options);
//   auto runnable = std::make_unique<DatabaseWorkerRunnable>(
//       std::move(repos), control);
//   executor::BlockingWorkerSpec spec;
//   spec.name = "aki.db-worker";
//   spec.config.thread_name = "aki-db-worker";   // 库校验必填
//   spec.worker = std::move(runnable);
//   owner.start_blocking_worker(std::move(spec));
// 之后宿主仅经 control 入队/排空/观测（线程安全，生命周期覆盖全程）。
#pragma once

#include "persistence/database/database_worker.hpp"

#include <executor/blocking_io.hpp>
#include <executor/comm/channel.hpp>
#include <executor/comm/types.hpp>
#include <executor/stop_token.hpp>

#include <atomic>
#include <memory>
#include <utility>

namespace aki::persistence {

// 控制面共享状态：通道 + 原子旗标 + 计数（宿主与 runnable 两端共享）。
struct DatabaseWorkerControl::Impl {
    explicit Impl(DatabaseWorkerOptions worker_options)
        : options(worker_options),
          channel(executor::comm::ChannelOptions{
              .capacity = worker_options.channel_capacity,
              .enable_stats = true,
              .name = "aki.db-worker.jobs"}) {}

    DatabaseWorkerOptions options;
    executor::comm::MpscChannel<DbJob> channel;
    std::atomic<bool> registered{false};
    std::atomic<bool> drain_requested{false};
    std::atomic<bool> exit_requested{false};
    std::atomic<bool> drain_completed{false};
    std::atomic<bool> drain_budget_exhausted{false};
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> failed{0};
    std::atomic<std::uint64_t> rejected{0};
};

// IBlockingIoWorker 适配器：单一连接独占 + 有界通道串行消费（EXEC-04）。
// 对象所有权归 executor facade（注册后由其销毁，M1-04 实测）；宿主仅经
// DatabaseWorkerControl 交互。
class DatabaseWorkerRunnable final : public executor::IBlockingIoWorker {
public:
    explicit DatabaseWorkerRunnable(
        std::unique_ptr<Repositories> repositories,
        std::shared_ptr<DatabaseWorkerControl> control)
        : repos_(std::move(repositories)),
          control_(control, control->impl_.get()) {}  // 别名构造：生命周期随 control

    DatabaseWorkerRunnable(const DatabaseWorkerRunnable&) = delete;
    DatabaseWorkerRunnable& operator=(const DatabaseWorkerRunnable&) = delete;

    // 消费循环：轮询通道（短睡眠有界等待）→ 逐作业执行（异常经 promise 结算，
    // worker 存活）→ 作业间检查 StopToken；drain 请求后排空（有界预算）→
    // 关闭通道 → 退出（EXEC-01 步骤 2/3 随后由 facade request_stop/stop 回收）。
    // 实现注记（M2-05 验证记录）：等待采用 try_receive + 短睡眠轮询环——
    // pinned v0.5.0-7 的 receive_for 在本场景观测到已 admit 作业不可见/进程
    // 异常终止（上游缺陷，经官方 DLL 复现），轮询环为集成指南备选 ⑧；
    // 停止/排空响应延迟上界 = 轮询间隔（默认 10ms），满足 §8.2 步骤 3。
    void run(executor::StopToken stop_token) override {
        // 排空预算锚定于 drain 分支首次被观察到时（非 run 启动）：进程存活
        // 超过 drain_budget 后才请求排空时预算仍须完整可用——锚定启动时刻会
        // 使排空在首个作业后耗尽、已 admit 存量被放弃，违反第 11.1 节 ③
        // “不丢作业”契约。
        std::chrono::steady_clock::time_point drain_deadline{};
        bool drain_deadline_set = false;
        for (;;) {
            // drain 请求（EXEC-01 步骤 1 钩子内，先于步骤 2）：排空优先于
            // StopToken——预算在作业间检查，耗尽如实记录。
            if (control_->exit_requested.load()) {
                // 协作退出：存量按取消结算（future 明确可见，不悬挂），
                // 关闭通道使 exit 后入队明确拒绝（RULE-09：admission 不撒谎）。
                cancel_remaining_jobs("worker exit requested");
                control_->channel.close();
                return;
            }
            if (control_->drain_requested.load()) {
                if (!drain_deadline_set) {
                    drain_deadline = std::chrono::steady_clock::now()
                        + control_->options.drain_budget;
                    drain_deadline_set = true;
                }
                DbJob job;
                while (control_->channel.try_receive(job)) {
                    execute(job);
                    if (std::chrono::steady_clock::now() >= drain_deadline
                        && control_->channel.size_approx() > 0) {
                        control_->drain_budget_exhausted.store(true);
                        break;  // 预算耗尽：不伪造排空完成
                    }
                }
                if (control_->drain_budget_exhausted.load()) {
                    // 存量按取消结算：不执行，但 promise 必须结算（见下）。
                    cancel_remaining_jobs("drain budget exhausted");
                }
                control_->channel.close();  // 排空完成后通道关闭
                control_->drain_completed.store(true);
                return;
            }
            // StopToken 在作业间检查（取消粒度=语句间；在飞作业不打断）。
            if (stop_token.stop_requested()) {
                // EXEC-01 步骤 2：正常流程宿主已先排空（通道已关且空，此处
                // 为幂等 no-op）；无排空的关闭路径存量同样按取消结算，不悬挂。
                cancel_remaining_jobs("worker stop requested");
                control_->channel.close();
                return;
            }
            DbJob job;
            if (control_->channel.try_receive(job)) {
                execute(job);
                continue;
            }
            std::this_thread::sleep_for(control_->options.wait_timeout);
        }
    }

    // 平凡 noexcept 实现：通道等待为有界自旋（≤wait_timeout 内自解），
    // 结构性满足 §8.2 步骤 3 可解除阻塞契约。
    void wakeup() noexcept override {}

private:
    void execute(DbJob& job) {
        try {
            job.work(*repos_);
            control_->completed.fetch_add(1);  // 先计数后结算（消除与 future 的竞态）
            job.done->set_value();
        } catch (...) {
            // 逐作业结算异常（worker 存活；未捕获异常会以 WorkerException
            // 终止 worker——硬实现纪律）。
            control_->failed.fetch_add(1);
            job.done->set_exception(std::current_exception());
        }
    }

    // 存量作业放弃结算（各退出路径统一出口）：不执行（宿主已放弃该批作业），
    // 但每个 promise 必须以取消异常结算——调用方 future 得到明确结果而非
    // 悬挂至 broken_promise（AGENTS 规则 10 / RULE-09；DbJob 契约“成功
    // set_value、异常 set_exception”的关闭侧闭环）。计入 failed 保持
    // EXEC-06 观测一致。
    void cancel_remaining_jobs(const char* context) {
        DbJob job;
        while (control_->channel.try_receive(job)) {
            control_->failed.fetch_add(1);
            job.done->set_exception(
                std::make_exception_ptr(JobCancelledError(context)));
        }
    }

    std::unique_ptr<Repositories> repos_;
    std::shared_ptr<DatabaseWorkerControl::Impl> control_;
};

}  // namespace aki::persistence
