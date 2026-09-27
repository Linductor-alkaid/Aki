// HostRuntime 实现（设计 §8.3/§9.1/§11.1；M5-02 抽离自根 main.cpp——装配与
// 受控关闭的编排原样迁移，宿主演示断言移至 console 驱动 aki_host_smoke）。
#include "app/lifecycle/host_runtime.hpp"

#include "app/application/reconnect_loop.hpp"
#include "app/application/router_sink.hpp"
#include "app/state/app_state_owner.hpp"
#include "heyaki/adapter/heyaki_node_adapter.hpp"
#include "heyaki/adapter/local_identity.hpp"
#include "heyaki/adapter/peer_sessions_pipeline.hpp"
#include "heyaki/session/runtime_node.hpp"
#include "persistence/database/database_worker.hpp"
#include "persistence/database/database_worker_adapter.hpp"
#include "persistence/recovery/startup_recovery.hpp"
#include "persistence/repository/update_jobs.hpp"
#include "persistence/storage/data_root.hpp"
#include "persistence/storage/file_jobs.hpp"
#include "persistence/storage/file_store.hpp"
#include "persistence/storage/transfer_io_worker.hpp"
#include "transfer/transfer/transfer_types.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace aki::app {
namespace {

using namespace std::chrono_literals;

using aki::conversation::ConversationId;
using aki::device::ConnectionPath;
using aki::device::DeviceClass;
using aki::device::DeviceId;
using aki::device::DeviceIdentity;
using aki::device::DiscoveryMethod;
using aki::device::PresenceState;
using aki::device::TrustState;
using aki::heyaki::HeyakiNodeAdapter;
using aki::persistence::DatabaseWorkerControl;
using aki::persistence::DatabaseWorkerOptions;
using aki::persistence::DatabaseWorkerRunnable;
using aki::persistence::DbJob;
using aki::persistence::FileStore;
using aki::transfer::TransferState;

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

// 本地身份 → DeviceIdentity（DEC-006 映射 1；main.cpp 同款语义）。
DeviceIdentity make_local_device_identity(const aki::heyaki::LocalIdentity& identity,
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

AppState app_state_from(const aki::persistence::RecoveredData& data) {
    AppState state;
    state.devices.devices = data.devices;
    state.conversations.conversations = data.conversations;
    state.messages.messages = data.messages;
    state.transfers.transfers = data.transfers;
    return state;
}

// DEC-009 ① 接受后处理器（main.cpp 同款；owner 单写者上下文按接受顺序同步
// 调用，作业映射 §11.1 ① + update_jobs 工厂；入队拒绝双计数可见）。
struct WritePathSink {
    std::shared_ptr<DatabaseWorkerControl> control;
    std::shared_ptr<FileStore> store;
    std::string receive_dir;
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
                        jobs.push_back(
                            aki::persistence::make_transfer_complete_job(
                                store, concrete.transfer.value, receive_dir));
                    } else {
                        jobs.push_back(
                            aki::persistence::make_transfer_terminal_job(
                                concrete.transfer, concrete.final_state));
                        jobs.push_back(
                            aki::persistence::make_transfer_discard_job(
                                store, concrete.transfer.value));
                    }
                }
                // SetPresence / SetDeviceConnectionPath：不持久化（§11.1 ①，
                // DEC-015），无作业。
            },
            update);
        return jobs;
    }
};

}  // namespace

struct HostRuntime::Impl {
    // ---- 声明序 = 构造序；析构为逆序（ExecutorOwner 最后析构，兜底关闭
    // ---- 语义与原 main.cpp 栈序一致）。
    ExecutorOwner executor_owner;  // DEC-006 合并负载定容（main.cpp 同款）
    std::string data_root;
    std::string receive_dir;

    std::optional<aki::persistence::RecoveryResult> recovery;
    std::optional<aki::heyaki::LocalProfile> profile;
    std::optional<aki::heyaki::NodeSession> node_session;

    std::shared_ptr<DatabaseWorkerControl> db;
    std::shared_ptr<aki::persistence::TransferIoControl> transfer_io;
    WritePathSink sink;  // 须先于 state_owner 构造（handler 捕获其地址）
    std::function<void()> wake;  // 快照发布唤醒回调（M5-03，ensure_assembled 注入）

    std::optional<AppStateOwner> state_owner;
    std::optional<HeyakiNodeAdapter> adapter;
    std::optional<DeviceManager> devices;
    std::optional<ConversationManager> conversations;
    std::optional<MessageManager> messages;
    std::optional<TransferManager> transfers;
    std::optional<RouterSink> router;
    std::unique_ptr<ReconnectCoordinator> reconnect;
    std::unique_ptr<aki::heyaki::PeerSessionPipeline> peer_pipeline;

    bool assembled = false;
    bool assembly_failed = false;
    // blocking worker 注册成功标记（注册失败路径不做 drain 等待——worker 从未
    // 启动，排空等待只会空转预算；fail 回收路径据此跳过）。
    bool db_registered = false;
    HostAssemblyReport assembly_report;

    bool shutdown_attempted = false;
    HostShutdownReport shutdown_report;

    Impl() : executor_owner{[] {
        ExecutorOwnerOptions options;
        options.executor_config.min_threads = 2;
        options.executor_config.max_threads = 6;
        return options;
    }()} {}
};

HostRuntime::HostRuntime() : impl_(std::make_unique<Impl>()) {}

HostRuntime::~HostRuntime() {
    // 兜底：正常路径 GUI onShutdown / console 驱动显式调用已关闭（幂等返回）。
    // 极端路径（onShutdown 未执行）下按同一受控序回收；异常不向外传播
    //（析构 noexcept 语境），但也不静默——executor owner 自身析构兜底仍在。
    if (impl_ && impl_->assembled && !impl_->shutdown_attempted) {
        try {
            (void)shutdown_with_report();
        } catch (...) {
        }
    }
}

HostRuntime& HostRuntime::instance() {
    static HostRuntime runtime;
    return runtime;
}

const HostAssemblyReport& HostRuntime::ensure_assembled(std::string data_root,
    std::function<void()> wake) {
    Impl& impl = *impl_;
    if (impl.assembled || impl.assembly_failed || impl.assembly_report.attempted) {
        return impl.assembly_report;  // 幂等：返回首次结果。
    }
    impl.assembly_report.attempted = true;
    impl.wake = std::move(wake);  // 转交 AppStateOwner 构造选项（步骤 4）。

    // 数据根：GUI 宿主无 argv（框架 int main()），缺省 resolve_data_root()
    //（§9.1 启动↔关闭配对条款）；console 驱动/测试经参数注入。
    impl.data_root = data_root.empty()
        ? aki::persistence::resolve_data_root()
        : std::move(data_root);
    const std::string& run_root = impl.data_root;

    const auto fail = [&](std::string reason) -> const HostAssemblyReport& {
        impl.assembly_failed = true;
        impl.assembly_report.ok = false;
        impl.assembly_report.failure_reason = std::move(reason);
        // 部分构造件按同一受控序拆除（各步判存在；禁 std::exit——不销毁自动
        // 对象，受控关闭与兜底析构都不会执行，main.cpp:324-328 先例）。
        try {
            (void)shutdown_with_report();
        } catch (const std::exception& error) {
            impl.assembly_report.failure_reason += " (teardown error: ";
            impl.assembly_report.failure_reason += error.what();
            impl.assembly_report.failure_reason += ")";
        }
        return impl.assembly_report;
    };

    // 1) ExecutorOwner.initialize()——进程内唯一 Executor 生命周期 owner。
    if (!impl.executor_owner.initialize()) {
        return fail("ExecutorOwner.initialize() failed");
    }

    // 2) 主线程同步启动恢复（§11.1 ②：DB 损坏即干净失败，不静默）+ 本地
    //    身份供给（SCOPE-01，同一恢复段；profile 常驻供 Node 装配，M3-04）。
    try {
        impl.recovery.emplace(aki::persistence::perform_startup_recovery(run_root));
        impl.profile.emplace(aki::heyaki::LocalProfile::open(run_root));
    } catch (const std::exception& error) {
        return fail(std::string("startup recovery failed: ") + error.what());
    }
    const aki::heyaki::LocalIdentity identity = impl.profile->identity();
    {
        const auto& recovered = impl.recovery->state;
        impl.assembly_report.recovered_devices = recovered.devices.size();
        impl.assembly_report.recovered_conversations =
            recovered.conversations.size();
        impl.assembly_report.recovered_messages = recovered.messages.size();
        impl.assembly_report.recovered_transfers = recovered.transfers.size();
        impl.assembly_report.migrations_applied =
            impl.recovery->diagnostics.migrations_applied;
        impl.assembly_report.tmp_orphans_removed =
            impl.recovery->diagnostics.tmp_orphans_removed;
    }
    impl.assembly_report.identity_created = identity.created;
    impl.assembly_report.local_device_id = identity.id.value;

    // 接收根（M4-05，DEC-012 风险②）：配置在数据根内（同卷可 rename），根
    // 逻辑名 inbox 与 Adapter push_root 对应；NodeConfig 要求目录存在，先于
    // NodeSession 创建。
    const std::string receive_root_name = "inbox";
    impl.receive_dir = run_root + "/receive/" + receive_root_name;
    {
        std::error_code receive_ec;
        std::filesystem::create_directories(impl.receive_dir, receive_ec);
        if (receive_ec) {
            return fail("receive root cannot be created: "
                + receive_ec.message());
        }
    }
    impl.assembly_report.receive_dir = impl.receive_dir;

    // 3.5) Node/Runtime 装配（DEC-006 借用注入：borrowed Runtime + Node，
    //      EXEC-02 启动段纪律——恢复完成后、事件源接通前）。
    impl.node_session.emplace(aki::heyaki::NodeSession::create(
        impl.executor_owner.executor(),
        aki::heyaki::NodeSession::Options{.profile = &*impl.profile,
            .file_receive_roots = {::heyaki::FileRootConfig{
                .name = receive_root_name,
                .directory = impl.receive_dir}}}));
    impl.assembly_report.lan_interfaces = impl.node_session->has_lan_interfaces();

    // 3) DatabaseWorkerControl——先于 AppStateOwner 构造（§8.3 七步序，
    //    DEC-009 装配时序；单一连接整体移交）。
    impl.db = std::make_shared<DatabaseWorkerControl>(
        std::move(impl.recovery->repositories), DatabaseWorkerOptions{});

    // 3.6) 传输归档 IO worker 控制面（M4-04，DEC-011/§7.1③）。
    impl.transfer_io =
        std::make_shared<aki::persistence::TransferIoControl>(
            impl.recovery->store, aki::persistence::TransferIoWorkerOptions{});

    // 4) AppStateOwner：加载结果播种初始快照 + 接受后处理器（DEC-009 ①）；
    //    快照发布唤醒回调经构造选项注入（M5-03，设计 §9.1 跨线程唤醒接线）。
    impl.sink.control = impl.db;
    impl.sink.store = impl.recovery->store;
    impl.sink.receive_dir = impl.receive_dir;
    AppStateOwnerOptions owner_options;
    owner_options.on_publish = [wake = impl.wake] {
        if (wake) {
            wake();  // 异常由 owner 全捕获计数（publish_hook_failures）。
        }
    };
    impl.state_owner.emplace(std::move(owner_options),
        app_state_from(impl.recovery->state),
        [&sink = impl.sink](const AppStateUpdate& update) { sink(update); });

    // 本地身份经 UpsertDevice 进入 Application State（SCOPE-01）。
    (void)impl.state_owner->submit_update(
        UpsertDevice{make_local_device_identity(identity,
            impl.recovery->state.devices)});

    // 5) 真实 Adapter + 四 Manager（M3-08 组合；presence/path 观察关闭保
    //    确定性，发现观察管道由 start_discovery 启停）。
    impl.adapter.emplace(impl.executor_owner.executor(),
        HeyakiNodeAdapter::Options{.profile = &*impl.profile,
            .session = &*impl.node_session,
            .conversation_for =
                [](const DeviceId& remote) {
                    return ConversationId{
                        std::string{"conv-"} + remote.value};
                },
            .peer_observation = false});

    ManagerPumpOptions device_pump;
    device_pump.name = "aki.dm";
    impl.devices.emplace(impl.executor_owner.executor(), *impl.state_owner,
        *impl.adapter, device_pump);

    ConversationManagerOptions conversation_options;
    conversation_options.pump.name = "aki.cm";
    impl.conversations.emplace(impl.executor_owner.executor(),
        *impl.state_owner, conversation_options);

    MessageManagerOptions message_options;
    message_options.pump.name = "aki.mm";
    message_options.local_device = identity.id;
    impl.messages.emplace(impl.executor_owner.executor(), *impl.state_owner,
        *impl.adapter, message_options);

    TransferManagerOptions transfer_options;
    transfer_options.pump.name = "aki.tm";
    transfer_options.sender = identity.id;
    transfer_options.io = impl.transfer_io.get();
    for (const auto& row : impl.recovery->state.transfers) {
        if (!is_terminal(row.state)) {
            transfer_options.seeded_rows.push_back(row);
        }
    }
    impl.transfers.emplace(impl.executor_owner.executor(), *impl.state_owner,
        *impl.adapter, transfer_options);

    // 6) 注册 DatabaseWorker（EXEC-07 唯一入口 start_blocking_worker）。
    {
        auto db_runnable = std::make_unique<DatabaseWorkerRunnable>(impl.db);
        executor::BlockingWorkerSpec db_spec;
        db_spec.name = "aki.db-worker";
        db_spec.config.thread_name = "aki-db-worker";
        db_spec.worker = std::move(db_runnable);
        if (!impl.executor_owner.start_blocking_worker(std::move(db_spec))) {
            return fail("DatabaseWorker registration failed");
        }
        impl.db->mark_registered();
        impl.db_registered = true;
    }

    // 6.5) 注册 transfer IO worker（M4-04，DEC-011/§11.1③）。
    {
        auto transfer_io_runnable =
            std::make_unique<aki::persistence::TransferIoRunnable>(
                impl.transfer_io->impl());
        executor::BlockingWorkerSpec transfer_io_spec;
        transfer_io_spec.name = "aki.transfer-io";
        transfer_io_spec.config.thread_name = "aki-transfer-io";
        transfer_io_spec.worker = std::move(transfer_io_runnable);
        if (!impl.executor_owner.start_blocking_worker(
                std::move(transfer_io_spec))) {
            return fail("transfer IO worker registration failed");
        }
    }

    // 7) RouterSink 注册（EXEC-02 启动段纪律：恢复完成、worker 就位后才
    //    接通事件源）。
    impl.router.emplace(*impl.devices, *impl.conversations, *impl.messages,
        *impl.transfers);
    impl.adapter->set_sink(&*impl.router);

    // 7.5) peer_sessions diff 管道 + 重连协调器（M3-06/M3-08；断开后有界
    //      重连循环 EXEC-05 长任务，DEC-008 承载）。裸指针捕获成员地址：
    //      peer_pipeline 先于各指向对象析构（声明序 + 关闭序双重保证）。
    impl.reconnect =
        std::make_unique<ReconnectCoordinator>(impl.executor_owner.executor(),
            ReconnectCoordinatorOptions{});
    auto* node_for_reconnect = &*impl.node_session;
    auto* reconnect_for_hooks = impl.reconnect.get();
    auto* router_for_hooks = &*impl.router;
    impl.peer_pipeline =
        std::make_unique<aki::heyaki::PeerSessionPipeline>(
            impl.executor_owner.executor(), *impl.node_session,
            aki::heyaki::PeerSessionEvents{
                .on_connected =
                    // M5-04（DEC-015）：初连即携带 diff 映射路径（删除 M3-06
                    // 的 Lan 硬编码——初连不发独立路径事件）。
                    [router_for_hooks](const DeviceId& peer,
                        ConnectionPath path) {
                        (void)router_for_hooks->on_device_connected(peer,
                            path);
                    },
                .on_disconnected =
                    [router_for_hooks, node_for_reconnect,
                        reconnect_for_hooks](const DeviceId& peer) {
                        (void)router_for_hooks->on_device_disconnected(peer);
                        // SCOPE-11：断开后启动有界重连循环（恢复后原会话经
                        // connect_lan 回到 authenticated）。
                        ReconnectCoordinator::Attempt try_conn =
                            [node_for_reconnect, peer]() {
                                return node_for_reconnect->connect_lan(peer);
                            };
                        ReconnectCoordinator::RecoveryCheck is_auth =
                            [node_for_reconnect, peer]() {
                                return node_for_reconnect
                                    ->session_authenticated(peer);
                            };
                        ReconnectCoordinator::PerPeerHooks hooks{
                            std::move(try_conn), std::move(is_auth)};
                        (void)reconnect_for_hooks->start(peer, hooks);
                    },
                .on_connection_path_changed =
                    [router_for_hooks](const DeviceId& peer,
                        ConnectionPath path) {
                        (void)router_for_hooks->on_connection_path_changed(
                            peer, ConnectionPath::Unknown, path);
                    }});

    // 首帧播种快照即刻可读（装配完成即恢复结果可见，§11.1 ② 播种断言先例
    // 由 console 驱动承载）。
    executor::comm::Snapshot<AppState> seeded_snapshot;
    if (!load_snapshot(*impl.state_owner, seeded_snapshot)) {
        return fail("seeded snapshot unreadable after assembly");
    }

    impl.assembled = true;
    impl.assembly_report.ok = true;
    return impl.assembly_report;
}

bool HostRuntime::assembled() const noexcept {
    return impl_->assembled;
}

bool HostRuntime::assembly_failed() const noexcept {
    return impl_->assembly_failed;
}

const HostAssemblyReport& HostRuntime::assembly_report() const noexcept {
    return impl_->assembly_report;
}

const std::string& HostRuntime::data_root() const noexcept {
    return impl_->data_root;
}

bool HostRuntime::start_discovery_observation() {
    if (!impl_->assembled) {
        return false;
    }
    return impl_->adapter->start_discovery(DiscoveryMethod::LanDiscovery);
}

bool HostRuntime::discovery_observation_running() const {
    return impl_->assembled && impl_->adapter->discovery_running();
}

void HostRuntime::stop_discovery_observation() {
    if (impl_->assembled) {
        impl_->adapter->stop_discovery();
    }
}

void HostRuntime::quiesce() {
    if (!impl_->assembled) {
        return;
    }
    Impl& impl = *impl_;
    (void)impl.transfers->flush(2s);   // 有界预算（main.cpp 同款）
    (void)impl.devices->flush(2s);
    (void)impl.conversations->flush(2s);
    (void)impl.messages->flush(2s);
    for (;;) {
        const auto watermark = [](const AppStateOwner& owner) {
            return owner.stats().updates_applied
                + owner.stats().updates_rejected + owner.stats().events_forwarded
                + owner.stats().events_dropped;
        };
        const std::uint64_t before = watermark(*impl.state_owner);
        impl.state_owner->drain();
        if (watermark(*impl.state_owner) == before) {
            return;
        }
    }
}

bool HostRuntime::load_state_snapshot(AppState& out) const {
    if (!impl_->assembled) {
        return false;
    }
    executor::comm::Snapshot<AppState> snapshot;
    if (!load_snapshot(*impl_->state_owner, snapshot)) {
        return false;
    }
    out = snapshot.value;
    return true;
}

const HostShutdownReport& HostRuntime::shutdown_with_report() {
    Impl& impl = *impl_;
    if (impl.shutdown_attempted) {
        return impl.shutdown_report;  // 幂等：返回首次报告。
    }
    impl.shutdown_attempted = true;
    HostShutdownReport& report = impl.shutdown_report;
    report.attempted = true;

    // ---- §8.3 宿主钩子原序（不省略不重排）→ EXEC-01 步骤 2~5 ----
    // 各步对部分装配状态判存在（装配失败回收路径同样安全）；「不适用」步
    // 的完成标志按满足记录（无可取消/无排队即视为该步无工作）。
    report.executor_report = impl.executor_owner.shutdown([&] {
        // ① 取消在途可取消任务（无在飞时幂等）。
        if (impl.transfers) {
            (void)impl.transfers->request_cancel_all();
        }
        report.transfers_cancelled = true;
        report.hook_sequence.push_back("hook:transfer.request_cancel_all");

        // ② flush 各 Manager 至泵静止并消费 future（TM flush 含 IO 在飞归零）。
        if (impl.transfers && impl.devices && impl.conversations
            && impl.messages) {
            report.managers_flushed = impl.transfers->flush(2s)
                && impl.devices->flush(2s) && impl.conversations->flush(2s)
                && impl.messages->flush(2s);
        } else {
            report.managers_flushed = true;
        }
        report.hook_sequence.push_back("hook:managers.flush");

        // ③ 停 Adapter 投递。
        if (impl.adapter) {
            impl.adapter->set_sink(nullptr);
            impl.adapter->stop_discovery();
        }
        report.adapter_delivery_stopped = true;
        report.hook_sequence.push_back("hook:adapter.stop_delivery");

        // ③.5 Heyaki 生产者停止（观察管道停止 + 重连长任务取消并消费 future
        //     + Node::shutdown + Runtime::shutdown，早于 owner 步骤 2/3/5）。
        if (impl.peer_pipeline) {
            impl.peer_pipeline->stop();
        }
        report.peer_pipeline_stopped = true;
        report.hook_sequence.push_back("hook:peer_pipeline.stop");
        if (impl.reconnect) {
            const auto consumed = impl.reconnect->stop_all();
            report.reconnect_futures_consumed =
                consumed > 0 || impl.reconnect->running_count() == 0;
        } else {
            report.reconnect_futures_consumed = true;
        }
        report.hook_sequence.push_back("hook:reconnect.stop_all");
        if (impl.node_session) {
            const auto node_report = impl.node_session->shutdown();
            report.node_stopped = node_report.node_stopped;
            report.runtime_stopped = node_report.runtime_stopped;
            report.borrowed_runtime_shutdown_performed =
                node_report.runtime_executor_shutdown_performed;
            report.runtime_drain_timed_out =
                node_report.runtime_drain_timed_out;
        } else {
            report.node_stopped = true;
            report.runtime_stopped = true;
        }
        report.hook_sequence.push_back("hook:node_session.shutdown");

        // ④ comm 关闭（主线程 = owner 上下文）。
        if (impl.state_owner) {
            impl.state_owner->close();
        }
        report.state_owner_closed =
            !impl.state_owner || impl.state_owner->is_closed();
        report.hook_sequence.push_back("hook:state_owner.close");

        // ⑤ DB 排空位于钩子序列末尾（close() 之后、owner 步骤 2/3 之前，
        //    §11.1 ③）。仅对已注册 worker 执行（未注册即无在飞作业）。
        if (impl.db && impl.db_registered) {
            impl.db->request_drain();
            report.db_drain_requested = true;
            report.db_drained_within_budget =
                wait_until([&] { return impl.db->drain_completed(); }, 3s);
            report.db_budget_exhausted = impl.db->drain_budget_exhausted();
        } else {
            report.db_drain_requested = true;
            report.db_drained_within_budget = true;
        }
        report.hook_sequence.push_back("hook:db.request_drain");

        report.hook_sequence_completed = true;
    });

    // ---- 写路径复验（DEC-009 ①）：future 逐个消费（RULE-09：结果可见）----
    report.write_admitted = impl.sink.admitted;
    report.write_enqueue_rejected = impl.sink.enqueue_rejected;
    for (auto& future : impl.sink.futures) {
        try {
            future.get();
        } catch (const std::exception& error) {
            ++report.write_settle_failures;
            if (report.first_write_settle_error.empty()) {
                report.first_write_settle_error = error.what();
            }
        }
    }
    if (impl.db) {
        report.db_completed = impl.db->completed_count();
        report.db_failed = impl.db->failed_count();
        report.db_rejected = impl.db->rejected_count();
    }
    if (impl.state_owner) {
        report.post_accept_failures =
            impl.state_owner->stats().post_accept_failures;
    }

    return impl.shutdown_report;
}

bool HostRuntime::shutdown_completed() const noexcept {
    return impl_->shutdown_attempted;
}

const HostShutdownReport& HostRuntime::last_shutdown_report() const noexcept {
    return impl_->shutdown_report;
}

executor::Executor& HostRuntime::executor() {
    return impl_->executor_owner.executor();
}

AppStateOwner& HostRuntime::state_owner() {
    return *impl_->state_owner;
}

DeviceManager& HostRuntime::device_manager() {
    return *impl_->devices;
}

ConversationManager& HostRuntime::conversation_manager() {
    return *impl_->conversations;
}

MessageManager& HostRuntime::message_manager() {
    return *impl_->messages;
}

TransferManager& HostRuntime::transfer_manager() {
    return *impl_->transfers;
}

void HostRuntime::pump_state() {
    if (!impl_->assembled) {
        return;
    }
    impl_->state_owner->drain();  // 有界工作单元（owner 上下文，EXEC-03）。
}

}  // namespace aki::app
