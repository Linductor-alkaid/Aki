// Aki console 宿主驱动（M5-02：根 aki exe 迁移 GUI 后的 smoke 承载者）。
//
// 语义自 M1-06 console 冒烟宿主零迁移损失（M3-08 真实 Adapter 形态）：
// HostRuntime 公开面（装配报告/发现启停/quiesce/受控关闭报告/快照）+
// 重启恢复断言（persistence 公开 API 直接复核）。argv 契约沿旧宿主：
// argv[1] 数据根基址（缺省 resolve_data_root()）、argv[2] == "--exact" 时
// argv[1] 原样使用（不建子目录、不清理）——损坏 DB 干净失败用例依赖该模式。
// 输出标记不变："aki 0.1.0"（skeleton.app_runs 正则）与 "smoke: PASS/FAIL"
//（smoke.device_lifecycle 正则）。
#include "app/lifecycle/host_runtime.hpp"
#include "app/state/app_state.hpp"
#include "app/state/app_state_owner.hpp"
#include "heyaki/adapter/local_identity.hpp"
#include "persistence/database/database.hpp"
#include "persistence/recovery/startup_recovery.hpp"
#include "persistence/storage/data_root.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace std::chrono_literals;

int g_checks_failed = 0;
int g_run_counter = 0;

void report(bool ok, const std::string& what) {
    if (ok) {
        std::printf("    [ok]   %s\n", what.c_str());
    } else {
        ++g_checks_failed;
        std::printf("    [FAIL] %s\n", what.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    // argv 契约沿旧 console 宿主（M2-07 起）：argv[1] 数据根基址，argv[2] ==
    // "--exact" 为诊断钩子——argv[1] 作为数据根原样使用（不建子目录、不自动
    // 清理）；损坏 DB 干净失败用例与手动复跑真实数据根依赖该模式。
    const bool exact_root = argc > 2 && std::string(argv[2]) == "--exact";
    const std::string base = argc > 1
        ? std::string(argv[1])
        : aki::persistence::resolve_data_root();
    std::string run_root;
    if (exact_root) {
        run_root = base;
    } else {
        const auto now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        run_root = base + "/run-" + std::to_string(now_ns) + "-"
            + std::to_string(++g_run_counter);
    }

    // 本进程唯一 Executor owner 为 HostRuntime 单例（AGENTS 规则 7/8；设计
    // §8.2/§8.3/§9.1）：受控关闭经 shutdown_with_report 显式完成。
    aki::app::HostRuntime& host = aki::app::HostRuntime::instance();
    std::printf(
        "aki 0.1.0 (M5 host smoke: HostRuntime composition root - real"
        " adapter + persistence restart recovery)\n");
    std::printf("devices: this host (real heyaki identity)\n");
    std::printf("data root: %s\n", run_root.c_str());

    const auto& assembly = host.ensure_assembled(run_root);
    if (!assembly.ok) {
        // 干净失败（M2-07 验收 ②）：输出原因 + 非零退出，不静默。
        std::printf("[FATAL] startup recovery failed: %s\n",
            assembly.failure_reason.c_str());
        (void)host.shutdown_with_report();
        return 2;
    }
    std::printf("recovery: %zu device(s), %zu conversation(s), %zu message(s),"
        " %zu transfer(s), %zu migration step(s), %zu tmp orphan(s) removed\n",
        assembly.recovered_devices, assembly.recovered_conversations,
        assembly.recovered_messages, assembly.recovered_transfers,
        assembly.migrations_applied, assembly.tmp_orphans_removed);
    std::printf("local identity: %s (%s), device id %.16s...\n",
        assembly.identity_created ? "created" : "loaded",
        aki::heyaki::kAkiApplicationId, assembly.local_device_id.c_str());
    std::printf("node session: creating (borrowed runtime)\n");
    if (!assembly.lan_interfaces) {
        std::printf(
            "node session: no LAN interface (presence idle this run)\n");
    }

    // ---- 真实链路段（M3-08 宿主切换语义保持）----
    std::printf("\n[real discovery] start/stop (observation pipeline)\n");
    host.quiesce();  // 本地身份 UpsertDevice 落库（经处理器入队）。
    report(host.start_discovery_observation(),
        "start_discovery(LanDiscovery) accepted (real pipeline)");
    host.quiesce();
    report(host.discovery_observation_running(),
        "real discovery pipeline running");
    host.stop_discovery_observation();
    host.quiesce();
    report(!host.discovery_observation_running(),
        "real discovery pipeline stopped");

    // 交互链路降级证据（工程规范 4.3/7：不冒充已验证）。
    std::printf(
        "[degraded] two-end interaction (discover/pair/text/recover) not "
        "verifiable behind the local firewall; covered by integration "
        "loopback binaries with [skip] + rerun conditions\n");

    // 关闭前最终权威快照（session B 逐域一致性断言的期望值；presence 为易失
    // 状态，恢复后默认 Offline——§11.1 ①）。
    aki::app::AppState expected;
    {
        aki::app::AppState snapshot;
        report(host.load_state_snapshot(snapshot), "final snapshot readable");
        expected = std::move(snapshot);
        for (auto& device : expected.devices.devices) {
            device.presence = aki::device::PresenceState::Offline;
        }
        report(expected.devices.devices.size() == 1
                && expected.devices.devices.front().id.value
                    == assembly.local_device_id,
            "device store holds exactly the local identity");
    }

    // ---- 受控关闭（§8.3 钩子原序 → EXEC-01 步骤 2~5；GUI onShutdown 薄委托
    //      的同一编排，设计 §9.1）----
    std::printf("\n[controlled shutdown]\n");
    const auto& shutdown = host.shutdown_with_report();
    report(shutdown.hook_sequence_completed,
        "8.3 hook sequence completed (cancel->flush->stop delivery->pipelines"
        "->node->close->db drain)");
    report(shutdown.executor_report.fully_stopped(),
        "shutdown fully_stopped (Completed + lifecycle Stopped +"
        " wait_timeout_count==0)");
    report(shutdown.executor_report.blocking_workers_requested == 2
            && shutdown.executor_report.blocking_workers_stopped == 2,
        "two blocking workers requested and stopped (db + transfer-io,"
        " handles owned by ExecutorOwner)");
    report(shutdown.state_owner_closed, "AppStateOwner closed");
    report(shutdown.db_drained_within_budget,
        "database worker drained within budget");
    report(!shutdown.db_budget_exhausted, "drain budget not exhausted");
    report(shutdown.node_stopped && shutdown.runtime_stopped,
        "node session stopped (Node + borrowed Runtime)");
    report(!shutdown.borrowed_runtime_shutdown_performed,
        "borrowed runtime did not shut the host executor down (DEC-006)");
    report(!shutdown.runtime_drain_timed_out,
        "runtime drain completed without timeout");
    report(shutdown.write_admitted > 0
            && shutdown.db_completed == shutdown.write_admitted,
        "db jobs completed == admitted (" + std::to_string(shutdown.db_completed)
            + " == " + std::to_string(shutdown.write_admitted)
            + ", zero loss)");
    report(shutdown.db_failed == 0, "no failed db jobs");
    report(shutdown.write_enqueue_rejected == 0 && shutdown.db_rejected == 0,
        "no enqueue rejections");
    report(shutdown.post_accept_failures == 0,
        "no post-accept handler failures");
    report(shutdown.write_settle_failures == 0,
        "all admitted db jobs settled successfully");

    // ---- 重启恢复（session B）：重新 open 后逐域断言一致（SCOPE-09/01）----
    std::printf("\n[restart] reopen data root and assert per-domain consistency\n");
    aki::persistence::RecoveryResult reopened;
    aki::heyaki::LocalIdentity reopened_identity;
    try {
        reopened = aki::persistence::perform_startup_recovery(run_root);
        reopened_identity = aki::heyaki::provision_local_identity(run_root);
    } catch (const std::exception& error) {
        report(false, std::string("reopen recovery failed: ") + error.what());
    }
    if (reopened.store != nullptr) {
        report(reopened.diagnostics.migrations_applied == 0,
            "migration idempotent on reopen (0 steps applied)");
        report(reopened.diagnostics.tmp_orphans_removed == 0,
            "no tmp orphans after clean shutdown");
        // 本地身份二次加载（SCOPE-01 验收 ②）：DeviceId/公钥逐字节一致。
        report(reopened_identity.id.value == assembly.local_device_id,
            "local identity DeviceId stable across restart");
        report(!reopened_identity.created,
            "second boot loads the existing profile (no new identity)");
        report(reopened.state.devices == expected.devices.devices,
            "devices consistent after restart (incl. local identity row,"
            " presence recovered Offline)");
        report(reopened.state.conversations
                == expected.conversations.conversations,
            "conversations consistent after restart");
        report(reopened.state.messages == expected.messages.messages,
            "messages consistent after restart (incl. delivery final state)");
        report(reopened.state.transfers == expected.transfers.transfers,
            "transfer history consistent after restart");
        // 无对端交互（防火墙受限）：无传输/文件本体。
        report(reopened.state.transfers.empty(),
            "no transfers without a paired peer (single-host smoke)");
        if (reopened.repositories != nullptr) {
            aki::persistence::Statement local_row =
                reopened.repositories->database.prepare(
                    "SELECT trust_state FROM device WHERE device_id = ?1;");
            local_row.bind(1, assembly.local_device_id);
            const bool has_row = local_row.step();
            report(has_row
                    && static_cast<int>(local_row.column_int64(0))
                        == static_cast<int>(aki::device::TrustState::Unknown),
                "local identity row consistent (trust_state as registered)");
        }
        // 播种对接复验（§11.1 ②）：恢复结果构造 AppStateOwner 初始快照立即可读。
        aki::app::AppState reopened_state;
        reopened_state.devices.devices = reopened.state.devices;
        reopened_state.conversations.conversations =
            reopened.state.conversations;
        reopened_state.messages.messages = reopened.state.messages;
        reopened_state.transfers.transfers = reopened.state.transfers;
        aki::app::AppStateOwner reopened_owner{aki::app::AppStateOwnerOptions{},
            reopened_state};
        executor::comm::Snapshot<aki::app::AppState> seeded;
        bool seeded_readable = false;
        for (int attempt = 0; attempt < 64 && !seeded_readable; ++attempt) {
            seeded_readable = reopened_owner.try_load_snapshot(seeded);
        }
        report(seeded_readable
                && seeded.value.devices.devices.size()
                    == expected.devices.devices.size()
                && seeded.value.messages.messages.size()
                    == expected.messages.messages.size()
                && seeded.value.transfers.transfers.size()
                    == expected.transfers.transfers.size(),
            "AppStateOwner seeded from recovery (initial snapshot readable)");
    }

    std::printf(
        "\nsmoke: %s (%d check(s) failed)\n",
        g_checks_failed == 0 ? "PASS" : "FAIL", g_checks_failed);
    const int exit_code = g_checks_failed == 0 ? 0 : 1;
    if (exit_code == 0 && !exact_root) {
        std::error_code ec;
        std::filesystem::remove_all(run_root, ec);  // 成功清理；失败保留诊断。
    }
    return exit_code;
}
