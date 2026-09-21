// M1-04：Executor 生命周期 owner（app/lifecycle）shutdown 测试。
//
// 覆盖（验收标准 ①，设计第 8.2 节）：
//   - 正常关闭排空存量（EXEC-01 五步 + 完整证据）；
//   - 关闭后提交返回明确失败（不静默，AGENTS 规则 10）；
//   - 重复 shutdown 幂等、owner 不可重复初始化/关闭后不可重建；
//   - blocking worker 有可解除阻塞路径（request_stop + wakeup 契约，EXEC-05 呼应）；
//   - DOD-02 六项沿 owner 生命周期覆盖：正常完成、任务异常、提交拒绝、
//     执行中取消（阻塞等待被 request_stop 解除）、超时（owner 等待预算耗尽
//     证据如实记录）、shutdown（本文件主体）。
//
// 与 test_app_state / test_heyaki_adapter 不同，本测试不设进程级临时 Executor
// owner——被测对象就是 owner（ExecutorOwner 实例）。每个测试用例各自持有独立的
// owner（独立 ExecutorManager，资源隔离），生命周期显式 initialize/shutdown。
#include "app/lifecycle/executor_owner.hpp"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

using aki::app::ExecutorOwner;
using aki::app::ExecutorShutdownReport;

// 满足 wakeup 可解除阻塞契约的 Fake worker（设计 8.2 节步骤 3）：
// run() 在条件变量上等待（上限 10s），wakeup() 唤醒后立即重查 StopToken。
// 若 wakeup 未解除阻塞，run() 只能在 10s 超时后退出——测试用耗时断言区分。
class FakeBlockingWorker final : public executor::IBlockingIoWorker {
public:
    void run(executor::StopToken stop_token) override {
        running_.store(true);
        std::unique_lock<std::mutex> lock(mutex_);
        // wakeup() 必须解除当前等待（blocking_io.hpp 契约）；StopToken 不能中断
        // 底层阻塞调用，因此等待原语必须可被 wakeup 唤醒。
        condition_.wait_for(lock, std::chrono::seconds(10), [&] {
            return stop_token.stop_requested();
        });
        ran_to_completion_.store(true);
        running_.store(false);
    }

    void wakeup() noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++wake_requests_;
        }
        condition_.notify_all();
    }

    [[nodiscard]] bool is_running() const noexcept { return running_.load(); }
    [[nodiscard]] bool ran_to_completion() const noexcept { return ran_to_completion_.load(); }
    [[nodiscard]] std::uint64_t wake_requests() const noexcept { return wake_requests_; }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ran_to_completion_{false};
    std::uint64_t wake_requests_ = 0;  // owner 线程读写（测试串行）。
};

bool wait_until(const std::function<bool()>& predicate, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

}  // namespace

TEST_CASE("EXEC-01 five-step shutdown drains admitted work with full evidence",
    "[unit][executor_lifecycle][dod02]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    REQUIRE(owner.is_initialized());
    REQUIRE_FALSE(owner.initialize());  // owner 不可重复初始化。

    std::atomic<int> completed{0};
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 4; ++i) {
        futures.push_back(owner.executor().submit_auto([&completed] {
            completed.fetch_add(1);
        }));
    }

    // 已 admit 不等于已完成（tasks-and-lifecycle 卡）：此时不等待，直接进入
    // 受控关闭，由步骤 4/5 排空存量。
    const std::size_t blocking_before = owner.blocking_worker_count();
    const auto report = owner.shutdown([] { /* 停生产者钩子：本用例无生产者 */ });

    REQUIRE(report.producers_stopped);
    REQUIRE(report.blocking_workers_requested == blocking_before);
    REQUIRE(report.blocking_workers_stopped == blocking_before);
    REQUIRE(report.completion_wait_completed);
    REQUIRE_FALSE(report.completion_wait_timed_out);
    REQUIRE(report.executor_shutdown_completed);
    REQUIRE(report.lifecycle_after == executor::ExecutorLifecycleState::Stopped);
    REQUIRE(report.wait_timeout_count == 0);
    REQUIRE(report.fully_stopped());  // Completed + Stopped + wait_timeout_count==0

    for (auto& future : futures) {
        future.get();  // 存量任务全部完成（shutdown(true) 排空）。
    }
    REQUIRE(completed.load() == 4);
    REQUIRE(owner.is_shutdown());
}

TEST_CASE("Owner rejects re-initialization before and after shutdown",
    "[unit][executor_lifecycle]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    REQUIRE_FALSE(owner.initialize());  // 已初始化。

    (void)owner.shutdown();

    // 关闭后同一 Executor 不可重建（initialize_ex 返回 AlreadyShutdown）。
    REQUIRE_FALSE(owner.initialize());
}

TEST_CASE("Repeated shutdown is idempotent and returns the same evidence",
    "[unit][executor_lifecycle]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    (void)owner.executor().submit_auto([] { return 1; });

    const auto first = owner.shutdown();
    REQUIRE(first.fully_stopped());

    const auto second = owner.shutdown();
    REQUIRE(second.producers_stopped == first.producers_stopped);
    REQUIRE(second.executor_shutdown_completed == first.executor_shutdown_completed);
    REQUIRE(second.lifecycle_after == first.lifecycle_after);
    REQUIRE(second.wait_timeout_count == first.wait_timeout_count);
    REQUIRE(second.fully_stopped());
    REQUIRE(owner.is_shutdown());
}

TEST_CASE("Submission after shutdown fails explicitly, not silently (RULE-10/AGENTS 10)",
    "[unit][executor_lifecycle][dod02]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    (void)owner.shutdown();

    bool threw = false;
    std::string message;
    try {
        auto future = owner.executor().submit_auto([] { return 42; });
        static_cast<void>(future.get());
    } catch (const std::exception& error) {
        threw = true;
        message = error.what();
    }
    REQUIRE(threw);
    REQUIRE_FALSE(message.empty());
    INFO("post-shutdown submit message: " << message);
    // pinned executor 以明确异常结算关闭后的新提交，拒绝不静默（AGENTS 规则 10）。
    // 实测消息："Async executor not initialized. Call initialize() first."
    // （shutdown 完成后默认异步后端已停止；串行上下文路径为 "Executor is stopped"。）
    const bool explicit_rejection = message.find("stop") != std::string::npos
        || message.find("Stop") != std::string::npos
        || message.find("not initialized") != std::string::npos;
    REQUIRE(explicit_rejection);
}

TEST_CASE("Blocking worker is registered, unblocked by request_stop, and joined",
    "[unit][executor_lifecycle][dod02]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());

    auto worker = std::make_unique<FakeBlockingWorker>();
    auto* worker_ptr = worker.get();
    executor::BlockingWorkerSpec spec;
    spec.name = "aki.fake_blocking";
    spec.config.thread_name = "aki-fake-blocking";  // thread_name 必填（executor.cpp 校验）。
    spec.worker = std::move(worker);
    REQUIRE(owner.start_blocking_worker(std::move(spec)));
    REQUIRE(owner.blocking_worker_count() == 1);

    // worker 进入 run() 的阻塞等待。
    REQUIRE(wait_until([&] { return worker_ptr->is_running(); }, 2s));

    const auto started_at = std::chrono::steady_clock::now();
    const auto report = owner.shutdown([] { /* 停生产者钩子 */ });
    const auto elapsed = std::chrono::steady_clock::now() - started_at;

    REQUIRE(report.blocking_workers_requested == 1);
    REQUIRE(report.blocking_workers_stopped == 1);
    REQUIRE(report.fully_stopped());

    // 解除阻塞契约：request_stop + wakeup 使 run() 立即返回（远小于 10s 等待上限），
    // 而不是靠超时轮询退出。
    REQUIRE(worker_ptr->ran_to_completion());
    REQUIRE(worker_ptr->wake_requests() >= 1);
    REQUIRE(elapsed < std::chrono::seconds(5));

    // owner 持有的句柄：步骤 2/3 之后 worker 已停止且可见（EXEC-06）。
    // 句柄 API 经 owner 内部使用；此处以报告与 worker 自身状态断言。
}

TEST_CASE("DOD-02 task exception stays visible across a clean shutdown",
    "[unit][executor_lifecycle][dod02]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());

    auto failing = owner.executor().submit_auto(
        []() -> int { throw std::runtime_error("boom"); });
    REQUIRE_THROWS_AS(failing.get(), std::runtime_error);
    // 失败计数相对 future 结算异步记账：以有界轮询等待可见（不破坏关闭证据）。
    REQUIRE(wait_until([&] {
        return owner.executor().get_failure_status().task_exception_count >= 1;
    }, 2s));

    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());  // 任务异常不污染关闭证据（计数独立）。
    REQUIRE(owner.executor().get_failure_status().task_exception_count >= 1);
}

TEST_CASE("DOD-02 timeout: expired owner wait budget is recorded honestly",
    "[unit][executor_lifecycle][dod02]") {
    ExecutorOwner::Options options;
    options.completion_wait_budget = 30ms;  // 有意小于任务耗时。
    ExecutorOwner owner(options);
    REQUIRE(owner.initialize());

    auto slow = owner.executor().submit_auto([] {
        std::this_thread::sleep_for(300ms);
        return 7;
    });

    const auto report = owner.shutdown();
    REQUIRE(report.completion_wait_timed_out);       // 预算耗尽如实记录。
    REQUIRE_FALSE(report.completion_wait_completed);
    REQUIRE(report.wait_timeout_count >= 1);
    REQUIRE_FALSE(report.fully_stopped());  // 超时不是干净关闭，证据不伪造。

    // 步骤 5 仍由 shutdown(true) 完成等待（库内上限内任务完成）。
    REQUIRE(report.executor_shutdown_completed);
    REQUIRE(report.lifecycle_after == executor::ExecutorLifecycleState::Stopped);
    REQUIRE(slow.get() == 7);
}

TEST_CASE("Shutdown without producers or workers is a valid minimal lifecycle",
    "[unit][executor_lifecycle]") {
    ExecutorOwner owner{ExecutorOwner::Options{}};
    REQUIRE(owner.initialize());
    REQUIRE(owner.executor().submit_auto([] { return 21; }).get() == 21);
    const auto report = owner.shutdown();
    REQUIRE(report.producers_stopped);
    REQUIRE(report.blocking_workers_requested == 0);
    REQUIRE(report.blocking_workers_stopped == 0);
    REQUIRE(report.fully_stopped());
}

int main(int argc, char* argv[]) {
    // 与 test_app_state / test_heyaki_adapter 的进程级临时 owner 不同：本文件的
    // 被测对象就是 owner（ExecutorOwner），用例各自 initialize/shutdown；正式
    // 生产 owner 自 M1-06 冒烟宿主起在进程内使用（设计第 8.2 节落点说明）。
    const int result = Catch::Session().run(argc, argv);
    return result;
}
