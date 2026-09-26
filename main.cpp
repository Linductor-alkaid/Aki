// Aki console 冒烟宿主（M1-06 起为设计第 8.3 节组合根的最小进程内实现；
// M3-08 起切换为真实 Adapter（HeyakiNodeAdapter，DEC-006 全映射）：本地
// 真实身份 + LAN 发现观察管道 + DEC-009 写路径 + 持久化重启恢复。防火墙
// 受限环境的双端交互链路由回环集成测试承载（[skip]+补跑条件，见 M3 记录）。
//
// 进程内 Executor owner 自 M1-06 起为正式 ExecutorOwner（设计第 8.2 节落点说明，
// AGENTS 规则 7/8）：Manager 排空泵、传输会话与 DatabaseWorker（blocking worker，
// EXEC-04）全部任务都在其生命周期内，无 std::thread / std::async / 自建线程
// （RULE-07）。
//
// M3-03 组合顺序（设计第 8.3 节七步 + 第 11.1 节 ②③，DEC-009）：
//   1 ExecutorOwner.initialize() → 2 主线程同步启动恢复（解析数据根 → open
//   （损坏即干净失败输出原因退出）→ user_version 迁移 → 四仓储逐域加载 →
//   files/tmp/ 清扫 → 播种数据）+ 本地身份供给（ProfileStore create-or-open，
//   heyaki/adapter/local_identity.hpp，RULE-10：公开面仅 aki/std 类型）→
//   3 DatabaseWorkerControl（锚定恢复移交的单一连接）→ 4 AppStateOwner（构造
//   入参：初始快照 + 接受后处理器——DEC-009 ① 正式落点，owner 单写者上下文
//   按接受顺序入队 DB 作业；M2-07 镜像形态移除，两者不并存）→ 5 四 Manager →
//   6 start_blocking_worker + mark_registered → 7 RouterSink（EXEC-02 启动段
//   纪律）→ 演示场景（发现/信任/文本/传输历史/断开/重连）→ 受控关闭（钩子
//   末尾 AppStateOwner.close() 之后排空 DatabaseWorker，§11.1 ③）→ 重开恢复
//   逐域断言一致 + 本地身份二次加载逐字节一致（SCOPE-01/09）。
//
// 演示脚本（M3-08 宿主切换后）：本机真实 heyaki 身份 + LAN 发现观察管道
// 启停 + 持久化重启恢复（本地行逐域一致）。双端交互链路（发现对端/配对/
// 收发/断线恢复）由回环集成测试承载（防火墙受限环境 [skip]+补跑条件，
// 沿 M3-04 登记项）。FakeHeyakiAdapter 保留用于单元测试（DEC-002/M1 纪律）。
//
// 确定性：主线程串行驱动 + 每步 flush（有界预算）+ owner drain 推进到静止
// 后断言；数据根为每次运行独立子目录（默认基址 resolve_data_root()，
// argv[1] 可覆盖），同一可执行文件连续多次运行输出一致；成功后清理运行
// 目录，失败保留供诊断。
#include "app/application/router_sink.hpp"
#include "app/lifecycle/executor_owner.hpp"
#include "app/state/app_state_owner.hpp"
#include "heyaki/adapter/heyaki_node_adapter.hpp"
#include "heyaki/adapter/fake_heyaki_adapter.hpp"  // M4 前仅测试目标使用
#include "heyaki/adapter/local_identity.hpp"
#include "heyaki/adapter/peer_sessions_pipeline.hpp"
#include "app/application/reconnect_loop.hpp"
#include "heyaki/session/runtime_node.hpp"
#include "persistence/database/database.hpp"
#include "persistence/database/database_worker.hpp"
#include "persistence/database/database_worker_adapter.hpp"
#include "persistence/recovery/startup_recovery.hpp"
#include "persistence/repository/update_jobs.hpp"
#include "persistence/storage/data_root.hpp"
#include "persistence/storage/file_jobs.hpp"
#include "persistence/storage/file_store.hpp"
#include "persistence/storage/sha256.hpp"
#include "persistence/storage/transfer_io_worker.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;

using aki::app::AppEvent;
using aki::app::AppState;
using aki::app::AppStateOwner;
using aki::app::AppStateOwnerOptions;
using aki::app::AppStateUpdate;
using aki::app::ConnectionPathChangedEvent;
using aki::app::ConversationManager;
using aki::app::ConversationManagerOptions;
using aki::app::DeviceConnectedEvent;
using aki::app::DeviceDiscoveredEvent;
using aki::app::DeviceDisconnectedEvent;
using aki::app::DeviceManager;
using aki::app::ExecutorOwner;
using aki::app::ExecutorOwnerOptions;
using aki::app::ManagerPumpOptions;
using aki::app::MessageDeliveredEvent;
using aki::app::MessageManager;
using aki::app::MessageManagerOptions;
using aki::app::MessageReceivedEvent;
using aki::app::ReconnectCoordinator;
using aki::app::ReconnectCoordinatorOptions;
using aki::app::RouterSink;
using aki::app::TransferManager;
using aki::app::TransferManagerOptions;
using aki::app::UpsertDevice;

using aki::conversation::ConversationId;
using aki::conversation::ConversationState;
using aki::conversation::DeliveryState;
using aki::conversation::Message;
using aki::conversation::MessageId;
using aki::conversation::MessageType;
using aki::conversation::TextPayload;
using aki::device::ConnectionPath;
using aki::device::DeviceClass;
using aki::device::DeviceId;
using aki::device::DeviceIdentity;
using aki::device::DiscoveredDevice;
using aki::device::DiscoveryMethod;
using aki::device::PresenceState;
using aki::device::TrustState;
using aki::heyaki::HeyakiNodeAdapter;
using aki::heyaki::LocalIdentity;
using aki::heyaki::provision_local_identity;
using aki::persistence::DatabaseWorkerControl;
using aki::persistence::DatabaseWorkerOptions;
using aki::persistence::DatabaseWorkerRunnable;
using aki::persistence::DbJob;
using aki::persistence::FileStore;
using aki::persistence::RecoveryResult;
using aki::persistence::perform_startup_recovery;
using aki::transfer::Transfer;
using aki::transfer::TransferId;
using aki::transfer::TransferState;

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

// DoubleBuffer try_load 槽位忙时可重试（消费侧契约，设计第 10.1 节）。
bool load_snapshot(AppStateOwner& state_owner,
    executor::comm::Snapshot<AppState>& out) {
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (state_owner.try_load_snapshot(out)) {
            return true;
        }
    }
    return false;
}

// 有界等待谓词为真（关闭排空用；预算耗尽返回 false，证据不伪造）。
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

// 本地身份 → DeviceIdentity（DEC-006 映射 1）：id = 规范 hex；公钥 32 字节；
// 已恢复的本地行保留其信任状态（状态机不接受 Trusted -> Unknown 的回退），
// 身份自有字段（公钥）以当前身份为准；presence 恢复为 Offline（易失）。
DeviceIdentity make_local_device_identity(const LocalIdentity& identity,
    const std::vector<DeviceIdentity>& recovered_devices) {
    DeviceIdentity device;
    device.id = identity.id;
    device.display_name = "aki";
    device.device_class = DeviceClass::Other;
    device.os_name = "console host";
    device.public_key = identity.public_key;
    device.trust_state = TrustState::Unknown;
    device.presence = PresenceState::Offline;
    for (const auto& recovered : recovered_devices) {
        if (recovered.id == identity.id) {
            device.display_name = recovered.display_name;
            device.device_class = recovered.device_class;
            device.os_name = recovered.os_name;
            device.trust_state = recovered.trust_state;
            device.capabilities = recovered.capabilities;
            device.public_key = identity.public_key;
            break;
        }
    }
    return device;
}

// DEC-009 ①：接受后处理器（owner 单写者上下文按接受顺序同步调用）。作业映射
// 复用 §11.1 ① + update_jobs 工厂；SetPresence/SetConnectionPath 不持久化。
// 入队拒绝经 enqueue_rejected 与 control->rejected_count 双可见（RULE-09）；
// 处理器异常由 owner 全捕获（post_accept_failures），此处不需自兜底。
// UpsertMessage 的会话归属自 M3-05 起随更新载荷携带（DEC-009 ②）。
struct WritePathSink {
    std::shared_ptr<DatabaseWorkerControl> control;
    std::shared_ptr<FileStore> store;
    std::vector<std::future<void>> futures;
    std::uint64_t admitted = 0;
    std::uint64_t enqueue_rejected = 0;

    void operator()(const AppStateUpdate& update) {
        for (DbJob& job : jobs_for(update)) {
            futures.push_back(job.done->get_future());
            if (!control->enqueue(std::move(job))) {
                futures.pop_back();
                ++enqueue_rejected;  // 明确拒绝，不静默（RULE-09）
                continue;
            }
            ++admitted;
        }
    }

private:
    std::vector<DbJob> jobs_for(const AppStateUpdate& update) const {
        std::vector<DbJob> jobs;
        std::visit(
            [this, &jobs](const auto& concrete) {
                using Update = std::decay_t<decltype(concrete)>;
                if constexpr (std::is_same_v<Update, aki::app::UpsertDevice>) {
                    jobs.push_back(
                        aki::persistence::make_device_upsert_job(
                            concrete.device));
                } else if constexpr (std::is_same_v<Update,
                                       aki::app::UpsertConversation>) {
                    jobs.push_back(
                        aki::persistence::make_conversation_upsert_job(
                            concrete.conversation));
                } else if constexpr (std::is_same_v<Update,
                                       aki::app::UpsertMessage>) {
                    jobs.push_back(
                        aki::persistence::make_message_upsert_job(
                            concrete.message, concrete.conversation));
                } else if constexpr (std::is_same_v<Update,
                                       aki::app::UpsertTransfer>) {
                    jobs.push_back(
                        aki::persistence::make_transfer_upsert_job(
                            concrete.transfer));
                } else if constexpr (std::is_same_v<Update,
                                       aki::app::UpdateTransferProgress>) {
                    jobs.push_back(
                        aki::persistence::make_transfer_progress_job(
                            concrete.transfer, concrete.transferred,
                            concrete.total));
                } else if constexpr (std::is_same_v<Update,
                                       aki::app::SetDeliveryState>) {
                    jobs.push_back(
                        aki::persistence::make_message_delivery_job(
                            concrete.message, concrete.state));
                } else if constexpr (std::is_same_v<Update,
                                       aki::app::CompleteTransfer>) {
                    if (concrete.final_state == TransferState::Completed) {
                        // §11.1 ④：终态更新 + 流式 SHA-256 + 原子改名 +
                        // 回写位（作业组自身幂等）。
                        jobs.push_back(
                            aki::persistence::make_transfer_complete_job(
                                store, concrete.transfer.value));
                    } else {
                        // Failed/Cancelled：终态列更新 + .part 幂等删除
                        // （两个串行作业，顺序 = 接受顺序）。
                        jobs.push_back(
                            aki::persistence::make_transfer_terminal_job(
                                concrete.transfer, concrete.final_state));
                        jobs.push_back(
                            aki::persistence::make_transfer_discard_job(
                                store, concrete.transfer.value));
                    }
                }
                // SetPresence / SetConnectionPath：不持久化（§11.1 ①），
                // 无作业。
            },
            update);
        return jobs;
    }
};

AppState app_state_from(const aki::persistence::RecoveredData& data) {
    AppState state;
    state.devices.devices = data.devices;
    state.conversations.conversations = data.conversations;
    state.messages.messages = data.messages;
    state.transfers.transfers = data.transfers;
    return state;
}

int run_demo(const std::string& run_root) {
    std::printf(
        "aki 0.1.0 (M3 smoke: real adapter - local identity + LAN discovery"
        " + persistence restart recovery)\n");
    std::printf("devices: this host (real heyaki identity)\n");
    std::printf("data root: %s\n", run_root.c_str());

    // ---- 组合根（设计第 8.3 节七步 + 第 11.1 节 ②，DEC-009）----
    // 1) ExecutorOwner.initialize()——进程内唯一 Executor 生命周期 owner。
    //    DEC-006 合并负载定容（borrowed Runtime 的线程参数不生效，仅 Aki 侧
    //    可 sizing）：heyaki asio/backend 负载 + Manager 泵 + DB blocking
    //    worker 并入后显式定容。
    ExecutorOwnerOptions executor_options;
    executor_options.executor_config.min_threads = 2;
    executor_options.executor_config.max_threads = 6;
    ExecutorOwner executor_owner{executor_options};
    if (!executor_owner.initialize()) {
        std::printf("[FATAL] ExecutorOwner.initialize() failed\n");
        return 1;
    }
    // 2) 主线程同步启动恢复（§11.1 ②：不经 blocking worker；DB 损坏即干净
    //    失败输出原因退出，不静默）+ 本地身份供给（SCOPE-01，同一恢复段；
    //    profile 常驻供 Node 装配，M3-04）。
    RecoveryResult recovery;
    LocalIdentity identity;
    std::optional<aki::heyaki::LocalProfile> profile_storage;
    try {
        recovery = perform_startup_recovery(run_root);
        profile_storage.emplace(aki::heyaki::LocalProfile::open(run_root));
    } catch (const std::exception& error) {
        std::printf("[FATAL] startup recovery failed: %s\n", error.what());
        // return 2 正常展开：栈上 executor_owner 析构兜底关闭（此时无生产者，
        // 与 main() 契约一致）。不得 std::exit——它不销毁自动对象，兜底析构
        // 与受控关闭都不会执行，executor 线程将存活至静态析构期。
        return 2;
    }
    aki::heyaki::LocalProfile& profile = *profile_storage;
    identity = profile.identity();
    std::printf("recovery: %zu device(s), %zu conversation(s), %zu message(s),"
        " %zu transfer(s), %zu migration step(s), %zu tmp orphan(s) removed\n",
        recovery.state.devices.size(), recovery.state.conversations.size(),
        recovery.state.messages.size(), recovery.state.transfers.size(),
        recovery.diagnostics.migrations_applied,
        recovery.diagnostics.tmp_orphans_removed);
    std::printf("local identity: %s (%s), device id %.16s...\n",
        identity.created ? "created" : "loaded",
        aki::heyaki::kAkiApplicationId, identity.id.value.c_str());

    // 3) DatabaseWorkerControl——先于 AppStateOwner 构造（§8.3 七步序，
    //    DEC-009 装配时序；单一连接整体移交）。
    // 3.5) Node/Runtime 装配（DEC-006 借用注入：borrowed Runtime + Node，
    //      EXEC-02 启动段纪律——恢复完成后、事件源接通前；LAN 发现观察管道
    //      随 M3-08 组合切换接入，本版本宿主 Node 仅常驻公告）。
    std::printf("node session: creating (borrowed runtime)\n");
    auto node_session = std::make_unique<aki::heyaki::NodeSession>(
        aki::heyaki::NodeSession::create(executor_owner.executor(),
            aki::heyaki::NodeSession::Options{.profile = &profile}));
    if (!node_session->has_lan_interfaces()) {
        std::printf(
            "node session: no LAN interface (presence idle this run)\n");
    }

    DatabaseWorkerOptions db_options;
    auto db = std::make_shared<DatabaseWorkerControl>(
        std::move(recovery.repositories), db_options);

    // 3.6) 传输归档 IO worker 控制面（M4-04，DEC-011/§7.1③：发送侧分块
    //      IO 走专用 aki.transfer-io blocking worker——与 DatabaseWorker
    //      同款形态；TM 构造注入承载面，注册在 Manager 之后见步骤 6.5）。
    aki::persistence::TransferIoWorkerOptions io_options;
    auto transfer_io = std::make_shared<aki::persistence::TransferIoControl>(
        recovery.store, io_options);

    // 4) AppStateOwner：以加载结果播种初始快照 + 接受后处理器（DEC-009 ①；
    //    owner 上下文 = 主线程，单写者，RULE-02/EXEC-03）。
    // 全成员显式初始化：GCC -Wmissing-field-initializers（CI Linux -Werror）
    // 对省略尾随成员的聚合初始化告警（CI run 35905251211 实测；MSVC 不告警）。
    WritePathSink sink{db, recovery.store, {}, 0, 0};
    AppStateOwner state_owner{AppStateOwnerOptions{},
        app_state_from(recovery.state),
        [&sink](const AppStateUpdate& update) { sink(update); }};

    // 本地身份经 UpsertDevice 进入 Application State（SCOPE-01）：恢复段已有
    // 本地行则保留其信任状态并刷新身份自有字段；首个权威更新在首次 drain 时
    // 经处理器入队 DEVICE 行（幂等接受同样入队，§11.1 ①）。
    report(state_owner.submit_update(
               UpsertDevice{make_local_device_identity(identity,
                   recovery.state.devices)}),
        "local identity submitted (UpsertDevice via post-accept handler)");

    // 5) 真实 Adapter（M3-08 宿主切换；HeyakiNodeAdapter 组装发现观察管道、
    //    peer_sessions diff 管道、消息面与重连协调器；conversation 解析注入
    //    CM/MM 同方案）；presence/path 观察关闭（smoke 确定性，见 M3-04/06
    //    记录），发现观察管道由 start_discovery 启停。
    HeyakiNodeAdapter adapter{executor_owner.executor(),
        {.profile = &profile,
            .session = node_session.get(),
            .conversation_for =
                [](const DeviceId& remote) {
                    return aki::conversation::ConversationId{
                        std::string{"conv-"} + remote.value};
                },
            .peer_observation = false}};

    ManagerPumpOptions device_pump;
    device_pump.name = "aki.dm";
    DeviceManager devices{executor_owner.executor(), state_owner, adapter,
        device_pump};

    ConversationManagerOptions conversation_options;
    conversation_options.pump.name = "aki.cm";
    ConversationManager conversations{
        executor_owner.executor(), state_owner, conversation_options};

    MessageManagerOptions message_options;
    message_options.pump.name = "aki.mm";
    message_options.local_device = identity.id;  // 出站消息 sender = 本地身份
    MessageManager messages{
        executor_owner.executor(), state_owner, adapter, message_options};

    TransferManagerOptions transfer_options;
    transfer_options.pump.name = "aki.tm";
    transfer_options.sender = identity.id;
    // M4-04（DEC-011/§7.1③）：归档链路承载面注入（事件驱动会话状态机 +
    // 专用 worker 分块 IO；构造即注册事件投递面，worker 注册见步骤 6.5）。
    transfer_options.io = transfer_io.get();
    TransferManager transfers{
        executor_owner.executor(), state_owner, adapter, transfer_options};

    // 6) 注册 DatabaseWorker（恢复完成后；EXEC-07 唯一入口
    //    start_blocking_worker，句柄归 owner）。
    auto db_runnable = std::make_unique<DatabaseWorkerRunnable>(db);
    executor::BlockingWorkerSpec db_spec;
    db_spec.name = "aki.db-worker";
    db_spec.config.thread_name = "aki-db-worker";
    db_spec.worker = std::move(db_runnable);
    if (!executor_owner.start_blocking_worker(std::move(db_spec))) {
        std::printf("[FATAL] DatabaseWorker registration failed\n");
        return 2;
    }
    db->mark_registered();

    // 6.5) 注册 transfer IO worker（M4-04，DEC-011/§11.1③：同 DatabaseWorker
    //      纪律——TM 事件投递面已在其构造时注册（步骤 5 先于本步骤），
    //      worker 启动即有完整 sink；关闭序见钩子（flush 至 IO 归零后由
    //      owner 步骤 2/3 统一回收）。
    auto transfer_io_runnable =
        std::make_unique<aki::persistence::TransferIoRunnable>(
            transfer_io->impl());
    executor::BlockingWorkerSpec transfer_io_spec;
    transfer_io_spec.name = "aki.transfer-io";
    transfer_io_spec.config.thread_name = "aki-transfer-io";
    transfer_io_spec.worker = std::move(transfer_io_runnable);
    if (!executor_owner.start_blocking_worker(std::move(transfer_io_spec))) {
        std::printf("[FATAL] transfer IO worker registration failed\n");
        return 2;
    }

    // 7) RouterSink 注册（EXEC-02 启动段纪律：恢复完成、worker 就位后才接通
    //    事件源）。
    RouterSink router{devices, conversations, messages, transfers};
    adapter.set_sink(&router);

    // 7.5) peer_sessions diff 管道装配（M3-06；DEC-008 双 Manager 扇出经
    //      RouterSink：connected/disconnected → DM presence + CM 会话态，
    //      path 变化 → DM LatestMailbox）。构造不 start：smoke 确定性
    //     （真实 LAN 邻居会进入状态面），启动随 M3-08 组合切换。
    auto reconnect = std::make_unique<aki::app::ReconnectCoordinator>(
        executor_owner.executor(), ReconnectCoordinatorOptions{});
    auto peer_pipeline = std::make_unique<aki::heyaki::PeerSessionPipeline>(
        executor_owner.executor(), *node_session,
        aki::heyaki::PeerSessionEvents{
            .on_connected =
                [&router](const DeviceId& peer) {
                    (void)router.on_device_connected(peer, ConnectionPath::Lan);
                },
            .on_disconnected =
                [&](const DeviceId& peer) {
                    (void)router.on_device_disconnected(peer);
                    // SCOPE-11：断开后启动有界重连循环（EXEC-05 长任务，
                    // DEC-008 承载；恢复后原会话经 connect_lan 回到
                    // authenticated，M3-06 管道 connected 事件回推 Active）。
                    aki::app::ReconnectCoordinator::Attempt try_conn =
                        [node_session_ptr = node_session.get(), peer]() {
                            return node_session_ptr->connect_lan(peer);
                        };
                    aki::app::ReconnectCoordinator::RecoveryCheck is_auth =
                        [node_session_ptr = node_session.get(), peer]() {
                            return node_session_ptr->session_authenticated(
                                peer);
                        };
                    aki::app::ReconnectCoordinator::PerPeerHooks hooks{
                        std::move(try_conn), std::move(is_auth)};
                    (void)reconnect->start(peer, hooks);
                },
            .on_connection_path_changed =
                [&router](const DeviceId& peer, ConnectionPath path) {
                    (void)router.on_connection_path_changed(
                        peer, ConnectionPath::Unknown, path);
                }});

    // 每步推进到静止：flush 四个 Manager（有界预算，消费排空 future）+ 状态
    // owner drain 至水位不变（主线程即 owner 上下文；处理器在 drain 内按接受
    // 顺序入队 DB 作业，DEC-009 ①）。
    const auto quiesce = [&] {
        const bool flushed = devices.flush(2s) && conversations.flush(2s)
            && messages.flush(2s) && transfers.flush(2s);
        if (!flushed) {
            report(false, "manager pumps quiesced");
        }
        for (;;) {
            const auto watermark = [](const AppStateOwner& owner) {
                return owner.stats().updates_applied + owner.stats().updates_rejected
                    + owner.stats().events_forwarded + owner.stats().events_dropped;
            };
            const std::uint64_t before = watermark(state_owner);
            state_owner.drain();
            if (watermark(state_owner) == before) {
                return;
            }
        }
    };

    // ---- 真实链路段（M3-08 宿主切换）----
    // 防火墙受限环境（M3-04 登记项）：对端 TLS 入站被拦 → 双端交互（发现
    // 对端/配对/收发）不可达。宿主 smoke 收敛为可确定性验证的真实路径：
    // 本地身份（已落库）+ 真实发现启停（观察管道启动证据）+ 受控关闭 +
    // 重启恢复；发现→信任→收发交互链路由 test_message_loopback /
    // test_disconnect_recovery_loopback / test_discovery_pairing_loopback
    // 承载（可用环境全链路，受限环境 [skip] + 补跑条件）。
    std::printf("\n[real discovery] start/stop (observation pipeline)\n");
    quiesce();  // 本地身份 UpsertDevice 落库（经处理器入队）。

    report(adapter.start_discovery(DiscoveryMethod::LanDiscovery),
        "start_discovery(LanDiscovery) accepted (real pipeline)");
    quiesce();
    report(adapter.discovery_running(), "real discovery pipeline running");

    adapter.stop_discovery();
    quiesce();
    report(!adapter.discovery_running(), "real discovery pipeline stopped");

    // 交互链路降级证据（工程规范 4.3/7：不冒充已验证）。
    std::printf(
        "[degraded] two-end interaction (discover/pair/text/recover) not "
        "verifiable behind the local firewall; covered by integration "
        "loopback binaries with [skip] + rerun conditions\n");

    // 关闭前最终权威快照（session B 逐域一致性断言的期望值；presence 为易失
    // 状态，恢复后默认 Offline——§11.1 ①）。
    AppState expected;
    {
        executor::comm::Snapshot<AppState> snapshot;
        report(load_snapshot(state_owner, snapshot), "final snapshot readable");
        expected = snapshot.value;
        for (auto& device : expected.devices.devices) {
            device.presence = PresenceState::Offline;
        }
        report(expected.devices.devices.size() == 1
                && expected.devices.devices.front().id == identity.id,
            "device store holds exactly the local identity");
    }

    // ---- 受控关闭（设计第 8.3 节钩子顺序 -> EXEC-01 步骤 2~5）----
    std::printf("\n[controlled shutdown]\n");
    const auto shutdown_report = executor_owner.shutdown([&] {
        // EXEC-01 步骤 1 钩子（设计第 8.3 节顺序 + 第 11.1 节 ③ 排空落点）：
        (void)transfers.request_cancel_all();  // ① 取消在途可取消任务（无在飞时幂等）
        (void)transfers.flush(2s);             // ② flush 各 Manager 至泵静止并消费 future
        (void)devices.flush(2s);
        (void)conversations.flush(2s);
        (void)messages.flush(2s);
        adapter.set_sink(nullptr);             // ③ 停 Adapter 投递
        adapter.stop_discovery();
        // ③.5 Heyaki 生产者停止（DEC-006/§8.3：观察管道停止 + Node::
        //      shutdown + Runtime::shutdown，EXEC-01 步骤 1 内、早于 owner
        //      步骤 2/3/5）。
        peer_pipeline->stop();
        // 重连长任务先取消并消费在途 future（DEC-008/EXEC-01 步骤 1）。
        const auto reconnect_consumed = reconnect->stop_all();
        report(reconnect_consumed > 0 || reconnect->running_count() == 0,
            "reconnect loops cancelled and futures consumed");
        const auto node_report = node_session->shutdown();
        report(node_report.node_stopped && node_report.runtime_stopped,
            "node session stopped (Node + borrowed Runtime)");
        report(!node_report.runtime_executor_shutdown_performed,
            "borrowed runtime did not shut the host executor down (DEC-006)");
        report(!node_report.runtime_drain_timed_out,
            "runtime drain completed without timeout");
        state_owner.close();                   // ④ comm 关闭（主线程 = owner 上下文）
        // ⑤ DB 排空位于钩子序列末尾（close() 之后、EXEC-01 步骤 2/3 之前，
        //    §11.1 ③）：close 的排空让最后一批更新被接受并入队 DB 作业。
        db->request_drain();
        report(wait_until([&] { return db->drain_completed(); }, 3s),
            "database worker drained within budget");
    });
    report(shutdown_report.fully_stopped(),
        "shutdown fully_stopped (Completed + lifecycle Stopped + wait_timeout_count==0)");
    report(shutdown_report.blocking_workers_requested == 2
            && shutdown_report.blocking_workers_stopped == 2,
        "two blocking workers requested and stopped (db + transfer-io,"
        " handles owned by ExecutorOwner)");
    report(state_owner.is_closed(), "AppStateOwner closed");
    // 复验写路径（DEC-009 ①）：已 admit 作业全部完成、无拒绝、无失败、处理器
    // 无异常（future 逐个消费，RULE-09：结果可见，不静默）。
    report(db->drain_completed(), "database worker drain completed");
    report(!db->drain_budget_exhausted(), "drain budget not exhausted");
    {
        int settle_failures = 0;
        for (auto& future : sink.futures) {
            try {
                future.get();
            } catch (const std::exception& error) {
                ++settle_failures;
                std::printf("    [db job failure] %s\n", error.what());
            }
        }
        report(settle_failures == 0, "all admitted db jobs settled successfully");
    }
    report(sink.admitted > 0 && db->completed_count() == sink.admitted,
        "db jobs completed == admitted (" + std::to_string(db->completed_count())
            + " == " + std::to_string(sink.admitted) + ", zero loss)");
    report(db->failed_count() == 0, "no failed db jobs");
    report(sink.enqueue_rejected == 0 && db->rejected_count() == 0,
        "no enqueue rejections");
    report(state_owner.stats().post_accept_failures == 0,
        "no post-accept handler failures");

    // ---- 重启恢复（session B）：重新 open 后逐域断言一致（SCOPE-09/01）----
    std::printf("\n[restart] reopen data root and assert per-domain consistency\n");
    RecoveryResult reopened;
    LocalIdentity reopened_identity;
    try {
        reopened = perform_startup_recovery(run_root);
        reopened_identity = provision_local_identity(run_root);
    } catch (const std::exception& error) {
        report(false, std::string("reopen recovery failed: ") + error.what());
    }
    if (reopened.store != nullptr) {
        report(reopened.diagnostics.migrations_applied == 0,
            "migration idempotent on reopen (0 steps applied)");
        report(reopened.diagnostics.tmp_orphans_removed == 0,
            "no tmp orphans after clean shutdown");
        // 本地身份二次加载（SCOPE-01 验收 ②）：DeviceId/公钥逐字节一致。
        report(reopened_identity.id == identity.id,
            "local identity DeviceId stable across restart");
        report(reopened_identity.public_key == identity.public_key,
            "local identity public key byte-identical across restart");
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
        report(reopened.state.transfers
                == expected.transfers.transfers,
            "transfer history consistent after restart");
        // 无对端交互（防火墙受限）：无传输/文件本体（传输链路 M4；
        // 文件本体回写位由 test_restart_recovery 承载）。
        report(reopened.state.transfers.empty(),
            "no transfers without a paired peer (single-host smoke)");
        if (reopened.repositories != nullptr) {
            aki::persistence::Statement local_row =
                reopened.repositories->database.prepare(
                    "SELECT trust_state FROM device WHERE device_id = ?1;");
            local_row.bind(1, identity.id.value);
            const bool has_row = local_row.step();
            report(has_row
                    && static_cast<int>(local_row.column_int64(0))
                        == static_cast<int>(TrustState::Unknown),
                "local identity row consistent (trust_state as registered)");
        }
        // 播种对接复验（§11.1 ②）：恢复结果构造 AppStateOwner 初始快照立即可读。
        AppStateOwner reopened_owner{AppStateOwnerOptions{},
            app_state_from(reopened.state)};
        executor::comm::Snapshot<AppState> seeded;
        report(load_snapshot(reopened_owner, seeded)
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
    return g_checks_failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    // 数据根基址：argv[1] 可覆盖（测试/CI 注入）；缺省 resolve_data_root()
    // （Windows %APPDATA%\aki / POSIX XDG，M2-06）。每次运行使用基址下独立
    // 子目录（确定性：同一可执行文件连续多次运行输出一致；成功后清理，失败
    // 保留供诊断）。
    // argv[2] == "--exact" 为驱动/诊断钩子：argv[1] 作为数据根原样使用（不建
    // 子目录、不自动清理）——损坏 DB 干净失败用例与手动复跑真实数据根依赖该
    // 模式；演示断言假定空根。
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

    // 进程内唯一 Executor owner 为 run_demo 内的 ExecutorOwner（AGENTS 规则 7/8；
    // 设计第 8.2/8.3 节）：受控关闭在 run_demo 内显式完成，析构兜底仅覆盖
    // 异常提前返回的路径。
    const int exit_code = run_demo(run_root);

    if (exit_code == 0 && !exact_root) {
        std::error_code ec;
        std::filesystem::remove_all(run_root, ec);  // 成功清理；失败保留诊断。
    }
    return exit_code;
}
