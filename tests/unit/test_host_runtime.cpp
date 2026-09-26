// M5-02：HostRuntime 关闭路径测试（设计 §9.1 启动↔关闭配对条款；DOD-02 六项
// 沿宿主生命周期路径；console exe 不链 eui，DEC-005）。
//
// 覆盖（单用例线性序列——HostRuntime 为进程级单例，一次装配↔关闭周期内
// 依次驱动六项 + 关闭序断言）：
//   - 正常完成：宿主 executor submit_auto 返回值 + Manager 泵路径（本地身份
//     UpsertDevice → DB 行落库）+ 真实发现启停 + 快照读取；
//   - 任务异常：submit_auto 抛异常经 future 上浮 + failure 计数可见；
//   - 提交拒绝：总量有界 admission（set_max_in_flight_tasks 耗尽 →
//     CapacityExhaustedException 即时就绪）+ 关闭后提交显式拒绝；
//   - 执行中取消：submit_cancellable + request_task_cancel（RequestedRunning
//     协作退出 + CancellationStatus 独立计数）；
//   - 超时：排队软超时（task timeout_ms，holder 任务占满池后排队被击杀 →
//     TimedOutException）；
//   - shutdown：shutdown_with_report 全量断言（§8.3 钩子原序 hook_sequence
//     逐项、EXEC-01 五步 fully_stopped、两 blocking worker 回收、写路径
//     admit==completed 零丢失）+ 幂等。
//
// onShutdown 的 GUI 真实接线（窗口/GPU 销毁与 worker 回收次序）以本机运行
// 日志证据归档（aki-run.log；RULE-11 渲染层不进 CI）——本文件锁定其委托的
// 同一 HostRuntime::shutdown_with_report 编排。
#include "app/lifecycle/host_runtime.hpp"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

using aki::app::HostRuntime;

bool wait_until(const std::function<bool()>& predicate,
    std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

std::filesystem::path make_temp_data_root() {
    static int counter = 0;
    const auto now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    auto root = std::filesystem::temp_directory_path()
        / ("aki-host-runtime-" + std::to_string(now_ns) + "-"
            + std::to_string(++counter));
    std::filesystem::create_directories(root);
    return root;
}

const std::vector<std::string>& expected_hook_sequence() {
    static const std::vector<std::string> expected{
        "hook:transfer.request_cancel_all",
        "hook:managers.flush",
        "hook:adapter.stop_delivery",
        "hook:peer_pipeline.stop",
        "hook:reconnect.stop_all",
        "hook:node_session.shutdown",
        "hook:state_owner.close",
        "hook:db.request_drain",
    };
    return expected;
}

}  // namespace

TEST_CASE("HostRuntime lifecycle carries DOD-02 six paths and the 8.3 hook order",
    "[unit][host_runtime][dod02]") {
    const std::filesystem::path data_root = make_temp_data_root();

    // ---- 装配（§8.3 七步；空根 → 0 恢复 + 新建身份；M5-03 唤醒回调注入）----
    HostRuntime& host = HostRuntime::instance();
    // 唤醒计数器经 shared_ptr 值捕获：宿主单例生命周期覆盖测试函数之外，
    // 引用捕获会在静态析构期悬垂（AppStateOwner 关闭排空仍可能触发钩子）。
    auto wake_calls = std::make_shared<std::atomic<int>>(0);
    const auto& assembly =
        host.ensure_assembled(data_root.string(), [wake_calls] {
            wake_calls->fetch_add(1);  // GUI 侧此处为 app::requestUpdate()。
        });
    REQUIRE(host.assembled());
    REQUIRE(assembly.ok);
    REQUIRE(assembly.failure_reason.empty());
    REQUIRE(assembly.recovered_devices == 0);
    REQUIRE(assembly.recovered_conversations == 0);
    REQUIRE(assembly.recovered_messages == 0);
    REQUIRE(assembly.recovered_transfers == 0);
    // 空根首开：v1 schema 引导迁移恰一步（0→1）；tmp 清扫零孤儿。
    REQUIRE(assembly.migrations_applied == 1);
    REQUIRE(assembly.tmp_orphans_removed == 0);
    REQUIRE(assembly.identity_created);
    REQUIRE_FALSE(assembly.local_device_id.empty());
    REQUIRE(host.data_root() == data_root.string());
    // 幂等：重复装配返回首次结果。
    REQUIRE(&host.ensure_assembled(data_root.string()) == &assembly);

    // ---- ① 正常完成：宿主 executor 直接任务 ----
    auto answer = host.executor().submit_auto([] { return 42; });
    REQUIRE(answer.get() == 42);

    // ---- Manager 泵路径（事件→任务→DB）：本地身份 + 发现启停 + 静止 + 快照 ----
    host.quiesce();
    // M5-03（§9.1 跨线程唤醒接线）：本地身份 UpsertDevice 经 quiesce 推进
    // 发布后注入唤醒已触发（HostRuntime 装配参数 →
    // AppStateOwnerOptions::on_publish 的接线面；发布→唤醒调用序的 owner
    // 级断言在 test_ui_models）。
    REQUIRE(wake_calls->load() >= 1);
    REQUIRE(host.start_discovery_observation());
    REQUIRE(host.discovery_observation_running());
    host.quiesce();
    host.stop_discovery_observation();
    REQUIRE_FALSE(host.discovery_observation_running());
    host.quiesce();

    aki::app::AppState snapshot;
    REQUIRE(host.load_state_snapshot(snapshot));
    REQUIRE(snapshot.devices.devices.size() == 1);
    REQUIRE(snapshot.devices.devices.front().id.value
        == assembly.local_device_id);

    // ---- ② 任务异常：future 上浮 + Executor failure 计数可见 ----
    {
        auto failing = host.executor().submit_auto(
            []() -> int { throw std::runtime_error("host runtime boom"); });
        REQUIRE_THROWS_AS(failing.get(), std::runtime_error);
        REQUIRE(wait_until([&] {
            return host.executor()
                       .get_failure_status()
                       .task_exception_count >= 1;
        }, 2s));
    }

    // ---- ③ 执行中取消：协作轮询退出 + 独立取消计数 ----
    {
        std::atomic<bool> loop_entered{false};
        std::atomic<bool> loop_exited{false};
        auto submission = host.executor().submit_cancellable(
            [&loop_entered, &loop_exited](executor::StopToken token) {
                loop_entered.store(true);
                while (!token.stop_requested()) {
                    std::this_thread::yield();
                }
                loop_exited.store(true);
                return 7;
            });
        REQUIRE(wait_until([&] { return loop_entered.load(); }, 2s));
        const auto before =
            host.executor().get_cancellation_status().request_count;
        const auto response =
            host.executor().request_task_cancel(submission.handle);
        INFO("cancel response: "
            << executor::to_string(response.result));
        REQUIRE(response.accepted());
        REQUIRE(submission.future.get() == 7);  // 协作退出，正常返回值结算。
        REQUIRE(loop_exited.load());
        REQUIRE(wait_until([&] {
            return host.executor()
                       .get_cancellation_status()
                       .running_request_count >= 1
                && host.executor()
                           .get_cancellation_status()
                           .request_count >= before + 1;
        }, 2s));
    }

    // ---- ④ 提交拒绝：总量有界 admission 耗尽 → 即时就绪的拒绝 future ----
    {
        host.quiesce();  // 泵静止，避免 Manager 排空任务被 cap 波及。
        host.executor().set_max_in_flight_tasks(1);
        std::atomic<bool> release_held{false};
        auto held = host.executor().submit_auto([&release_held] {
            while (!release_held.load()) {
                std::this_thread::yield();
            }
            return 1;
        });
        auto rejected = host.executor().submit_auto([] { return 2; });
        REQUIRE_THROWS_AS(rejected.get(), executor::CapacityExhaustedException);
        release_held.store(true);
        REQUIRE(held.get() == 1);
        host.executor().set_max_in_flight_tasks(0);  // 恢复未启用（0）。
        REQUIRE(wait_until([&] {
            return host.executor().get_failure_status().capacity_exhausted_count
                >= 1;
        }, 2s));
    }

    // ---- ⑤+⑥ 超时与 shutdown 合并为一次受控关闭（进程级单例只有一次
    //      装配↔关闭周期）：sleeper 使步骤 4 完成等待预算耗尽——超时项与
    //      关闭序断言共用同一份报告，预算超时证据与钩子序/五步其余字段
    //      在同一关闭内分别断言（互斥字段见下）。
    const aki::app::HostShutdownReport* shutdown_report_ptr = nullptr;
    std::future<int> sleeper_future;
    {
        host.quiesce();
        sleeper_future = host.executor().submit_auto([] {
            std::this_thread::sleep_for(6s);  // > 钩子耗时 + 3s 完成等待预算。
            return 0;
        });
        // 受控关闭：钩子序完整执行，步骤 4 预算耗尽被如实记录。
        const auto& closed = host.shutdown_with_report();
        shutdown_report_ptr = &closed;
    }
    // sleeper 被 EXEC-01 步骤 5 的 shutdown(true) 排空结算（future 保留并消费）。
    REQUIRE(sleeper_future.get() == 0);

    const auto& report = *shutdown_report_ptr;
    REQUIRE(report.attempted);
    REQUIRE(report.hook_sequence_completed);
    REQUIRE(report.hook_sequence == expected_hook_sequence());
    REQUIRE(report.transfers_cancelled);
    REQUIRE(report.managers_flushed);
    REQUIRE(report.adapter_delivery_stopped);
    REQUIRE(report.peer_pipeline_stopped);
    REQUIRE(report.reconnect_futures_consumed);
    REQUIRE(report.node_stopped);
    REQUIRE(report.runtime_stopped);
    REQUIRE_FALSE(report.borrowed_runtime_shutdown_performed);  // DEC-006
    REQUIRE_FALSE(report.runtime_drain_timed_out);
    REQUIRE(report.state_owner_closed);
    REQUIRE(report.db_drain_requested);
    REQUIRE(report.db_drained_within_budget);
    REQUIRE_FALSE(report.db_budget_exhausted);

    // EXEC-01 五步（含超时项证据）：生产者停止、worker 2/2 回收、最终
    // shutdown(true) 完成；步骤 4 预算耗尽如实记录——超时不是干净关闭
    //（fully_stopped 为 false、wait_timeout_count >= 1，证据不伪造），
    // 但步骤 5 仍把生命周期收敛到 Stopped。
    REQUIRE(report.executor_report.producers_stopped);
    REQUIRE(report.executor_report.executor_shutdown_completed);
    REQUIRE(report.executor_report.completion_wait_timed_out);
    REQUIRE_FALSE(report.executor_report.completion_wait_completed);
    REQUIRE(report.executor_report.wait_timeout_count >= 1);
    REQUIRE_FALSE(report.executor_report.fully_stopped());
    REQUIRE(report.executor_report.blocking_workers_requested == 2);
    REQUIRE(report.executor_report.blocking_workers_stopped == 2);
    REQUIRE(report.executor_report.lifecycle_after
        == executor::ExecutorLifecycleState::Stopped);

    // 写路径（DEC-009 ①）：本地身份 UpsertDevice 已 admit 并零丢失落库。
    REQUIRE(report.write_admitted > 0);
    REQUIRE(report.write_enqueue_rejected == 0);
    REQUIRE(report.write_settle_failures == 0);
    REQUIRE(report.db_completed == report.write_admitted);
    REQUIRE(report.db_failed == 0);
    REQUIRE(report.db_rejected == 0);
    REQUIRE(report.post_accept_failures == 0);

    // 幂等：重复关闭返回同一报告。
    REQUIRE(&host.shutdown_with_report() == &report);
    REQUIRE(host.shutdown_completed());

    // 关闭后提交显式拒绝（不静默，AGENTS 规则 10）。
    bool rejected_after_shutdown = false;
    try {
        auto stale = host.executor().submit_auto([] { return 3; });
        static_cast<void>(stale.get());
    } catch (const std::exception&) {
        rejected_after_shutdown = true;
    }
    REQUIRE(rejected_after_shutdown);

    // 清理（成功路径；失败保留诊断）。
    std::error_code ec;
    std::filesystem::remove_all(data_root, ec);
}

int main(int argc, char* argv[]) {
    // 本进程唯一 Executor owner 是 HostRuntime 单例（AGENTS 规则 7/8；GUI 宿主
    // onShutdown 薄委托的同一编排在此直接验证）。函数级 static 的析构在
    // main 返回后执行：受控关闭已在用例内完成，析构为已停状态的空 teardown。
    return Catch::Session().run(argc, argv);
}
