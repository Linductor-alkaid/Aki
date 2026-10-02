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
//
// M5-11（设计 §8.1/§11.1）：装配即启动 peer_sessions 观察管道
//（peer_observation_started / peer_observation_running 证据面）与启动恢复
// 播种策略 seeded_app_state（历史 Unknown 设备行不进入会话 DeviceStore，
// 其余信任态与其他域原值恢复）——播种为纯函数单测，不触碰 HostRuntime 单例。
//
// M5-16（DEC-020）：Settings 改本机设备名 set_device_name——未装配/已关闭
// 拒绝、非法名零扰动拒绝、合法名（含中文）快照改名 + 重启语义 DB 持久化、
// 同名幂等；广播名校验由 LanNameBeacon::set_name 纯逻辑用例锁定。
#include "app/lifecycle/host_runtime.hpp"
#include "heyaki/adapter/lan_name_beacon.hpp"
#include "heyaki/adapter/local_identity.hpp"

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
#include <fstream>
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
    // M5-29：真实 DB 恢复行，验证组合根将同一 ID/端点播种到 CM。
    aki::conversation::Conversation restored;
    restored.id = aki::conversation::ConversationId{"historical-conversation"};
    restored.local_device = aki::device::DeviceId{"historical-local"};
    restored.remote_device = aki::device::DeviceId{"historical-peer"};
    restored.state = aki::conversation::ConversationState::Disconnected;
    {
        auto seeded = aki::persistence::perform_startup_recovery(data_root.string());
        REQUIRE(seeded.diagnostics.migrations_applied == 4);
        for (const auto& id : {restored.local_device, restored.remote_device}) {
            aki::device::DeviceIdentity row;
            row.id = id;
            row.display_name = id.value;
            seeded.repositories->devices.upsert(row);
        }
        seeded.repositories->conversations.upsert(restored);
    }

    // ---- 装配（§8.3 七步；历史会话恢复 + 新建身份；M5-03 唤醒回调注入）----
    HostRuntime& host = HostRuntime::instance();
    // M5-16：未装配（默认构造单例）set_device_name 显式拒绝。
    REQUIRE_FALSE(host.set_device_name("pre-assembly-name"));
    // 唤醒计数器经 shared_ptr 值捕获：宿主单例生命周期覆盖测试函数之外，
    // 引用捕获会在静态析构期悬垂（AppStateOwner 关闭排空仍可能触发钩子）。
    auto wake_calls = std::make_shared<std::atomic<int>>(0);
    const auto& assembly =
        host.ensure_assembled(data_root.string(), [wake_calls] {
            wake_calls->fetch_add(1);  // GUI 侧此处为 app::requestUpdate()。
        }, "test-local-password");
    REQUIRE(host.assembled());
    REQUIRE(host.set_language("en"));
    REQUIRE(assembly.ok);
    REQUIRE(assembly.failure_reason.empty());
    REQUIRE(assembly.recovered_devices == 2);
    REQUIRE(assembly.recovered_conversations == 1);
    REQUIRE(assembly.recovered_messages == 0);
    std::string password_error;
    REQUIRE(host.set_local_pairing_password("rotated-password",
        password_error));
    REQUIRE(password_error.empty());
    auto profile_after_rotation = aki::heyaki::LocalProfile::open(
        data_root.string());
    const auto verifier = profile_after_rotation.store().password_verifier();
    REQUIRE(verifier.has_value());
    REQUIRE(verifier.value_if()->has_value());
    const auto matched = ::heyaki::verify_password(
        "rotated-password", **verifier.value_if());
    REQUIRE(matched.has_value());
    REQUIRE(*matched.value_if());
    REQUIRE(assembly.recovered_transfers == 0);
    // DB 已由恢复夹具完成三步迁移；装配重开零迁移、tmp 清扫零孤儿。
    REQUIRE(assembly.migrations_applied == 0);
    REQUIRE(assembly.tmp_orphans_removed == 0);
    REQUIRE(assembly.identity_created);
    REQUIRE_FALSE(assembly.local_device_id.empty());
    REQUIRE(host.data_root() == data_root.string());
    // 幂等：重复装配返回首次结果。
    REQUIRE(&host.ensure_assembled(data_root.string()) == &assembly);

    // ---- M5-11：peer_sessions 观察管道随装配启动（事件源常驻证据面）----
    REQUIRE(assembly.peer_observation_started);
    REQUIRE(host.peer_observation_running());

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
    REQUIRE(snapshot.conversations.conversations.size() == 1);
    REQUIRE(snapshot.conversations.conversations.front().id == restored.id);
    REQUIRE(snapshot.conversations.conversations.front().state == restored.state);
    REQUIRE(host.conversation_manager().enqueue_peer_connected(restored.remote_device));
    host.quiesce();
    REQUIRE(host.load_state_snapshot(snapshot));
    REQUIRE(snapshot.conversations.conversations.front().state
        == aki::conversation::ConversationState::Active);
    REQUIRE(snapshot.conversations.conversations.front().id == restored.id);
    REQUIRE(host.conversation_manager().enqueue_peer_disconnected(restored.remote_device));
    host.quiesce();
    REQUIRE(host.load_state_snapshot(snapshot));
    REQUIRE(snapshot.conversations.conversations.front().state == restored.state);

    // ---- M5-16：Settings 改本机设备名（set_device_name；广播热更新为
    //      DEC-020 尽力而为元数据，本用例锁定快照/DB 权威面）----
    {
        const auto& local_row = snapshot.devices.devices.front();
        REQUIRE(local_row.display_name == "aki");  // 首启默认名（无注入名）。

        // (b) 非法名（空 / 65 字节 / 控制字符）→ false 且状态零扰动：校验在
        // HostRuntime 层，更新不进 owner（updates_rejected 不动、快照不变）。
        const auto rejected_before =
            host.state_owner().stats().updates_rejected;
        REQUIRE_FALSE(host.set_device_name(""));
        REQUIRE_FALSE(host.set_device_name(std::string(65, 'a')));
        REQUIRE_FALSE(host.set_device_name("aki\nname"));
        REQUIRE_FALSE(host.set_device_name(std::string{"aki\x01" "name"}));
        host.quiesce();
        REQUIRE(host.state_owner().stats().updates_rejected
            == rejected_before);
        aki::app::AppState unchanged;
        REQUIRE(host.load_state_snapshot(unchanged));
        REQUIRE(unchanged.devices.devices.front().display_name == "aki");

        // (a) 合法名（含中文 UTF-8）：quiesce 推进后快照本机行改名，公钥
        // 绑定不变。
        REQUIRE(host.set_device_name("小明的工作站"));
        host.quiesce();
        aki::app::AppState renamed;
        REQUIRE(host.load_state_snapshot(renamed));
        REQUIRE(renamed.devices.devices.size() == 1);
        REQUIRE(renamed.devices.devices.front().display_name
            == "小明的工作站");
        REQUIRE(renamed.devices.devices.front().public_key
            == local_row.public_key);

        // (d) 同名重复提交幂等 → true（apply 幂等 no-op，状态不再脏发布）。
        REQUIRE(host.set_device_name("小明的工作站"));
        host.quiesce();
        aki::app::AppState idempotent;
        REQUIRE(host.load_state_snapshot(idempotent));
        REQUIRE(idempotent.devices.devices.front().display_name
            == "小明的工作站");

        // 上边界：恰好 64 字节合法；末次再改值为 DB 持久化断言锚点。
        REQUIRE(host.set_device_name(std::string(64, 'n')));
        host.quiesce();
        aki::app::AppState bounded;
        REQUIRE(host.load_state_snapshot(bounded));
        REQUIRE(bounded.devices.devices.front().display_name
            == std::string(64, 'n'));
        REQUIRE(host.set_device_name("aki-renamed"));
        host.quiesce();
        aki::app::AppState final_named;
        REQUIRE(host.load_state_snapshot(final_named));
        REQUIRE(final_named.devices.devices.front().display_name
            == "aki-renamed");
    }

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
    REQUIRE_FALSE(report.language_write_failed);
    {
        std::ifstream preference(data_root / "ui-language.txt");
        std::string code;
        REQUIRE(preference >> code);
        CHECK(code == "en");
    }

    // M5-16：已 shutdown 后 set_device_name 显式拒绝。
    REQUIRE_FALSE(host.set_device_name("post-shutdown-name"));

    // M5-16 重启语义：写路径已零丢失结算（上方 db_completed == 写路径计数），
    // 同一数据根重开启动恢复（第二次 open：0 迁移步），恢复出的本机行
    // display_name 必须是末次合法改名值（make_device_name_job →
    // set_display_name 列落库）。
    {
        auto reopened = aki::persistence::perform_startup_recovery(
            data_root.string());
        REQUIRE(reopened.diagnostics.migrations_applied == 0);
        const aki::device::DeviceIdentity* recovered_local = nullptr;
        for (const auto& device : reopened.state.devices) {
            if (device.id.value == assembly.local_device_id) {
                recovered_local = &device;
                break;
            }
        }
        REQUIRE(recovered_local != nullptr);
        REQUIRE(recovered_local->display_name == "aki-renamed");
        REQUIRE(reopened.state.conversations.size() == 1);
        REQUIRE(reopened.state.conversations.front().id == restored.id);
        REQUIRE(reopened.state.conversations.front().state == restored.state);
    }

    // 幂等：重复关闭返回同一报告。
    REQUIRE(&host.shutdown_with_report() == &report);
    REQUIRE(host.shutdown_completed());

    // M5-11：观察管道随关闭钩子 ③.5 停止（peer_pipeline.stop → running 消失）。
    REQUIRE_FALSE(host.peer_observation_running());

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

// ---- M5-11：启动恢复播种策略（纯函数，不触碰 HostRuntime 进程单例）----

TEST_CASE("seeded_app_state drops recovered Unknown devices and preserves the rest",
    "[unit][host_runtime][seeding][m5_11]") {
    using aki::device::DeviceId;
    using aki::device::DeviceIdentity;
    using aki::device::PresenceState;
    using aki::device::TrustState;

    // 恢复行：五个信任态各一行（历史扫描残留 Unknown 打头）。
    auto make_row = [](std::string id, TrustState trust) {
        DeviceIdentity device;
        device.id = DeviceId{std::move(id)};
        device.display_name = device.id.value;
        device.trust_state = trust;
        // §11.1 ①：恢复行 presence 一律 Offline（易失，不跨会话恢复）。
        device.presence = PresenceState::Offline;
        return device;
    };
    aki::persistence::RecoveredData data;
    data.devices.push_back(make_row("scan-residue", TrustState::Unknown));
    data.devices.push_back(make_row("dev-pending", TrustState::Pending));
    data.devices.push_back(make_row("dev-trusted", TrustState::Trusted));
    data.devices.push_back(make_row("dev-rejected", TrustState::Rejected));
    data.devices.push_back(make_row("dev-revoked", TrustState::Revoked));

    // 其他域各一行：会话/消息/传输不受设备播种过滤影响（各自 FK 语义）。
    aki::conversation::Conversation conversation;
    conversation.id = aki::conversation::ConversationId{"conv-1"};
    conversation.local_device = DeviceId{"local"};
    conversation.remote_device = DeviceId{"dev-trusted"};
    data.conversations.push_back(conversation);

    aki::conversation::Message message;
    message.id = aki::conversation::MessageId{"msg-1"};
    message.sender = DeviceId{"dev-trusted"};
    message.receiver = DeviceId{"local"};
    message.type = aki::conversation::MessageType::Text;
    message.state = aki::conversation::DeliveryState::Delivered;
    data.messages.push_back(message);

    aki::transfer::Transfer transfer;
    transfer.id = aki::transfer::TransferId{"t-1"};
    transfer.sender = DeviceId{"local"};
    transfer.receiver = DeviceId{"dev-trusted"};
    transfer.file.name = "model.gguf";
    transfer.total = 128;
    transfer.state = aki::transfer::TransferState::Completed;
    data.transfers.push_back(transfer);

    const auto seeded = aki::app::seeded_app_state(data);

    // Unknown 历史残留不播种；其余四态原值原序通过。
    REQUIRE(seeded.devices.devices.size() == 4);
    REQUIRE(seeded.devices.devices[0].id == DeviceId{"dev-pending"});
    REQUIRE(seeded.devices.devices[0].trust_state == TrustState::Pending);
    REQUIRE(seeded.devices.devices[1].id == DeviceId{"dev-trusted"});
    REQUIRE(seeded.devices.devices[1].trust_state == TrustState::Trusted);
    REQUIRE(seeded.devices.devices[2].id == DeviceId{"dev-rejected"});
    REQUIRE(seeded.devices.devices[2].trust_state == TrustState::Rejected);
    REQUIRE(seeded.devices.devices[3].id == DeviceId{"dev-revoked"});
    REQUIRE(seeded.devices.devices[3].trust_state == TrustState::Revoked);

    // 其他域整域保留（深拷贝原值，行数与关键字段逐项一致）。
    REQUIRE(seeded.conversations.conversations.size() == 1);
    REQUIRE(seeded.conversations.conversations.front().id
        == aki::conversation::ConversationId{"conv-1"});
    REQUIRE(seeded.messages.messages.size() == 1);
    REQUIRE(seeded.messages.messages.front().id
        == aki::conversation::MessageId{"msg-1"});
    REQUIRE(seeded.messages.messages.front().state
        == aki::conversation::DeliveryState::Delivered);
    REQUIRE(seeded.transfers.transfers.size() == 1);
    REQUIRE(seeded.transfers.transfers.front().id
        == aki::transfer::TransferId{"t-1"});
    REQUIRE(seeded.transfers.transfers.front().state
        == aki::transfer::TransferState::Completed);
}

TEST_CASE("seeded_app_state keeps an empty recovery clean", "[unit][host_runtime][seeding][m5_11]") {
    const auto seeded = aki::app::seeded_app_state(aki::persistence::RecoveredData{});
    REQUIRE(seeded.devices.devices.empty());
    REQUIRE(seeded.conversations.conversations.empty());
    REQUIRE(seeded.messages.messages.empty());
    REQUIRE(seeded.transfers.transfers.empty());
}

// ---- M5-16：LanNameBeacon::set_name 广播名校验（纯逻辑面：不 start、无
// socket/timer；executor 引用仅由构造存储、未启动不会被使用——宿主单例在
// 上一个用例已受控关闭，引用仍有效；与 host_runtime::set_device_name 同一
// valid_lan_name 校验口径）。
TEST_CASE("lan name beacon set_name validates without starting",
    "[unit][host_runtime][lan_name][m5_16]") {
    auto identity = ::heyaki::create_identity();
    REQUIRE(identity.has_value());
    aki::heyaki::LanNameBeacon beacon(HostRuntime::instance().executor(),
        std::move(*identity.value_if()), "aki-beacon", {});
    REQUIRE(beacon.set_name("rename-ok"));
    REQUIRE_FALSE(beacon.set_name(""));
    REQUIRE_FALSE(beacon.set_name(std::string(65, 'b')));
    REQUIRE_FALSE(beacon.set_name("bad\nname"));
    REQUIRE_FALSE(beacon.set_name("bad\x01" "name"));
    // 非法拒绝不锁死改名路径：随后仍可合法更新。
    REQUIRE(beacon.set_name("小明"));
}

int main(int argc, char* argv[]) {
    // 本进程唯一 Executor owner 是 HostRuntime 单例（AGENTS 规则 7/8；GUI 宿主
    // onShutdown 薄委托的同一编排在此直接验证）。函数级 static 的析构在
    // main 返回后执行：受控关闭已在用例内完成，析构为已停状态的空 teardown。
    return Catch::Session().run(argc, argv);
}

TEST_CASE("Startup restores archive availability without accepting paths outside its root",
    "[unit][host_runtime][local_media]") {
    const auto root = std::filesystem::temp_directory_path()
        / ("aki-archive-seed-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "files" / "image");
    { std::ofstream out(root / "files" / "image" / "test.png"); out << "image"; }
    aki::persistence::RecoveredData data;
    aki::transfer::Transfer row;
    row.id = aki::transfer::TransferId{"image"};
    row.state = aki::transfer::TransferState::Completed;
    data.transfers.push_back(row);
    data.local_files.push_back({row.id, {"files/image/test.png", std::string(64, 'a'), 5}});
    auto seed = aki::app::seeded_app_state(data, root.string());
    REQUIRE(seed.transfers.local_artifacts[0].available);
    std::filesystem::remove(root / "files" / "image" / "test.png");
    seed = aki::app::seeded_app_state(data, root.string());
    REQUIRE_FALSE(seed.transfers.local_artifacts[0].available);
    data.local_files[0].second.relative_path = "files/image/../../secret";
    REQUIRE_THROWS(aki::app::seeded_app_state(data, root.string()));
    std::filesystem::remove_all(root);
}
