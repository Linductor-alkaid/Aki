// M3-07：重连协调器单元测试（EXEC-05 长任务取消与超时闭合；DEC-008 宿主
// 关闭纪律；SCOPE-11 断线恢复的承载层；网络无关——重连动作/恢复判定为注入
// 回调，不依赖真实网络）。
//
// 覆盖 DOD-02 六项沿重连长任务路径：
//   1) 正常完成：try_reconnect 成功 → recovered 计数、future 干净结算；
//   2) 任务异常：回调异常 → future 结算 + callback_failures 可见，循环继续
//      职责（下一轮重试）；
//   3) 提交拒绝：同 peer 在途单飞回归 false；终结记录就地消费后允许新一轮；
//      同 peer 并发 start（8 racer 竞态）仅一次获准、无失控循环（登记
//      原子性回归）；
//   4) 执行中取消：stop_all → request_task_cancel + future 消费（零悬挂），
//      cancelled 计数；
//   5) 超时：recovery_budget 耗尽 → budget_exhausted 如实计数退出；
//   6) shutdown：stop_all 于 owner.shutdown 钩子内 → fully_stopped + 零
//      未消费 future。
#include "app/application/reconnect_loop.hpp"
#include "app/lifecycle/executor_owner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

using aki::app::ExecutorOwner;
using aki::app::ReconnectCoordinator;
using aki::app::ReconnectCoordinatorOptions;
using aki::device::DeviceId;
using Hooks = aki::app::ReconnectCoordinator::PerPeerHooks;

bool wait_until_local(const std::function<bool()>& predicate,
    std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return predicate();
}

}  // namespace

TEST_CASE("Reconnect loop recovers and settles cleanly (DOD-02 normal)",
    "[unit][reconnect][dod02]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    ReconnectCoordinator coordinator{owner.executor()};

    int attempts = 0;
    Hooks hooks1{
        .try_reconnect = [&attempts] {
            ++attempts;
            return true;  // 首轮重连即成功
        },
        .is_recovered = [&attempts] { return attempts > 0; }};
    REQUIRE(coordinator.start(DeviceId{"peer-a"}, hooks1));
    REQUIRE(wait_until_local(
        [&] { return coordinator.stats().recovered == 1; }, 2s));
    REQUIRE(attempts == 1);

    const auto report = owner.shutdown([&] {
        // 自终循环的记录保留至消费（AGENTS 规则 3 future 纪律）→ 消费数 1。
        REQUIRE(coordinator.stop_all() == 1);
    });
    REQUIRE(report.fully_stopped());
}

TEST_CASE("Reconnect loop callback exceptions are visible and retrying continues",
    "[unit][reconnect][dod02]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    ReconnectCoordinator coordinator{owner.executor()};

    std::atomic<int> attempts{0};
    Hooks hooks2{
        .try_reconnect =
            [&attempts]() -> bool {
                attempts.fetch_add(1);
                throw std::runtime_error("reconnect attempt defect");
            },
        .is_recovered = [] { return false; }};
    REQUIRE(coordinator.start(DeviceId{"peer-a"}, hooks2));
    REQUIRE(wait_until_local(
        [&] { return coordinator.stats().callback_failures >= 2; }, 2s));
    REQUIRE(attempts.load() >= 2);  // 循环职责延续：异常后仍重试

    const auto report = owner.shutdown([&] {
        REQUIRE(coordinator.stop_all() == 1);  // 在途循环消费（零悬挂）
    });
    REQUIRE(report.fully_stopped());
}

TEST_CASE("Reconnect start is single-flight per peer; finished records recycle",
    "[unit][reconnect][dod02]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    ReconnectCoordinator coordinator{owner.executor()};

    std::atomic<bool> release{false};
    // 门闩用有界等待（回调契约：等待必须可解除阻塞——DEC-008/EXEC-05）：
    // 最长 1s 后返回，stop_all 的消费预算（3s）内必然就绪。
    Hooks hooks3{
        .try_reconnect =
            [&release] {
                for (int i = 0; i < 100 && !release.load(); ++i) {
                    std::this_thread::sleep_for(10ms);
                }
                return release.load();
            },
        .is_recovered = [] { return false; }};
    REQUIRE(coordinator.start(DeviceId{"peer-a"}, hooks3));
    // 同 peer 在途：第二次 start 被拒（单飞）。
    Hooks hooks4{
        .try_reconnect = [] { return true; },
        .is_recovered = [] { return false; }};
    REQUIRE_FALSE(coordinator.start(DeviceId{"peer-a"}, hooks4));
    // 不同 peer 可并行。
    Hooks hooks5{
        .try_reconnect = [] { return false; },
        .is_recovered = [] { return false; }};
    REQUIRE(coordinator.start(DeviceId{"peer-b"}, hooks5));
    REQUIRE(coordinator.running_count() == 2);

    release.store(true);
    REQUIRE(coordinator.stop_all() == 2);  // 全部消费
    // 终结记录清理后允许新一轮（就地消费语义）。
    Hooks hooks6{
        .try_reconnect = [] { return true; },
        .is_recovered = [] { return true; }};
    REQUIRE(coordinator.start(DeviceId{"peer-a"}, hooks6));
    REQUIRE(coordinator.stop_all() >= 1);
}

TEST_CASE("Concurrent start for the same peer admits exactly one loop (single-flight race)",
    "[unit][reconnect][concurrency]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    ReconnectCoordinator coordinator{owner.executor()};

    std::atomic<int> attempts{0};
    std::atomic<bool> release{false};
    // 门闩有界等待（回调契约：可解除阻塞）——保证竞态窗口内登记的循环处于
    // 在途（future 未就绪），后续 start 的单飞拒绝路径确定可达。
    Hooks hooks{
        .try_reconnect =
            [&attempts, &release] {
                attempts.fetch_add(1);
                for (int i = 0; i < 100 && !release.load(); ++i) {
                    std::this_thread::sleep_for(10ms);
                }
                return release.load();
            },
        .is_recovered = [] { return false; }};

    // 8 个 racer 经 executor 任务（AGENTS 规则 2/3：提交经 executor、future
    // 保留并消费）屏障放行后同时调用同 peer start——复现检查与登记之间
    // 的竞态窗口（若检查/登记不原子，第二个 submit 的 handle/future 被丢弃，
    // 产生 stop_all 永不取消的失控循环）。
    constexpr int kRacers = 8;
    std::atomic<int> go{0};
    std::vector<std::future<bool>> racers;
    for (int i = 0; i < kRacers; ++i) {
        racers.push_back(owner.executor().submit_auto(
            [&] {
                while (go.load() == 0) {
                    std::this_thread::yield();
                }
                return coordinator.start(DeviceId{"peer-race"}, hooks);
            }));
    }
    go.store(1);
    int admitted = 0;
    for (auto& racer : racers) {
        if (racer.get()) {
            ++admitted;
        }
    }
    REQUIRE(admitted == 1);  // 并发 start 只有一次获准（登记原子）
    REQUIRE(coordinator.stats().started == 1);
    REQUIRE(coordinator.running_count() == 1);

    // 无失控循环：stop_all 取消并消费唯一在途循环后零新尝试——若存在未登记
    // 的第二个循环，其无法被取消，attempts 将继续增长并在此暴露。
    REQUIRE(coordinator.stop_all() == 1);
    REQUIRE(coordinator.running_count() == 0);
    const auto attempts_at_rest = attempts.load();
    std::this_thread::sleep_for(150ms);
    REQUIRE(attempts.load() == attempts_at_rest);

    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("Reconnect stop_all cancels in-flight loops (DOD-02 in-execution cancel)",
    "[unit][reconnect][dod02]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    ReconnectCoordinator coordinator{owner.executor()};

    std::atomic<int> attempts{0};
    Hooks hooks7{
        .try_reconnect =
            [&attempts] {
                attempts.fetch_add(1);
                std::this_thread::sleep_for(50ms);  // 模拟重连动作耗时
                return false;
            },
        .is_recovered = [] { return false; }};
    REQUIRE(coordinator.start(DeviceId{"peer-a"}, hooks7));
    REQUIRE(wait_until_local(
        [&] { return attempts.load() >= 1; }, 2s));

    const auto consumed = coordinator.stop_all();  // EXEC-01 步骤 1 钩子纪律
    REQUIRE(consumed == 1);                        // 零悬挂
    REQUIRE(coordinator.stats().cancelled == 1);   // 执行中取消计数
    REQUIRE(coordinator.running_count() == 0);

    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("Reconnect budget exhaustion is recorded (DOD-02 timeout semantics)",
    "[unit][reconnect][dod02]") {
    ReconnectCoordinatorOptions options;
    options.recovery_budget = 150ms;  // 有意小于恢复所需
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    ReconnectCoordinator coordinator{owner.executor(), options};

    Hooks hooks8{
        .try_reconnect = [] { return false; },
        .is_recovered = [] { return false; }};
    REQUIRE(coordinator.start(DeviceId{"peer-a"}, hooks8));
    REQUIRE(wait_until_local(
        [&] { return coordinator.stats().budget_exhausted == 1; }, 2s));
    // 预算耗尽如实退出（EXEC-05，不悬挂）。

    // 自终循环的记录保留至消费。
    REQUIRE(coordinator.stop_all() == 1);
    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("Reconnect coordinator shutdown inside the owner hook is fully stopped",
    "[unit][reconnect][dod02]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    ReconnectCoordinator coordinator{owner.executor()};

    Hooks hooks9a{
        .try_reconnect = [] { return false; },
        .is_recovered = [] { return false; }};
    Hooks hooks9b{
        .try_reconnect = [] { return false; },
        .is_recovered = [] { return false; }};
    REQUIRE(coordinator.start(DeviceId{"peer-a"}, hooks9a));
    REQUIRE(coordinator.start(DeviceId{"peer-b"}, hooks9b));
    std::this_thread::sleep_for(100ms);  // 让循环进入执行（在飞/排队混合属
    // executor 调度语义：排队期取消由 executor 直接结算——run_loop 未执行、
    // cancelled 不计数；运行期取消经 StopToken 退出并计数。两 future 均由
    // 钩子消费，零悬挂——断言按此语义放宽到 >=1）。

    const auto report = owner.shutdown([&] {
        // EXEC-01 步骤 1 钩子：先取消并消费在途 future 再交还 owner。
        REQUIRE(coordinator.stop_all() == 2);
    });
    REQUIRE(report.fully_stopped());
    REQUIRE(coordinator.running_count() == 0);
    const auto snapshot = coordinator.stats();
    REQUIRE(snapshot.cancelled >= 1);
}
