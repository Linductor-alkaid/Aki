// Aki console 冒烟宿主（M1-06 起为设计第 8.3 节组合根的最小进程内实现；M2-07
// 起接入本地持久化的启动恢复与写路径，v0.2.0 验收载体 / M2 退出-1）。
//
// 进程内 Executor owner 自 M1-06 起为正式 ExecutorOwner（设计第 8.2 节落点说明，
// AGENTS 规则 7/8）：Manager 排空泵、传输会话与 DatabaseWorker（blocking worker，
// EXEC-04 首次启用）全部任务都在其生命周期内，无 std::thread / std::async /
// 自建线程（RULE-07）。
//
// M2-07 组合顺序（设计第 8.3 节 + 第 11.1 节 ②③）：
//   ExecutorOwner.initialize() → 主线程同步启动恢复（解析数据根 → open（损坏即
//   干净失败输出原因退出）→ user_version 迁移 → 四仓储逐域加载 → files/tmp/
//   清扫 → 以加载结果经 AppStateOwner 构造入参播种初始快照）→ 四 Manager 构造
//   → 注册 DatabaseWorker → RouterSink（恢复完成前不注册、不注入事件，
//   EXEC-02 启动段纪律）→ 演示场景（每步 quiesce 后由 owner 单写者上下文按
//   接受顺序镜像 typed 更新为 DB 作业，§11.1 ①）→ 受控关闭（钩子末尾
//   AppStateOwner.close() 之后排空 DatabaseWorker，§11.1 ③；EXEC-01 步骤 2~5）
//   → 重开恢复逐域断言一致（SCOPE-09 / M2 退出-1，进程内两次打开同一数据根）。
//
// 演示脚本：两台假设备（local-1 本机 / alpha-01 远端）经 FakeHeyakiAdapter 的
// inject_* 编程式注入完成"发现 -> 信任（Pending -> Trusted）-> 文本消息
// （send_text + delivered/received）-> 传输历史（t-1 Completed 含文件本体作业组 /
// t-2 Cancelled）-> 断开 -> 重连"，经 DoubleBuffer 一致快照与序列号排序的必达
// 事件主路径逐步断言，输出人可读控制台结果。
//
// 确定性：inject_* 与出站命令全部由主线程串行驱动（FakeHeyakiAdapter 的宿主
// 串行化契约，EXEC-02），每步经 flush（有界预算）+ owner drain 推进到静止后再
// 断言，不依赖时序；数据根为每次运行独立子目录（默认基址 resolve_data_root()，
// argv[1] 可覆盖），同一可执行文件连续多次运行输出一致；成功后清理运行目录，
// 失败保留供诊断。
//
// 设备信任确认属用户流程（设计第 4/8.3 节）：M1 无 Trust Manager 与 UI，由宿主
// 经 AppStateOwner 的 UpsertDevice 更新指令模拟用户确认（owner 侧按信任状态机
// 校验合法边），UI 于 M5 接入。
#include "app/application/router_sink.hpp"
#include "app/lifecycle/executor_owner.hpp"
#include "app/state/app_state_owner.hpp"
#include "heyaki/adapter/fake_heyaki_adapter.hpp"
#include "persistence/database/database.hpp"
#include "persistence/database/database_worker.hpp"
#include "persistence/database/database_worker_adapter.hpp"
#include "persistence/recovery/startup_recovery.hpp"
#include "persistence/repository/update_jobs.hpp"
#include "persistence/storage/data_root.hpp"
#include "persistence/storage/file_jobs.hpp"
#include "persistence/storage/file_store.hpp"
#include "persistence/storage/sha256.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
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
using aki::app::ManagerPumpOptions;
using aki::app::MessageDeliveredEvent;
using aki::app::MessageManager;
using aki::app::MessageManagerOptions;
using aki::app::MessageReceivedEvent;
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
using aki::heyaki::FakeHeyakiAdapter;
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

std::string enum_text(PresenceState state) {
    return std::string(aki::device::to_string(state));
}

std::string enum_text(TrustState state) {
    return std::string(aki::device::to_string(state));
}

std::string enum_text(ConversationState state) {
    return std::string(aki::conversation::to_string(state));
}

std::string enum_text(DeliveryState state) {
    return std::string(aki::conversation::to_string(state));
}

std::string enum_text(TransferState state) {
    return std::string(aki::transfer::to_string(state));
}

const char* event_type_name(const AppEvent& event) {
    if (std::holds_alternative<DeviceDiscoveredEvent>(event.payload)) {
        return "DeviceDiscovered";
    }
    if (std::holds_alternative<DeviceConnectedEvent>(event.payload)) {
        return "DeviceConnected";
    }
    if (std::holds_alternative<DeviceDisconnectedEvent>(event.payload)) {
        return "DeviceDisconnected";
    }
    if (std::holds_alternative<MessageReceivedEvent>(event.payload)) {
        return "MessageReceived";
    }
    if (std::holds_alternative<MessageDeliveredEvent>(event.payload)) {
        return "MessageDelivered";
    }
    if (std::holds_alternative<aki::app::TransferStartedEvent>(event.payload)) {
        return "TransferStarted";
    }
    if (std::holds_alternative<aki::app::TransferProgressEvent>(event.payload)) {
        return "TransferProgress";
    }
    if (std::holds_alternative<aki::app::TransferCompletedEvent>(event.payload)) {
        return "TransferCompleted";
    }
    if (std::holds_alternative<ConnectionPathChangedEvent>(event.payload)) {
        return "ConnectionPathChanged";
    }
    return "Unknown";
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

const DeviceIdentity* find_device(
    const AppState& state, const DeviceId& id) {
    for (const auto& device : state.devices.devices) {
        if (device.id == id) {
            return &device;
        }
    }
    return nullptr;
}

const Message* find_message(const AppState& state, const MessageId& id) {
    for (const auto& message : state.messages.messages) {
        if (message.id == id) {
            return &message;
        }
    }
    return nullptr;
}

const Transfer* find_transfer(const AppState& state, const TransferId& id) {
    for (const auto& transfer : state.transfers.transfers) {
        if (transfer.id == id) {
            return &transfer;
        }
    }
    return nullptr;
}

std::span<const std::byte> bytes_of(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// §11.1 ① typed 更新 → DB 作业的宿主接线（owner 单写者上下文 = 主线程，按
// 接受顺序入队；入队不新增执行上下文）。SetPresence/SetConnectionPath 不持久化
// （易失/摘要语义）；幂等 no-op 接受同样入队，由作业侧幂等语义吸收（§11.1 ①）。
// admission 拒绝与执行失败可观测（RULE-09）：future 全部保留、排空后逐个消费。
struct PersistenceMirror {
    PersistenceMirror(std::shared_ptr<DatabaseWorkerControl> control_in,
        std::shared_ptr<FileStore> store_in)
        : control(std::move(control_in)), store(std::move(store_in)) {}

    std::shared_ptr<DatabaseWorkerControl> control;
    std::shared_ptr<FileStore> store;
    ConversationId conversation;  // UpsertMessage 的 FK 归属（宿主已知会话）。
    std::vector<std::future<void>> futures;
    std::uint64_t admitted = 0;
    std::uint64_t enqueue_rejected = 0;

    bool submit(AppStateUpdate update) {
        for (DbJob& job : jobs_for(update)) {
            futures.push_back(job.done->get_future());
            if (!control->enqueue(std::move(job))) {
                futures.pop_back();
                ++enqueue_rejected;
                return false;  // 明确拒绝，不静默（RULE-09）
            }
            ++admitted;
        }
        return true;
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
                            concrete.message, conversation));
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
        "aki 0.1.0 (M2 smoke: discovery -> trust -> messaging -> transfer"
        " history -> restart recovery)\n");
    std::printf(
        "devices: local-1 (this host) / alpha-01 (remote, FakeHeyakiAdapter)\n");
    std::printf("data root: %s\n", run_root.c_str());

    // ---- 组合根（设计第 8.3 节装配顺序 + 第 11.1 节 ② 启动恢复）----
    // 1) ExecutorOwner.initialize()——进程内唯一 Executor 生命周期 owner。
    ExecutorOwner executor_owner;
    if (!executor_owner.initialize()) {
        std::printf("[FATAL] ExecutorOwner.initialize() failed\n");
        return 1;
    }
    // 2) 主线程同步启动恢复（§11.1 ②：不经 blocking worker；DB 损坏即干净
    //    失败输出原因退出，不静默）。
    RecoveryResult recovery;
    try {
        recovery = perform_startup_recovery(run_root);
    } catch (const std::exception& error) {
        std::printf("[FATAL] startup recovery failed: %s\n", error.what());
        return 2;  // ExecutorOwner 析构兜底关闭（无生产者）。
    }
    std::printf(
        "recovery: %zu device(s), %zu conversation(s), %zu message(s),"
        " %zu transfer(s), %zu migration step(s), %zu tmp orphan(s) removed\n",
        recovery.state.devices.size(), recovery.state.conversations.size(),
        recovery.state.messages.size(), recovery.state.transfers.size(),
        recovery.diagnostics.migrations_applied,
        recovery.diagnostics.tmp_orphans_removed);
    // 3) AppStateOwner：以加载结果播种初始快照（构造入参自 M1-02 存在；
    //    owner 上下文 = 主线程，单写者，RULE-02/EXEC-03）。
    AppStateOwner state_owner{AppStateOwnerOptions{},
        app_state_from(recovery.state)};
    // 4) FakeHeyakiAdapter 与四 Manager（构造注入 executor、owner、adapter 与
    //    容量预算）。
    FakeHeyakiAdapter adapter;

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
    message_options.local_device = DeviceId{"local-1"};
    MessageManager messages{
        executor_owner.executor(), state_owner, adapter, message_options};

    TransferManagerOptions transfer_options;
    transfer_options.pump.name = "aki.tm";
    transfer_options.sender = DeviceId{"local-1"};
    TransferManager transfers{
        executor_owner.executor(), state_owner, adapter, transfer_options};

    // 5) 注册 DatabaseWorker（恢复完成后；单一连接整体移交，§11.1 ③；
    //    EXEC-07 唯一入口 start_blocking_worker，句柄归 owner）。
    DatabaseWorkerOptions db_options;
    auto db = std::make_shared<DatabaseWorkerControl>(
        std::move(recovery.repositories), db_options);
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

    // 6) RouterSink 注册（EXEC-02 启动段纪律：恢复完成、worker 就位后才接通
    //    事件源）。
    RouterSink router{devices, conversations, messages, transfers};
    adapter.set_sink(&router);

    PersistenceMirror mirror{db, recovery.store};

    // 每步推进到静止：flush 四个 Manager（有界预算，消费排空 future）+ 状态
    // owner drain 至水位不变（主线程即 owner 上下文）。
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

    // 必达事件主路径：按 owner 分配的单调序列号逐条消费并核对 FIFO 顺序
    // （设计第 10.1 节）。
    std::uint64_t next_sequence = 1;
    const auto consume_events = [&] {
        std::vector<AppEvent> events;
        AppEvent event;
        while (state_owner.try_receive_event(event)) {
            events.push_back(std::move(event));
        }
        for (const auto& received : events) {
            report(received.sequence == next_sequence,
                "event sequence " + std::to_string(received.sequence) + " == "
                    + std::to_string(next_sequence) + " ("
                    + event_type_name(received) + ")");
            ++next_sequence;
        }
        return events;
    };

    // ---- 步骤 1/7：本机身份 + 发现（discovered + connected）----
    std::printf("\n[step 1/7] local identity + discovery\n");
    DeviceIdentity local;
    local.id = DeviceId{"local-1"};
    local.display_name = "local-1 (this host)";
    local.device_class = DeviceClass::Desktop;
    local.os_name = "console host";
    local.trust_state = TrustState::Unknown;
    report(state_owner.submit_update(UpsertDevice{local}),
        "local identity registered (UpsertDevice local-1)");
    quiesce();
    report(mirror.submit(UpsertDevice{local}), "db: device local-1 enqueued");

    report(devices.start_discovery(DiscoveryMethod::LanDiscovery),
        "start_discovery(LanDiscovery) accepted");
    quiesce();
    report(adapter.discovery_running(), "adapter discovery running");

    DeviceIdentity alpha;
    alpha.id = DeviceId{"alpha-01"};
    alpha.display_name = "alpha-01";
    alpha.device_class = DeviceClass::Desktop;
    alpha.os_name = "Linux";
    alpha.trust_state = TrustState::Unknown;
    alpha.presence = PresenceState::Online;
    DiscoveredDevice discovered;
    discovered.identity = alpha;
    discovered.method = DiscoveryMethod::LanDiscovery;
    report(adapter.inject_device_discovered(discovered),
        "inject_device_discovered(alpha-01)");
    report(adapter.inject_device_connected(DeviceId{"alpha-01"}, ConnectionPath::Lan),
        "inject_device_connected(alpha-01, LAN)");
    quiesce();

    {
        executor::comm::Snapshot<AppState> snapshot;
        report(load_snapshot(state_owner, snapshot), "snapshot readable");
        const DeviceIdentity* device =
            find_device(snapshot.value, DeviceId{"alpha-01"});
        report(device != nullptr, "device store has alpha-01");
        if (device != nullptr) {
            report(device->trust_state == TrustState::Unknown,
                "trust_state == Unknown");
            report(device->presence == PresenceState::Online,
                "presence == Online (" + enum_text(device->presence) + ")");
            report(mirror.submit(UpsertDevice{*device}),
                "db: device alpha-01 enqueued");
        }
    }
    {
        auto events = consume_events();
        report(events.size() == 2, "two events on the main path");
        if (events.size() == 2) {
            report(std::holds_alternative<DeviceDiscoveredEvent>(events[0].payload),
                "event 1 is DeviceDiscovered");
            report(std::holds_alternative<DeviceConnectedEvent>(events[1].payload),
                "event 2 is DeviceConnected");
        }
    }

    // ---- 步骤 2/7：信任（Pending -> Trusted，用户流程模拟）----
    std::printf("\n[step 2/7] trust (Pending -> Trusted)\n");
    DeviceIdentity pending = alpha;
    pending.trust_state = TrustState::Pending;
    report(state_owner.submit_update(UpsertDevice{pending}),
        "trust: Unknown -> Pending accepted by trust state machine");
    quiesce();
    report(mirror.submit(UpsertDevice{pending}), "db: trust Pending enqueued");
    DeviceIdentity trusted = alpha;
    trusted.trust_state = TrustState::Trusted;
    report(state_owner.submit_update(UpsertDevice{trusted}),
        "trust: Pending -> Trusted accepted by trust state machine");
    quiesce();
    report(mirror.submit(UpsertDevice{trusted}), "db: trust Trusted enqueued");
    {
        executor::comm::Snapshot<AppState> snapshot;
        report(load_snapshot(state_owner, snapshot), "snapshot readable");
        const DeviceIdentity* device =
            find_device(snapshot.value, DeviceId{"alpha-01"});
        report(device != nullptr && device->trust_state == TrustState::Trusted,
            "trust_state == Trusted (" + std::string(
                device != nullptr ? enum_text(device->trust_state) : "missing")
                + ")");
    }
    consume_events();  // 信任更新不产生主路径事件。

    // ---- 步骤 3/7：会话 + 文本消息（send_text + delivered/received）----
    std::printf("\n[step 3/7] conversation + text messaging\n");
    report(conversations.ensure_conversation(DeviceId{"local-1"}, DeviceId{"alpha-01"}),
        "ensure_conversation(local-1, alpha-01)");
    quiesce();
    {
        executor::comm::Snapshot<AppState> snapshot;
        report(load_snapshot(state_owner, snapshot), "snapshot readable");
        report(snapshot.value.conversations.conversations.size() == 1,
            "one conversation");
        if (snapshot.value.conversations.conversations.size() == 1) {
            const auto& conversation = snapshot.value.conversations.conversations.front();
            report(conversation.id == ConversationId{"conv-alpha-01"},
                "conversation id == conv-alpha-01");
            report(conversation.state == ConversationState::Active,
                "conversation state == Active ("
                    + enum_text(conversation.state) + ")");
            mirror.conversation = conversation.id;
            report(mirror.submit(
                        aki::app::UpsertConversation{conversation}),
                "db: conversation enqueued");
        }
    }
    report(messages.send_text(DeviceId{"alpha-01"}, MessageId{"m-1"}, "hello alpha"),
        "send_text(m-1, \"hello alpha\") accepted");
    quiesce();
    report(adapter.inject_message_delivered(
               ConversationId{"conv-alpha-01"}, MessageId{"m-1"}),
        "inject_message_delivered(m-1)");
    quiesce();
    Message reply;
    reply.id = MessageId{"m-2"};
    reply.sender = DeviceId{"alpha-01"};
    reply.receiver = DeviceId{"local-1"};
    reply.type = MessageType::Text;
    reply.state = DeliveryState::Sent;  // Manager 收到事件后强制记录 Delivered。
    reply.payload = TextPayload{"hello local"};
    report(adapter.inject_message_received(reply), "inject_message_received(m-2)");
    quiesce();

    {
        executor::comm::Snapshot<AppState> snapshot;
        report(load_snapshot(state_owner, snapshot), "snapshot readable");
        report(snapshot.value.messages.messages.size() == 2, "two messages stored");
        const Message* sent = find_message(snapshot.value, MessageId{"m-1"});
        report(sent != nullptr && sent->state == DeliveryState::Delivered,
            "m-1: Sent -> Delivered ("
                + std::string(sent != nullptr ? enum_text(sent->state) : "missing")
                + ")");
        const Message* received = find_message(snapshot.value, MessageId{"m-2"});
        report(received != nullptr && received->state == DeliveryState::Delivered,
            "m-2: received recorded as Delivered ("
                + std::string(received != nullptr ? enum_text(received->state)
                                                  : "missing")
                + ")");
        report(adapter.sent_texts().size() == 1,
            "adapter observed one outbound text");
        if (sent != nullptr) {
            report(mirror.submit(aki::app::UpsertMessage{*sent}),
                "db: message m-1 enqueued");
            report(mirror.submit(aki::app::SetDeliveryState{
                        MessageId{"m-1"}, DeliveryState::Delivered}),
                "db: delivery final state m-1 enqueued");
        }
        if (received != nullptr) {
            report(mirror.submit(aki::app::UpsertMessage{*received}),
                "db: message m-2 enqueued");
        }
    }
    {
        // 主路径 FIFO 顺序：delivered(m-1) 先于 received(m-2)。
        auto events = consume_events();
        report(events.size() == 2, "two events on the main path");
        if (events.size() == 2) {
            const auto& delivered = events[0];
            report(std::holds_alternative<MessageDeliveredEvent>(delivered.payload),
                "event 3 is MessageDelivered");
            if (std::holds_alternative<MessageDeliveredEvent>(delivered.payload)) {
                report(std::get<MessageDeliveredEvent>(delivered.payload).message
                        == MessageId{"m-1"},
                    "delivered event carries m-1");
            }
            const auto& received = events[1];
            report(std::holds_alternative<MessageReceivedEvent>(received.payload),
                "event 4 is MessageReceived");
            if (std::holds_alternative<MessageReceivedEvent>(received.payload)) {
                report(std::get<MessageReceivedEvent>(received.payload).message.id
                        == MessageId{"m-2"},
                    "received event carries m-2");
            }
        }
    }

    // ---- 步骤 4/7：断开（disconnected）----
    std::printf("\n[step 4/7] disconnect\n");
    report(adapter.inject_device_disconnected(DeviceId{"alpha-01"}),
        "inject_device_disconnected(alpha-01)");
    quiesce();
    {
        executor::comm::Snapshot<AppState> snapshot;
        report(load_snapshot(state_owner, snapshot), "snapshot readable");
        const DeviceIdentity* device =
            find_device(snapshot.value, DeviceId{"alpha-01"});
        report(device != nullptr && device->presence == PresenceState::Offline,
            "presence == Offline ("
                + std::string(device != nullptr ? enum_text(device->presence)
                                                : "missing")
                + ")");
        report(snapshot.value.conversations.conversations.size() == 1
                && snapshot.value.conversations.conversations.front().state
                    == ConversationState::Disconnected,
            "conversation state == Disconnected");
        report(snapshot.value.messages.messages.size() == 2,
            "message history kept across disconnect");
        if (snapshot.value.conversations.conversations.size() == 1) {
            report(mirror.submit(aki::app::UpsertConversation{
                        snapshot.value.conversations.conversations.front()}),
                "db: conversation Disconnected enqueued");
        }
    }
    {
        auto events = consume_events();
        report(events.size() == 1, "one event on the main path");
        if (events.size() == 1) {
            report(std::holds_alternative<DeviceDisconnectedEvent>(events[0].payload),
                "event 5 is DeviceDisconnected");
        }
    }

    // ---- 步骤 5/7：重连（connected -> Active，同一会话与历史保持，RULE-06）----
    std::printf("\n[step 5/7] reconnect\n");
    report(adapter.inject_device_connected(DeviceId{"alpha-01"}, ConnectionPath::P2p),
        "inject_device_connected(alpha-01, P2P)");
    quiesce();
    {
        executor::comm::Snapshot<AppState> snapshot;
        report(load_snapshot(state_owner, snapshot), "snapshot readable");
        const DeviceIdentity* device =
            find_device(snapshot.value, DeviceId{"alpha-01"});
        report(device != nullptr && device->presence == PresenceState::Online,
            "presence == Online ("
                + std::string(device != nullptr ? enum_text(device->presence)
                                                : "missing")
                + ")");
        report(snapshot.value.conversations.conversations.size() == 1,
            "no new conversation created (RULE-06)");
        if (snapshot.value.conversations.conversations.size() == 1) {
            const auto& conversation = snapshot.value.conversations.conversations.front();
            report(conversation.id == ConversationId{"conv-alpha-01"},
                "same conversation id conv-alpha-01");
            report(conversation.state == ConversationState::Active,
                "conversation state == Active ("
                    + enum_text(conversation.state) + ")");
            report(mirror.submit(aki::app::UpsertConversation{conversation}),
                "db: conversation Active enqueued");
        }
        report(snapshot.value.messages.messages.size() == 2,
            "message history unchanged across reconnect");
    }
    {
        auto events = consume_events();
        report(events.size() == 1, "one event on the main path");
        if (events.size() == 1) {
            report(std::holds_alternative<DeviceConnectedEvent>(events[0].payload),
                "event 6 is DeviceConnected");
        }
    }

    // ---- 步骤 6/7：传输历史（t-1 Completed 含文件本体 / t-2 Cancelled）----
    std::printf("\n[step 6/7] transfer history (completed with file + cancelled)\n");
    const std::string t1_payload =
        "restart-recovery payload for transfer t-1 (host byte source)";
    Transfer t1;
    t1.id = TransferId{"t-1"};
    t1.sender = DeviceId{"local-1"};
    t1.receiver = DeviceId{"alpha-01"};
    t1.file = aki::transfer::FileMetadata{"notes.txt", t1_payload.size(),
        "text/plain"};
    // 新行 upsert 无转移校验（owner 侧），但 Completed 终态必须经合法边
    // Transferring -> Completed 到达（Queued -> Completed 会被状态机拒绝）。
    t1.state = TransferState::Transferring;
    report(adapter.inject_transfer_started(t1), "inject_transfer_started(t-1)");
    quiesce();
    {
        executor::comm::Snapshot<AppState> snapshot;
        report(load_snapshot(state_owner, snapshot), "snapshot readable");
        const Transfer* row = find_transfer(snapshot.value, TransferId{"t-1"});
        report(row != nullptr && row->state == TransferState::Transferring,
            "t-1 recorded (Transferring)");
        if (row != nullptr) {
            report(mirror.submit(aki::app::UpsertTransfer{*row}),
                "db: transfer t-1 enqueued");
        }
    }
    // 字节源驱动 .part（M2-06 纪律：真实数据链路 M4）。
    recovery.store->write_part("t-1", bytes_of(t1_payload));
    report(adapter.inject_transfer_progress(TransferId{"t-1"}, t1_payload.size(),
                t1_payload.size()),
        "inject_transfer_progress(t-1)");
    quiesce();
    {
        executor::comm::Snapshot<AppState> snapshot;
        report(load_snapshot(state_owner, snapshot), "snapshot readable");
        const Transfer* row = find_transfer(snapshot.value, TransferId{"t-1"});
        report(row != nullptr && row->transferred == t1_payload.size()
                && row->total == t1_payload.size(),
            "t-1 progress recorded");
        if (row != nullptr) {
            report(mirror.submit(aki::app::UpdateTransferProgress{
                        TransferId{"t-1"}, row->transferred, row->total}),
                "db: transfer t-1 progress enqueued");
        }
    }
    report(adapter.inject_transfer_completed(TransferId{"t-1"},
                TransferState::Completed),
        "inject_transfer_completed(t-1, Completed)");
    quiesce();
    {
        executor::comm::Snapshot<AppState> snapshot;
        report(load_snapshot(state_owner, snapshot), "snapshot readable");
        const Transfer* row = find_transfer(snapshot.value, TransferId{"t-1"});
        report(row != nullptr && row->state == TransferState::Completed,
            "t-1: -> Completed ("
                + std::string(row != nullptr ? enum_text(row->state) : "missing")
                + ")");
        report(row != nullptr && mirror.submit(aki::app::CompleteTransfer{
                    TransferId{"t-1"}, TransferState::Completed}),
            "db: transfer t-1 complete job group enqueued");
    }

    Transfer t2;
    t2.id = TransferId{"t-2"};
    t2.sender = DeviceId{"alpha-01"};
    t2.receiver = DeviceId{"local-1"};
    t2.file = aki::transfer::FileMetadata{"aborted.bin", 4096,
        "application/octet-stream"};
    t2.state = TransferState::Queued;
    report(adapter.inject_transfer_started(t2), "inject_transfer_started(t-2)");
    quiesce();
    {
        executor::comm::Snapshot<AppState> snapshot;
        report(load_snapshot(state_owner, snapshot), "snapshot readable");
        const Transfer* row = find_transfer(snapshot.value, TransferId{"t-2"});
        report(row != nullptr, "t-2 recorded (Queued)");
        if (row != nullptr) {
            report(mirror.submit(aki::app::UpsertTransfer{*row}),
                "db: transfer t-2 enqueued");
        }
    }
    report(adapter.inject_transfer_completed(TransferId{"t-2"},
                TransferState::Cancelled),
        "inject_transfer_completed(t-2, Cancelled)");
    quiesce();
    {
        executor::comm::Snapshot<AppState> snapshot;
        report(load_snapshot(state_owner, snapshot), "snapshot readable");
        const Transfer* row = find_transfer(snapshot.value, TransferId{"t-2"});
        report(row != nullptr && row->state == TransferState::Cancelled,
            "t-2: -> Cancelled ("
                + std::string(row != nullptr ? enum_text(row->state) : "missing")
                + ")");
        // 终态作业不入队等待结算：留作关闭排空「不丢作业」复验载荷（M2 退出-1）。
        report(row != nullptr && mirror.submit(aki::app::CompleteTransfer{
                    TransferId{"t-2"}, TransferState::Cancelled}),
            "db: transfer t-2 terminal + discard jobs enqueued");
    }
    {
        auto events = consume_events();
        report(events.size() == 5, "five events on the main path");
        if (events.size() == 5) {
            report(std::holds_alternative<aki::app::TransferStartedEvent>(
                       events[0].payload),
                "event 7 is TransferStarted (t-1)");
            report(std::holds_alternative<aki::app::TransferProgressEvent>(
                       events[1].payload),
                "event 8 is TransferProgress (t-1)");
            report(std::holds_alternative<aki::app::TransferCompletedEvent>(
                       events[2].payload),
                "event 9 is TransferCompleted (t-1)");
            report(std::holds_alternative<aki::app::TransferStartedEvent>(
                       events[3].payload),
                "event 10 is TransferStarted (t-2)");
            report(std::holds_alternative<aki::app::TransferCompletedEvent>(
                       events[4].payload),
                "event 11 is TransferCompleted (t-2)");
        }
    }

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
    }

    // ---- 步骤 7/7：受控关闭（设计第 8.3 节钩子顺序 -> EXEC-01 步骤 2~5）----
    std::printf("\n[step 7/7] controlled shutdown\n");
    const auto shutdown_report = executor_owner.shutdown([&] {
        // EXEC-01 步骤 1 钩子（设计第 8.3 节顺序 + 第 11.1 节 ③ 排空落点）：
        (void)transfers.request_cancel_all();  // ① 取消在途可取消任务（无在飞时幂等）
        (void)transfers.flush(2s);             // ② flush 各 Manager 至泵静止并消费 future
        (void)devices.flush(2s);
        (void)conversations.flush(2s);
        (void)messages.flush(2s);
        adapter.set_sink(nullptr);             // ③ 停 Adapter 投递
        adapter.stop_discovery();
        state_owner.close();                   // ④ comm 关闭（主线程 = owner 上下文）
        // ⑤ DB 排空位于钩子序列末尾（close() 之后、EXEC-01 步骤 2/3 之前，
        //    §11.1 ③）：close 的排空让最后一批更新被接受并入队 DB 作业。
        db->request_drain();
        report(wait_until([&] { return db->drain_completed(); }, 3s),
            "database worker drained within budget");
    });
    report(shutdown_report.fully_stopped(),
        "shutdown fully_stopped (Completed + lifecycle Stopped + wait_timeout_count==0)");
    report(shutdown_report.blocking_workers_requested == 1
            && shutdown_report.blocking_workers_stopped == 1,
        "one blocking worker requested and stopped (handle owned by ExecutorOwner)");
    report(state_owner.is_closed(), "AppStateOwner closed");
    report(!adapter.inject_message_received(reply),
        "injection after delivery stopped is rejected");

    // 复验宿主路径排空不丢作业（M2 退出-1）：已 admit 作业全部完成、无拒绝、
    // 无失败，future 全部结算成功（RULE-09：结果可见，不静默）。
    report(db->drain_completed(), "database worker drain completed");
    report(!db->drain_budget_exhausted(), "drain budget not exhausted");
    {
        int settle_failures = 0;
        for (auto& future : mirror.futures) {
            try {
                future.get();
            } catch (const std::exception& error) {
                ++settle_failures;
                std::printf("    [db job failure] %s\n", error.what());
            }
        }
        report(settle_failures == 0, "all admitted db jobs settled successfully");
    }
    report(db->completed_count() == mirror.admitted,
        "db jobs completed == admitted (" + std::to_string(db->completed_count())
            + " == " + std::to_string(mirror.admitted) + ", zero loss)");
    report(db->failed_count() == 0, "no failed db jobs");
    report(mirror.enqueue_rejected == 0 && db->rejected_count() == 0,
        "no enqueue rejections");

    // ---- 重启恢复（session B）：重新 open 后逐域断言一致（SCOPE-09 / 退出-1）----
    std::printf("\n[restart] reopen data root and assert per-domain consistency\n");
    RecoveryResult reopened;
    try {
        reopened = perform_startup_recovery(run_root);
    } catch (const std::exception& error) {
        report(false, std::string("reopen recovery failed: ") + error.what());
    }
    if (reopened.store != nullptr) {
        report(reopened.diagnostics.migrations_applied == 0,
            "migration idempotent on reopen (0 steps applied)");
        report(reopened.diagnostics.tmp_orphans_removed == 0,
            "no tmp orphans after clean shutdown");
        report(reopened.state.devices == expected.devices.devices,
            "devices consistent after restart (identity + trust state,"
            " presence recovered Offline)");
        report(reopened.state.conversations
                == expected.conversations.conversations,
            "conversations consistent after restart");
        report(reopened.state.messages == expected.messages.messages,
            "messages consistent after restart (incl. delivery final state)");
        report(reopened.state.transfers
                == expected.transfers.transfers,
            "transfer history consistent after restart");
        // Completed 文件本体与回写位（M2-06 契约，经 SQL 断言）。
        report(std::filesystem::exists(run_root + "/files/t-1/notes.txt"),
            "completed file landed at files/t-1/notes.txt");
        if (reopened.repositories != nullptr) {
            aki::persistence::Statement stored =
                reopened.repositories->database.prepare(
                    "SELECT stored_relative_path, stored_sha256,"
                    " stored_size_bytes FROM transfer WHERE transfer_id = 't-1';");
            const bool has_row = stored.step();
            report(has_row
                    && stored.column_text(0) == "files/t-1/notes.txt"
                    && stored.column_text(1)
                        == aki::persistence::sha256_hex(bytes_of(t1_payload))
                    && static_cast<std::uint64_t>(stored.column_int64(2))
                        == t1_payload.size(),
                "t-1 stored_* writeback columns consistent (path + sha256 + size)");
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
    // 子目录、不自动清理）——损坏 DB 干净失败用例（recovery.corrupt_db_clean_
    // failure）与手动复跑真实数据根依赖该模式；演示断言假定空根。
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
