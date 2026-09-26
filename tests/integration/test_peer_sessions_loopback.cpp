// M3-06：Presence 与连接路径回环集成测试（DEC-006 映射 5；SCOPE-10）。
//
// authenticated 会话经 peer_sessions diff 管道合成 connected 事件 →
// 双扇出（DM SetPresence Online + CM UpsertConversation Active，经 RouterSink
// 语义的 owner 直接投递）；连接路径映射 LatestMailbox 单值语义（SetConnection
// Path 覆盖式）；不持久化（SetPresence/SetDeviceConnectionPath 无 DB 作业，重启后
// presence 归一化 Offline）。握手被拦环境按 M3-04 先例降级。
//
// 二进制边界（2026-09-24 评审拆分，工程规范第 7 节）：本用例原追加于
// test_discovery_pairing.cpp（M3-04 用例含 [skip] 受控退出）同二进制——
// 多个各自可 std::_Exit(0) 的用例互相掩盖既有失败、且执行顺序随机使回环
// 证据不可达（实测）；拆分为独立单用例二进制后，REQUIRE 失败即中止当前
// 用例（Catch2 语义），[skip] 受控退出点只在前置断言全部通过时可达。
// 映射/diff 网络无关断言见 tests/unit/test_peer_sessions_pipeline.cpp。
#include "app/lifecycle/executor_owner.hpp"
#include "app/state/app_state_owner.hpp"
#include "heyaki/adapter/lan_discovery.hpp"
#include "heyaki/adapter/local_identity.hpp"
#include "heyaki/adapter/peer_sessions_pipeline.hpp"
#include "heyaki/session/runtime_node.hpp"
#include "persistence/database/database_worker.hpp"
#include "persistence/database/database_worker_adapter.hpp"
#include "persistence/recovery/startup_recovery.hpp"
#include "persistence/repository/update_jobs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;

using aki::app::ExecutorOwner;
using aki::app::ExecutorOwnerOptions;
using aki::app::UpsertDevice;
using aki::device::DeviceId;
using aki::device::DeviceIdentity;
using aki::device::PresenceState;
using aki::device::TrustState;
using aki::heyaki::LanDiscoveryPipeline;
using aki::heyaki::LocalProfile;
using aki::heyaki::NodeSession;
using aki::persistence::RecoveryResult;

int g_counter = 0;

std::string temp_root(const std::string& tag) {
    auto path = std::filesystem::temp_directory_path()
        / ("aki-pairing-test-" + tag + "-"
            + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())
            + "-" + std::to_string(++g_counter));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return path.string();
}

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

DeviceIdentity identity_of(const aki::heyaki::LocalIdentity& identity,
    TrustState trust) {
    DeviceIdentity device;
    device.id = identity.id;
    device.display_name = "peer-" + identity.id.value.substr(0, 8);
    device.public_key = identity.public_key;
    device.trust_state = trust;
    device.presence = PresenceState::Offline;
    return device;
}

// 节点域（测试形态）：独立 profile + 独立测试 ExecutorOwner + 借用 Runtime。
// pinned heyaki 的 blocking worker 以 runtime 内部注册为准，同一 executor 上
// 的第二个借用 Runtime 无法启动 asio worker（asio_worker_start_failed，实测，
// worker_name 区分亦不解决）；heyaki 自身双节点测试同样按每节点一套运行时
// 隔离。DEC-006 的借用语义不变：每个 runtime 仍借用其宿主 executor（无
// create_owned / runtime=nullptr），宿主即测试自身的显式 owner。
struct NodeDomain {
    explicit NodeDomain(const std::string& root)
        : profile(LocalProfile::open(root)),
          executor_options([] {
              aki::app::ExecutorOwnerOptions options;
              options.executor_config.min_threads = 2;
              options.executor_config.max_threads = 6;
              return options;
          }()),
          owner(executor_options) {
        // 借用前置：宿主 executor 已 Running（ExecutorOwner.initialize()），
        // 否则 create_borrowed 以 borrowed_executor_not_running 拒绝。
        if (!owner.initialize()) {
            throw std::runtime_error(
                "node domain: executor initialize failed");
        }
        session.emplace(
            NodeSession::create(owner.executor(), {.profile = &profile}));
    }

    LocalProfile profile;
    aki::app::ExecutorOwnerOptions executor_options;
    ExecutorOwner owner;
    std::optional<NodeSession> session;
};

}  // namespace

TEST_CASE("Peer session pipeline drives presence and path state over the loopback",
    "[integration][discovery_pairing][scope10]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());

    const std::string root_a = temp_root("ps-a");
    const std::string root_b = temp_root("ps-b");
    NodeDomain domain_a(root_a);
    NodeDomain domain_b(root_b);
    auto& side_a = *domain_a.session;
    auto& side_b = *domain_b.session;
    const auto identity_a = domain_a.profile.identity();
    const auto identity_b = domain_b.profile.identity();
    if (!side_a.has_lan_interfaces() || !side_b.has_lan_interfaces()) {
        std::printf("[skip] no LAN interface\n");
        std::fflush(nullptr);
        std::_Exit(0);
    }

    // 不持久化断言的载体：DB 控制面 + 处理器（DEC-009 ①）。SetPresence/
    // SetDeviceConnectionPath 在映射中无作业（§11.1 ①）——统计最终对账。
    auto control = std::make_shared<aki::persistence::DatabaseWorkerControl>(
        std::make_unique<aki::persistence::Repositories>(
            aki::persistence::Database::open(":memory:")));
    executor::BlockingWorkerSpec worker_spec;
    worker_spec.name = "aki.db-worker";
    worker_spec.config.thread_name = "aki-db-worker";
    worker_spec.worker =
        std::make_unique<aki::persistence::DatabaseWorkerRunnable>(control);
    REQUIRE(owner.start_blocking_worker(std::move(worker_spec)));
    control->mark_registered();

    aki::app::AppStateOwnerOptions state_options;
    std::uint64_t device_jobs = 0;
    aki::app::AppStateOwner state_owner{state_options,
        aki::app::AppState{},
        [&](const aki::app::AppStateUpdate& update) {
            if (std::holds_alternative<aki::app::UpsertDevice>(update)) {
                auto job = aki::persistence::make_device_upsert_job(
                    std::get<aki::app::UpsertDevice>(update).device);
                auto future = job.done->get_future();
                REQUIRE(control->enqueue(std::move(job)));
                future.get();
                ++device_jobs;
            }
            // SetPresence / SetDeviceConnectionPath：无作业（§11.1 ①）。
        }};

    // 发现 + 连接 + 配对（沿 M3-04；握手被拦 → 降级退出）。
    LanDiscoveryPipeline discovery(owner.executor(), side_a,
        [](const aki::device::DiscoveredDevice& device) {
            (void)device;
        });
    REQUIRE(discovery.start(200ms));
    REQUIRE(wait_until([&] {
        for (const auto& entry : side_a.endpoints()) {
            if (entry.device_id == identity_b.id) return true;
        }
        return false;
    }, 15s));
    REQUIRE(side_a.connect_lan(identity_b.id));
    const bool restricted = wait_until(
        [&] { return side_a.session_pairing_restricted(identity_b.id); },
        15s);
    if (!restricted) {
        std::printf(
            "[skip] pairing handshake blocked (firewall): presence/path "
            "loopback not verified; rerun with inbound TCP allowed\n");
        std::fflush(nullptr);
        std::_Exit(0);
    }
    REQUIRE(state_owner.submit_update(UpsertDevice{
        identity_of(identity_b, TrustState::Pending)}));
    std::atomic<bool> paired{false};
    side_a.set_pairing_observer(
        [&](const DeviceId& peer, bool ok, const std::string&) {
            if (ok && peer == identity_b.id) paired.store(true);
        });
    side_b.set_pairing_observer(
        [&](const DeviceId&, bool ok, const std::string&) {
            if (ok) paired.store(true);
        });
    REQUIRE(side_a.pair_peer(identity_b.id, aki::heyaki::kAkiPairingPassword));
    REQUIRE(side_b.pair_peer(identity_a.id, aki::heyaki::kAkiPairingPassword));
    if (!wait_until([&] { return paired.load(); }, 20s)) {
        // 环境受限降级（沿 M3-04/M3-05 纪律，不冒充已验证）：会话已到
        // pairing_restricted 但握手未在预算内完成——本机防火墙拦截至端
        // TLS / CI 偶发停滞（run 35943203897 asan 实测），presence/path
        // 断言位于其后无法执行。打印会话诊断作为补跑证据；已验证断言
        // （发现/连接/受限/提交）保持完整。
        for (const auto& entry : side_a.peer_session_diagnostics()) {
            std::printf("    [diag] A session peer=%s state=%d restricted=%d\n",
                entry.first.c_str(), entry.second.first, entry.second.second);
        }
        std::printf("[skip] pairing handshake did not complete after "
                    "submission (inbound TLS blocked / CI stall): "
                    "presence/path loopback not verified; rerun with "
                    "inbound TCP allowed\n");
        std::fflush(nullptr);
        // Node::shutdown 在握手停滞会话上阻塞（heyaki 侧行为，M3-04 实测）：
        // 证据已打印，受控退出（借用断言由 DOD-02 用例与主 owner 路径覆盖）。
        std::_Exit(0);
    }
    REQUIRE(state_owner.submit_update(UpsertDevice{
        identity_of(identity_b, TrustState::Trusted)}));
    state_owner.drain();

    // peer_sessions diff 管道（A 侧）：authenticated → connected 事件 →
    // SetPresence Online（DM 语义）。
    std::atomic<int> connected_events{0};
    std::atomic<int> path_events{0};
    aki::heyaki::PeerSessionEvents events;
    events.on_connected =
        [&](const aki::device::DeviceId& peer,
            aki::device::ConnectionPath path) {
            if (peer == identity_b.id) {
                // DM 语义：SetPresence Online + 初连路径（DEC-015）。
                REQUIRE(state_owner.submit_update(
                    aki::app::SetPresence{peer, PresenceState::Online}));
                REQUIRE(state_owner.submit_update(
                    aki::app::SetDeviceConnectionPath{peer, path}));
                ++connected_events;
            }
        };
    events.on_disconnected =
        [&](const aki::device::DeviceId& peer) {
            if (peer == identity_b.id) {
                REQUIRE(state_owner.submit_update(
                    aki::app::SetPresence{peer, PresenceState::Offline}));
            }
        };
    events.on_connection_path_changed =
        [&](const aki::device::DeviceId& peer,
            aki::device::ConnectionPath path) {
            if (peer == identity_b.id) {
                // 逐设备路径 upsert（DEC-015）：随 drain 合并，无事件洪泛。
                REQUIRE(state_owner.submit_update(
                    aki::app::SetDeviceConnectionPath{peer, path}));
                ++path_events;
            }
        };
    aki::heyaki::PeerSessionPipeline pipeline(
        owner.executor(), side_a, std::move(events));
    REQUIRE(pipeline.start(200ms));
    REQUIRE(wait_until([&] { return connected_events.load() >= 1; }, 15s));

    executor::comm::Snapshot<aki::app::AppState> snapshot;
    REQUIRE(state_owner.try_load_snapshot(snapshot));
    bool presence_online = false;
    for (const auto& device : snapshot.value.devices.devices) {
        if (device.id == identity_b.id) {
            presence_online = device.presence == PresenceState::Online;
        }
    }
    REQUIRE(presence_online);

    // 不持久化断言（验收 ③ 半边）：SetPresence/SetDeviceConnectionPath 无 DB 作业
    //——device_jobs 不因 presence/path 事件增长（仅 UpsertDevice 作业计入）。
    const auto device_jobs_at_connected = device_jobs;
    // 路径事件在本回环（lan_only 直连）可能仅一次或零次——不强制出现。
    (void)wait_until([&] { return path_events.load() >= 1; }, 15s);
    REQUIRE(device_jobs == device_jobs_at_connected);

    // 逐设备 upsert 覆盖式（DEC-015）：连续同设备路径更新仅保留最新值。
    REQUIRE(state_owner.submit_update(aki::app::SetDeviceConnectionPath{
        identity_b.id, aki::device::ConnectionPath::P2p}));
    REQUIRE(state_owner.submit_update(aki::app::SetDeviceConnectionPath{
        identity_b.id, aki::device::ConnectionPath::Relay}));
    state_owner.drain();
    executor::comm::Snapshot<aki::app::AppState> paths;
    REQUIRE(state_owner.try_load_snapshot(paths));
    bool latest_seen = false;
    for (const auto& entry : paths.value.devices.connection_paths) {
        if (entry.device == identity_b.id) {
            latest_seen = entry.path == aki::device::ConnectionPath::Relay;
        }
    }
    REQUIRE(latest_seen);  // 仅最新

    // 停止后零事件（TimerHandle 取消生效）。
    pipeline.stop();
    REQUIRE_FALSE(pipeline.running());
    const auto connected_before = connected_events.load();
    std::this_thread::sleep_for(800ms);
    REQUIRE(connected_events.load() == connected_before);

    // RULE-06/RULE-08：重复 connected 事件（同会话）仅更新 presence——会话
    // 记录与信任终态不复活/不新建（owner 侧状态机校验）。
    REQUIRE(state_owner.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.devices.devices.size() == 1);

    // 关闭（借用断言沿 M3-04）。
    pipeline.stop();
    discovery.stop();
    const auto node_report = side_a.shutdown();
    REQUIRE(node_report.node_stopped);
    REQUIRE_FALSE(node_report.runtime_executor_shutdown_performed);
    (void)side_b.shutdown();
    (void)domain_a.owner.shutdown();
    (void)domain_b.owner.shutdown();

    // 不持久化（验收 ③）：重启恢复后 presence 归一化 Offline。
    RecoveryResult reopened = aki::persistence::perform_startup_recovery(root_a);
    REQUIRE(reopened.state.devices.size() == 1);
    REQUIRE(reopened.state.devices[0].id == identity_b.id);
    REQUIRE(reopened.state.devices[0].presence == PresenceState::Offline);
    REQUIRE(reopened.state.devices[0].trust_state == TrustState::Trusted);

    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}
