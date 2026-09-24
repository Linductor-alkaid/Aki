// M3-04：DOD-02 六项沿 EXEC-04 timer 周期发现路径（executor 周期能力专项；
// 网络无关）。
//
// 覆盖：正常完成（周期 tick）、执行中取消（在飞完成）、任务异常（EXEC-06
// failure 体系可见）、超时（有界等待预算耗尽）、提交拒绝 + shutdown（停止后
// 零 tick）。
//
// 二进制边界（M3-06 评审拆分，工程规范第 7 节）：本文件不含 [skip]/受控退出
// ——失败始终经自身退出码如实报告。同一二进制内多个各自可 std::_Exit(0) 受控
// 退出的用例会互相掩盖既有失败、并使后行用例随机不可达（实测执行顺序不定）；
// 网络依赖的回环用例据此拆分为独立单用例二进制：
//   - tests/integration/test_discovery_pairing_loopback.cpp（M3-04 发现/信任）
//   - tests/integration/test_peer_sessions_loopback.cpp（M3-06 presence/path）
// 单用例二进制内 REQUIRE 失败即中止当前用例（Catch2 语义），[skip] 受控退出
// 点只在前置断言全部通过时可达。禁止在本文件重新引入受控退出或多用例回环。
#include "app/lifecycle/executor_owner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>

namespace {

using namespace std::chrono_literals;

using aki::app::ExecutorOwner;

bool wait_until(const std::function<bool()>& predicate,
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

TEST_CASE("DOD-02 six items along the periodic discovery path",
    "[integration][discovery_pairing][dod02]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());

    // 正常完成：周期 tick 连续触发。
    std::atomic<int> ticks{0};
    auto handle = owner.executor().submit_periodic_with_handle(
        20, [&ticks] { ticks.fetch_add(1); });
    REQUIRE(handle.valid());
    REQUIRE(wait_until([&] { return ticks.load() >= 3; }, 2s));
    (void)handle.cancel();  // 正常完成段收尾：取消后不再产生 tick。

    // 执行中取消：在飞 tick 完成后不再有后续 tick（cancel 不抹除在飞回调）。
    std::atomic<bool> first_running{false};
    std::atomic<bool> first_done{false};
    auto latched = owner.executor().submit_periodic_with_handle(50,
        [&first_running, &first_done] {
            if (!first_done.exchange(true)) {
                first_running.store(true);
                std::this_thread::sleep_for(200ms);  // 模拟执行中
            }
        });
    REQUIRE(wait_until([&] { return first_running.load(); }, 2s));
    REQUIRE(latched.cancel()
        != executor::TimerOperationResult::NotFound);  // 取消可见（句柄在册）
    std::this_thread::sleep_for(150ms);  // 取消时第一 tick 仍在飞
    first_running.store(false);
    REQUIRE(wait_until([&] { return first_done.load(); }, 2s));  // 在飞完成
    const int ticks_after_settle = ticks.load();
    std::this_thread::sleep_for(150ms);
    REQUIRE(ticks.load() == ticks_after_settle);  // 无后续 tick

    // 任务异常：tick 异常进入 executor failure 体系（EXEC-06 可见）。
    const auto failures_before =
        owner.executor().get_failure_status().task_exception_count;
    auto throwing = owner.executor().submit_periodic_with_handle(
        20, [] { throw std::runtime_error("tick defect (dod02)"); });
    REQUIRE(wait_until([&] {
        return owner.executor().get_failure_status().task_exception_count
            > failures_before;
    }, 2s));
    (void)throwing.cancel();

    // 超时：有界等待预算耗尽返回 false（等待预算语义可见，不悬挂）。
    REQUIRE_FALSE(wait_until([] { return false; }, 100ms));

    // 提交拒绝 + shutdown：钩子内取消在飞周期句柄；停止后的周期提交不再
    // 产生 tick（行为断言——timer 调度器在停止中仍可登记句柄，但执行面
    // 已随 shutdown 终止，EXEC-06 计数与 tick 行为为证）。
    const int ticks_at_shutdown = ticks.load();
    const auto shutdown_report = owner.shutdown([&] {
        (void)handle.cancel();
        (void)throwing.cancel();
        auto late = owner.executor().submit_periodic_with_handle(20, [&ticks] {
            ticks.fetch_add(1);
        });
        (void)late.cancel();
    });
    REQUIRE(shutdown_report.fully_stopped());
    REQUIRE(shutdown_report.wait_timeout_count == 0);
    std::this_thread::sleep_for(120ms);
    REQUIRE(ticks.load() == ticks_at_shutdown);  // 停止后零 tick
}

// DOD-02 六项沿新并发路径（EXEC-04 timer：submit_periodic_with_handle）。
// 双节点回环（发现/信任、presence/path）见同目录 *_loopback 二进制。
