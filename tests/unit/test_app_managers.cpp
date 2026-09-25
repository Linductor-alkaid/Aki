// M1-05：Device / Conversation / Message / Transfer Manager 骨架测试（DEC-008）。
//
// 覆盖（验收标准 ①②，设计第 8.3 节契约）：
//   - 10 类 Sink 方法（M3-05 DEC-006 映射 4 新增失败面 send_failed，不产
//     主路径事件）经 RouterSink 路由 → Manager 排空 → AppState 快照与必达
//     事件主路径 FIFO；connected/disconnected 扇出（DM presence + CM 会话推导，
//     主路径事件只投递一次）；ensure_conversation 显式建会话；
//   - 并发入队不丢（单飞泵丢失唤醒防护）；
//   - DOD-02 六项沿 Manager 任务路径：正常完成（用例 1/2）、任务异常（Adapter
//     抛出经 future + task_exception_count 可见且泵自愈）、提交拒绝
//     （max_in_flight_tasks 准入 + 收件箱满，均可见）、执行中取消
//     （M4-04 语义：事件驱动会话——续接抑制 + Adapter 命令 + 会话回收；IO
//     路径的执行中取消见 test_transfer_send_path）、超时
//     （独立 owner 排队软超时击杀排空任务且存量不丢）、shutdown（第 8.3 节
//     关闭钩子顺序 → fully_stopped）；
//   - 迟到事件不复活终态（RULE-08，退出-3）：迟到的进度/终态宣告、迟到的
//     送达/发送失败回报经 Manager 路由后被状态机应用层拒绝（updates_rejected）。
//   - M4-03 图片面（设计 §6.1/DEC-010）：出站路由与 DeliveryState 正向链/
//     终态幂等、图片路径任务异常（DOD-02）、发送侧准入闸门两向失败路径
//     （先传输准入、后发消息——传输准入失败零 send + 消息行 Failed；消息
//     准入失败 cancel_transfer 到达 Adapter）、闸门 enqueue 级判据（重复
//     TransferId 是 handler 级拒绝、闸门不可见——消息照常发送）、两级补偿
//     降级（补偿 Failed 行/补偿取消被收件箱拒 → RowLost/CancelLost 经编排
//     返回值可见，AGENTS 规则 10）、运行期零传导（消息 Delivered
//     与传输终态正交、接收侧一律 Delivered）。
//
// 每个用例持有独立的 ExecutorOwner（AGENTS 规则 7/8；设计第 8.2 节落点说明：
// 被测对象即组合根的一部分，EXEC-01 五步由用例显式驱动）。并发入队者经
// executor 任务承载（AGENTS 规则 2）；Catch2 断言只在主线程。
#include "app/application/image_flow.hpp"
#include "app/application/router_sink.hpp"
#include "app/lifecycle/executor_owner.hpp"
#include "app/state/app_state_owner.hpp"
#include "heyaki/adapter/fake_heyaki_adapter.hpp"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;

using aki::app::AppEvent;
using aki::app::AppState;
using aki::app::AppStateOwner;
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
using aki::app::TransferCompletedEvent;
using aki::app::TransferManagerOptions;
using aki::app::TransferManager;
using aki::app::TransferProgressEvent;
using aki::app::TransferStartedEvent;

using aki::conversation::ConversationId;
using aki::conversation::ConversationState;
using aki::conversation::DeliveryState;
using aki::conversation::Message;
using aki::conversation::MessageId;
using aki::conversation::MessageType;
using aki::device::ConnectionPath;
using aki::device::DeviceId;
using aki::device::DiscoveredDevice;
using aki::device::DiscoveryMethod;
using aki::device::PresenceState;
using aki::device::TrustState;
using aki::heyaki::FakeHeyakiAdapter;
using aki::heyaki::HeyakiAdapter;
using aki::heyaki::HeyakiAdapterSink;
using aki::transfer::FileMetadata;
using aki::transfer::Transfer;
using aki::transfer::TransferId;
using aki::transfer::TransferState;

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

DiscoveredDevice make_discovered(std::string id, TrustState trust = TrustState::Unknown) {
    DiscoveredDevice device;
    device.identity.id = DeviceId{std::move(id)};
    device.identity.display_name = device.identity.id.value;
    device.identity.trust_state = trust;
    device.identity.presence = PresenceState::Online;
    device.method = DiscoveryMethod::LanDiscovery;
    return device;
}

Message make_message(std::string id) {
    Message message;
    message.id = MessageId{std::move(id)};
    message.sender = DeviceId{"alpha"};
    message.receiver = DeviceId{"beta"};
    message.type = MessageType::Text;
    message.state = DeliveryState::Sent;  // Manager 收到事件后强制记录 Delivered。
    return message;
}

Transfer make_transfer(std::string id, TransferState state) {
    Transfer transfer;
    transfer.id = TransferId{std::move(id)};
    transfer.sender = DeviceId{"alpha"};
    transfer.receiver = DeviceId{"beta"};
    transfer.file = FileMetadata{"model.gguf", 1024, "application/octet-stream"};
    transfer.total = 1024;
    transfer.state = state;
    return transfer;
}

FileMetadata make_file() {
    return FileMetadata{"model.gguf", 1024, "application/octet-stream"};
}

// 可编程出站行为的 Adapter（任务异常 / 发送失败 / 排空期门控）。
struct GateState {
    std::mutex mutex;
    std::condition_variable condition;
    bool open = false;

    void open_gate() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = true;
        }
        condition.notify_all();
    }

    void await() {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return open; });
    }
};

struct StubAdapter final : HeyakiAdapter {
    std::shared_ptr<GateState> send_gate;      // 设置后 send_text_message 阻塞。
    std::shared_ptr<GateState> discovery_gate; // 设置后 start_discovery 阻塞。
    std::shared_ptr<GateState> transfer_gate;  // 设置后 start_file_transfer 阻塞
                                               //（M4-03 闸门用例：占住 TM 泵）。
    std::atomic<bool> throw_on_send{false};
    std::atomic<bool> fail_send{false};
    std::atomic<int> send_entered{0};
    std::atomic<int> transfer_entered{0};  // start_file_transfer 已进入（闸门
                                           // 用例的确定性同步点）。

    struct SentText {
        DeviceId to;
        MessageId message_id;
        std::string text;
    };
    std::vector<SentText> sent;

    struct SentImage {
        DeviceId to;
        MessageId message_id;
        FileMetadata file;
        TransferId transfer_id;
    };
    std::vector<SentImage> sent_images;
    std::vector<TransferId> cancelled_transfers;  // M4-03 闸门用例断言面
                                                  //（仅泵静止后读取）。
    std::atomic<int> cancel_entered{0};  // cancel_transfer 已进入（跨上下文
                                         // 同步点；vector 本体不加锁）。

    void set_sink(HeyakiAdapterSink*) noexcept {}

    bool start_discovery(DiscoveryMethod) override {
        if (discovery_gate) {
            discovery_gate->await();
        }
        return true;
    }

    void stop_discovery() override {}

    bool send_text_message(const DeviceId& to, const MessageId& message_id,
        std::string_view text) override {
        send_entered.fetch_add(1);
        if (send_gate) {
            send_gate->await();
        }
        if (throw_on_send.load()) {
            throw std::runtime_error("boom");
        }
        if (fail_send.load()) {
            return false;
        }
        sent.push_back(SentText{to, message_id, std::string(text)});
        return true;
    }

    // 图片出站沿文本同型语义（M4-03）：gate/throw/fail 行为对称。
    bool send_image_message(const DeviceId& to, const MessageId& message_id,
        const FileMetadata& file, const TransferId& transfer_id) override {
        send_entered.fetch_add(1);
        if (send_gate) {
            send_gate->await();
        }
        if (throw_on_send.load()) {
            throw std::runtime_error("boom");
        }
        if (fail_send.load()) {
            return false;
        }
        sent_images.push_back(SentImage{to, message_id, file, transfer_id});
        return true;
    }

    bool start_file_transfer(const DeviceId&, const TransferId&,
        const FileMetadata&, const std::filesystem::path&) override {
        transfer_entered.fetch_add(1);
        if (transfer_gate) {
            transfer_gate->await();
        }
        return true;
    }
    bool pause_transfer(const TransferId&) override { return true; }
    bool resume_transfer(const TransferId&) override { return true; }
    bool cancel_transfer(const TransferId& transfer_id) override {
        cancel_entered.fetch_add(1);
        cancelled_transfers.push_back(transfer_id);
        return true;
    }
};

// ---- 组合根（设计第 8.3 节装配顺序）：ExecutorOwner.initialize → AppStateOwner
// ---- → 四 Manager（构造注入 executor/owner/adapter/预算）→ RouterSink 注册。
template <typename AdapterT>
struct BasicStack {
    explicit BasicStack(ExecutorOwner::Options host_options = {},
        ManagerPumpOptions device_pump = {},
        ConversationManagerOptions conversation_options = {},
        MessageManagerOptions message_options = {},
        TransferManagerOptions transfer_options = {})
        : host([&] {
              // 固定线程池（脱离 runner 硬件决定性）：M1 会话骨架
              // session_loop 在池 worker 上 sleep 轮询（每个活跃会话停占
              // 一个 worker，M4-04 真实传输循环替换）。自适应池在 2 vCPU
              // CI runner 上仅 2 worker——闸门用例的双占位会话将其全部
              // 停占，TM 排空任务（flush 哨兵）永无调度 → flush 超时失败
              // （PR 34 首两轮 CI 五档红、~915s 的根因；2 线程池本地强制
              // 复现证实）。测试固定 4 worker：双会话停占 2 个，排空与
              // shutdown 路径恒可调度。生产侧 2 核设备同饥饿风险登记为
              // M4-02 观察项③，随真实传输循环收口。
              auto pinned = std::move(host_options);
              pinned.executor_config.min_threads = 4;
              pinned.executor_config.max_threads = 4;
              return pinned;
          }()) {
        if (device_pump.name.empty()) {
            device_pump.name = "aki.dm";
        }
        if (conversation_options.pump.name.empty()) {
            conversation_options.pump.name = "aki.cm";
        }
        if (message_options.pump.name.empty()) {
            message_options.pump.name = "aki.mm";
        }
        if (message_options.local_device.empty()) {
            message_options.local_device = DeviceId{"local-1"};
        }

        if (transfer_options.pump.name.empty()) {
            transfer_options.pump.name = "aki.tm";
        }
        devices.emplace(host.executor_owner.executor(), state_owner, adapter,
            std::move(device_pump));
        conversations.emplace(host.executor_owner.executor(), state_owner,
            std::move(conversation_options));
        messages.emplace(host.executor_owner.executor(), state_owner, adapter,
            std::move(message_options));
        transfers.emplace(host.executor_owner.executor(), state_owner, adapter,
            std::move(transfer_options));
        router.emplace(*devices, *conversations, *messages, *transfers);
        adapter.set_sink(&*router);
    }

    BasicStack(const BasicStack&) = delete;
    BasicStack& operator=(const BasicStack&) = delete;

    ~BasicStack() {
        // 兜底幂等关闭（显式断言仍由用例负责）：防止未取消的会话任务阻塞
        // shutdown 排空（EXEC-01 步骤 4）。
        if (host.initialized && !host.executor_owner.is_shutdown()) {
            (void)transfers->request_cancel_all();
            (void)transfers->flush(1s);
            (void)devices->flush(200ms);
            (void)conversations->flush(200ms);
            (void)messages->flush(200ms);
            (void)host.executor_owner.shutdown();
        }
    }

    struct Host {
        explicit Host(ExecutorOwner::Options options)
            : executor_owner(std::move(options)) {}
        ExecutorOwner executor_owner;
        bool initialized = executor_owner.initialize();
    };

    Host host;
    AppStateOwner state_owner{};
    AdapterT adapter{};
    std::optional<DeviceManager> devices;
    std::optional<ConversationManager> conversations;
    std::optional<MessageManager> messages;
    std::optional<TransferManager> transfers;
    std::optional<RouterSink> router;
};

using AppStack = BasicStack<FakeHeyakiAdapter>;

// 消息域最小组合（任务异常 / 提交拒绝 / 超时 / 迟到送达用例）。
struct MessageOnlyStack {
    explicit MessageOnlyStack(ExecutorOwner::Options host_options = {},
        ManagerPumpOptions pump = {}, bool fail_send = false,
        bool throw_send = false)
        : host(std::move(host_options)) {
        if (pump.name.empty()) {
            pump.name = "aki.mm";
        }
        MessageManagerOptions options;
        options.pump = pump;
        options.local_device = DeviceId{"local-1"};
        messages.emplace(host.executor_owner.executor(), state_owner, adapter,
            std::move(options));
        adapter.fail_send.store(fail_send);
        adapter.throw_on_send.store(throw_send);
    }

    MessageOnlyStack(const MessageOnlyStack&) = delete;
    MessageOnlyStack& operator=(const MessageOnlyStack&) = delete;

    ~MessageOnlyStack() {
        if (host.initialized && !host.executor_owner.is_shutdown()) {
            (void)messages->flush(1s);
            (void)host.executor_owner.shutdown();
        }
    }

    struct Host {
        explicit Host(ExecutorOwner::Options options)
            : executor_owner(std::move(options)) {}
        ExecutorOwner executor_owner;
        bool initialized = executor_owner.initialize();
    };

    Host host;
    // DEC-009 ②：无 ConversationManager 的最小组合——消息远端会话经初始
    // 快照预置（FK 前置校验的权威来源，与 MM 默认前缀 conv- 一致）。
    AppStateOwner state_owner{aki::app::AppStateOwnerOptions{},
        [] {
            aki::app::AppState state;
            for (const auto& remote : {std::string{"alpha"},
                     std::string{"beta"}}) {
                aki::conversation::Conversation conversation;
                conversation.id =
                    aki::conversation::ConversationId{"conv-" + remote};
                conversation.local_device = DeviceId{"local-1"};
                conversation.remote_device = DeviceId{remote};
                state.conversations.conversations.push_back(conversation);
            }
            return state;
        }()};
    StubAdapter adapter{};
    std::optional<MessageManager> messages;
};

// 排空四个 Manager 至泵静止，再驱动状态 owner 至水位不变（用例内 owner 上下文
// = 主线程，单写者纪律）。
void quiesce(AppStateOwner& state_owner, DeviceManager& devices,
    ConversationManager& conversations, MessageManager& messages,
    TransferManager& transfers) {
    REQUIRE(devices.flush(2s));
    REQUIRE(conversations.flush(2s));
    REQUIRE(messages.flush(2s));
    REQUIRE(transfers.flush(2s));
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
}

void drain_until_idle(AppStateOwner& owner) {
    for (;;) {
        const auto watermark = [](const AppStateOwner& o) {
            return o.stats().updates_applied + o.stats().updates_rejected
                + o.stats().events_forwarded + o.stats().events_dropped;
        };
        const std::uint64_t before = watermark(owner);
        owner.drain();
        if (watermark(owner) == before) {
            return;
        }
    }
}

}  // namespace

// ---- 用例 1：10 类 Sink 方法路由（失败面不产主路径事件）、Store 归属、
// ---- 扇出与 FIFO（DOD-02 正常完成）----

TEST_CASE("RouterSink routes the ten sink methods to per-domain stores in FIFO order",
    "[unit][managers][dod02]") {
    AppStack stack;
    auto& owner = stack.state_owner;
    auto& fake = stack.adapter;
    const auto settle = [&] {
        quiesce(owner, *stack.devices, *stack.conversations, *stack.messages,
            *stack.transfers);
    };

    // 发现启停（DM 出站 → Adapter，业务在 Manager 上下文）。
    REQUIRE(stack.devices->start_discovery(DiscoveryMethod::LanDiscovery));
    REQUIRE(wait_until([&] { return fake.discovery_running(); }, 2s));

    // ① device discovered：UpsertDevice + DeviceDiscovered 事件。
    REQUIRE(fake.inject_device_discovered(make_discovered("dev-a")));
    settle();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.devices.devices.size() == 1);
        REQUIRE(snapshot.value.devices.devices.front().id == DeviceId{"dev-a"});
        REQUIRE(snapshot.value.conversations.conversations.empty());
    }

    // ② device connected（ensure 之前）：DM presence Online + 事件一次；
    //    CM 无自建会话记录 → 幂等空操作，会话 Store 仍为空。
    REQUIRE(fake.inject_device_connected(DeviceId{"dev-a"}, ConnectionPath::Lan));
    settle();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.devices.devices.front().presence == PresenceState::Online);
        REQUIRE(snapshot.value.conversations.conversations.empty());
    }

    // ensure_conversation（宿主显式建会话）：Active，id 确定性派生。
    REQUIRE(stack.conversations->ensure_conversation(DeviceId{"local-1"}, DeviceId{"dev-a"}));
    settle();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.conversations.conversations.size() == 1);
        REQUIRE(snapshot.value.conversations.conversations.front().id
            == ConversationId{"conv-dev-a"});
        REQUIRE(snapshot.value.conversations.conversations.front().state
            == ConversationState::Active);
    }

    // ③ device disconnected：presence Offline + 会话 Disconnected + 事件一次。
    REQUIRE(fake.inject_device_disconnected(DeviceId{"dev-a"}));
    settle();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.devices.devices.front().presence == PresenceState::Offline);
        REQUIRE(snapshot.value.conversations.conversations.front().state
            == ConversationState::Disconnected);
    }

    // ④ 重连（LAN → P2P，RULE-06 路径无关）：会话回到 Active，不新建会话。
    REQUIRE(fake.inject_device_connected(DeviceId{"dev-a"}, ConnectionPath::P2p));
    settle();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.conversations.conversations.size() == 1);
        REQUIRE(snapshot.value.conversations.conversations.front().state
            == ConversationState::Active);
    }

    // ⑤ message received：收到的消息本地记录 Delivered（设计第 6 节）。
    // DEC-009 ②：消息 FK 前置校验——先 ensure sender 的会话。
    REQUIRE(stack.conversations->ensure_conversation(
        DeviceId{"local-1"}, DeviceId{"alpha"}));
    settle();
    REQUIRE(fake.inject_message_received(make_message("m-in")));
    settle();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.messages.messages.size() == 1);
        REQUIRE(snapshot.value.messages.messages.front().state == DeliveryState::Delivered);
    }

    // ⑥ send_text（MM 出站）：Adapter admission 成功 → 本地 Sent。
    REQUIRE(stack.conversations->ensure_conversation(
        DeviceId{"local-1"}, DeviceId{"beta"}));
    settle();
    REQUIRE(stack.messages->send_text(DeviceId{"beta"}, MessageId{"m-out"}, "hello"));
    settle();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        bool seen = false;
        for (const auto& message : snapshot.value.messages.messages) {
            if (message.id == MessageId{"m-out"}) {
                seen = true;
                REQUIRE(message.state == DeliveryState::Sent);
                REQUIRE(message.sender == DeviceId{"local-1"});
            }
        }
        REQUIRE(seen);
        REQUIRE(snapshot.value.messages.messages.size() == 2);
        REQUIRE(fake.sent_texts().size() == 1);
    }

    // on_message_delivered：SetDeliveryState Sent -> Delivered。
    REQUIRE(fake.inject_message_delivered(ConversationId{"conv-beta"}, MessageId{"m-out"}));
    settle();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        for (const auto& message : snapshot.value.messages.messages) {
            if (message.id == MessageId{"m-out"}) {
                REQUIRE(message.state == DeliveryState::Delivered);
            }
        }
    }

    // send_failed（sink 第 10 方法，DEC-006 映射 4 失败面）：RouterSink →
    // MM.enqueue_message_send_failed → SetDeliveryState(Failed)（Sent ->
    // Failed 合法边）。Fake 无此注入面（仅实现出站 Adapter），经 Sink 直驱；
    // 该路由不产生主路径事件（末尾 FIFO 空断言为证）。
    REQUIRE(stack.messages->send_text(DeviceId{"beta"}, MessageId{"m-fail"}, "hello"));
    settle();
    REQUIRE(stack.router->on_message_send_failed(
        ConversationId{"conv-beta"}, MessageId{"m-fail"}));
    settle();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        bool seen = false;
        for (const auto& message : snapshot.value.messages.messages) {
            if (message.id == MessageId{"m-fail"}) {
                seen = true;
                REQUIRE(message.state == DeliveryState::Failed);
            }
        }
        REQUIRE(seen);
    }

    // ⑦ transfer started/progress/completed（合法边 Transferring -> Completed）。
    REQUIRE(fake.inject_transfer_started(make_transfer("t-1", TransferState::Transferring)));
    settle();
    REQUIRE(fake.inject_transfer_progress(TransferId{"t-1"}, 512, 1024));
    settle();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.transfers.transfers.size() == 1);
        REQUIRE(snapshot.value.transfers.transfers.front().transferred == 512);
    }
    REQUIRE(fake.inject_transfer_completed(TransferId{"t-1"}, TransferState::Completed));
    settle();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.transfers.transfers.front().state == TransferState::Completed);
    }

    // ⑧ connection path changed：LatestMailbox 摘要（不落 Store）。
    REQUIRE(fake.inject_connection_path_changed(DeviceId{"dev-a"}, ConnectionPath::P2p,
        ConnectionPath::Relay));
    settle();
    ConnectionPath path = ConnectionPath::Unknown;
    REQUIRE(owner.try_load_connection_path(path));
    REQUIRE(path == ConnectionPath::Relay);

    // 必达事件主路径：10 个事件按注入顺序 FIFO，双投递事件各只投递一次。
    REQUIRE(owner.stats().updates_rejected == 0);
    const std::vector<bool (*)(const AppEvent&)> matches{
        [](const AppEvent& e) { return std::holds_alternative<DeviceDiscoveredEvent>(e.payload); },
        [](const AppEvent& e) { return std::holds_alternative<DeviceConnectedEvent>(e.payload); },
        [](const AppEvent& e) { return std::holds_alternative<DeviceDisconnectedEvent>(e.payload); },
        [](const AppEvent& e) { return std::holds_alternative<DeviceConnectedEvent>(e.payload); },
        [](const AppEvent& e) { return std::holds_alternative<MessageReceivedEvent>(e.payload); },
        [](const AppEvent& e) { return std::holds_alternative<MessageDeliveredEvent>(e.payload); },
        [](const AppEvent& e) { return std::holds_alternative<TransferStartedEvent>(e.payload); },
        [](const AppEvent& e) { return std::holds_alternative<TransferProgressEvent>(e.payload); },
        [](const AppEvent& e) { return std::holds_alternative<TransferCompletedEvent>(e.payload); },
        [](const AppEvent& e) { return std::holds_alternative<ConnectionPathChangedEvent>(e.payload); },
    };
    for (std::size_t i = 0; i < matches.size(); ++i) {
        AppEvent event;
        REQUIRE(owner.try_receive_event(event));
        INFO("event index: " << i);
        REQUIRE(event.sequence == i + 1);
        REQUIRE(matches[i](event));
    }
    AppEvent extra;
    REQUIRE_FALSE(owner.try_receive_event(extra));
    REQUIRE(owner.stats().events_dropped == 0);

    // EXEC-01：干净关闭（无会话、泵静止 → fully_stopped）。
    const auto report = stack.host.executor_owner.shutdown();
    REQUIRE(report.fully_stopped());
}

// ---- 用例 2：并发入队不丢（单飞泵丢失唤醒防护；DOD-02 正常完成·并发面）----

TEST_CASE("Concurrent senders never lose a work item (single-flight pump)",
    "[unit][managers][concurrency][dod02]") {
    AppStack stack;
    auto& owner = stack.state_owner;
    auto& messages = *stack.messages;
    auto& executor = stack.host.executor_owner.executor();

    // DEC-009 ②：出站消息远端会话先行（FK 前置校验）。
    REQUIRE(stack.conversations->ensure_conversation(
        DeviceId{"local-1"}, DeviceId{"beta"}));

    constexpr int kSenders = 4;
    constexpr int kPerSender = 25;
    std::atomic<int> rejected{0};

    std::vector<std::future<void>> senders;
    for (int t = 0; t < kSenders; ++t) {
        senders.push_back(executor.submit_auto([&, t] {
            for (int i = 0; i < kPerSender; ++i) {
                const auto id = "m-" + std::to_string(t) + "-" + std::to_string(i);
                if (!messages.send_text(DeviceId{"beta"}, MessageId{id}, "x")) {
                    rejected.fetch_add(1);
                }
            }
        }));
    }
    // 主线程交错入队，制造与排空任务的入队/接管竞争（丢失唤醒窗口）。
    for (int i = 0; i < 10; ++i) {
        const auto id = "m-main-" + std::to_string(i);
        REQUIRE(messages.send_text(DeviceId{"beta"}, MessageId{id}, "x"));
    }
    for (auto& sender : senders) {
        sender.get();
    }

    REQUIRE(messages.flush(5s));
    REQUIRE(rejected.load() == 0);
    const auto stats = messages.stats();
    REQUIRE(stats.enqueued == kSenders * kPerSender + 10);
    REQUIRE(stats.inbox_rejections == 0);
    REQUIRE(stats.handler_rejections == 0);
    REQUIRE(stats.processed == kSenders * kPerSender + 10);
    REQUIRE(stats.drain_failures == 0);

    drain_until_idle(owner);
    executor::comm::Snapshot<AppState> snapshot;
    REQUIRE(owner.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.messages.messages.size() == kSenders * kPerSender + 10);
    REQUIRE(stack.adapter.sent_texts().size() == kSenders * kPerSender + 10);

    const auto report = stack.host.executor_owner.shutdown();
    REQUIRE(report.fully_stopped());
}

// ---- 用例 3：DOD-02 任务异常——future + Executor 失败计数可见，泵自愈 ----

TEST_CASE("DOD-02 task exception on the manager drain path is visible and self-heals",
    "[unit][managers][dod02]") {
    MessageOnlyStack stack{ExecutorOwner::Options{}, ManagerPumpOptions{}, false, true};
    auto& owner = stack.state_owner;
    auto& messages = *stack.messages;
    auto& executor = stack.host.executor_owner.executor();


    const auto exceptions_before = executor.get_failure_status().task_exception_count;

    // 第一条消息：排空任务在 handler 内抛出（Adapter 异常穿透，泵不吞）；
    // flush 等待其终结并消费异常 future（计数 + 复位单飞，自愈不丢存量）。
    REQUIRE(messages.send_text(DeviceId{"beta"}, MessageId{"m-boom"}, "x"));
    REQUIRE(messages.flush(2s));
    stack.adapter.throw_on_send.store(false);

    // 第二条消息：自愈后的泵正常处理。
    REQUIRE(messages.send_text(DeviceId{"beta"}, MessageId{"m-ok"}, "x"));
    REQUIRE(messages.flush(2s));

    // 任务异常对 Executor 设施可见（AGENTS 规则 9：事实源是 Executor）。
    REQUIRE(wait_until([&] {
        return executor.get_failure_status().task_exception_count >= exceptions_before + 1;
    }, 2s));

    const auto stats = messages.stats();
    REQUIRE(stats.enqueued == 2);
    REQUIRE(stats.drain_failures == 1);
    REQUIRE(stats.task_submit_rejections == 0);
    REQUIRE(stats.spawn_count == 2);   // 失败后自愈重排。
    REQUIRE(stats.processed == 1);     // m-ok 成功处理；m-boom 未落地。

    drain_until_idle(owner);
    executor::comm::Snapshot<AppState> snapshot;
    REQUIRE(owner.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.messages.messages.size() == 1);
    REQUIRE(snapshot.value.messages.messages.front().id == MessageId{"m-ok"});

    const auto report = stack.host.executor_owner.shutdown();
    REQUIRE(report.fully_stopped());
}

// ---- 用例 4：DOD-02 提交拒绝——max_in_flight 准入 + 收件箱背压，均可见 ----

TEST_CASE("DOD-02 submit rejection: admission limit and inbox backpressure are visible",
    "[unit][managers][dod02]") {
    SECTION("max_in_flight_tasks exhaustion rejects the pump submission") {
        ExecutorOwner::Options host_options;
        host_options.executor_config.max_in_flight_tasks = 1;
        AppStack stack{std::move(host_options)};
        auto& owner = stack.state_owner;
        auto& messages = *stack.messages;

        // DEC-009 ②：出站消息远端会话先行，并排空 CM 泵（保持本用例的
        // 「干净 executor + 单准入槽」前提）。
        REQUIRE(stack.conversations->ensure_conversation(
            DeviceId{"local-1"}, DeviceId{"beta"}));
        REQUIRE(stack.conversations->flush(2s));

        // 饱和唯一准入槽位：排空任务的提交立即以 CapacityExhaustedException 就绪
        // （future 即时就绪，下一次消费时计数并自愈——拒绝可见，不静默）。
        auto saturate = stack.host.executor_owner.executor().submit_auto([] {
            std::this_thread::sleep_for(150ms);
        });
        REQUIRE(messages.send_text(DeviceId{"beta"}, MessageId{"m-1"}, "a"));
        REQUIRE(messages.stats().task_submit_rejections == 0);
        saturate.get();
        REQUIRE(messages.send_text(DeviceId{"beta"}, MessageId{"m-2"}, "b"));
        REQUIRE(messages.flush(2s));

        REQUIRE(wait_until([&] {
            return stack.host.executor_owner.executor().get_failure_status()
                       .capacity_exhausted_count
                >= 1;
        }, 2s));
        const auto stats = messages.stats();
        REQUIRE(stats.task_submit_rejections == 1);
        REQUIRE(stats.enqueued == 2);
        REQUIRE(stats.processed == 2);   // 自愈重排后存量不丢。
        REQUIRE(stats.inbox_rejections == 0);

        drain_until_idle(owner);
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.messages.messages.size() == 2);

        const auto report = stack.host.executor_owner.shutdown();
        REQUIRE(report.fully_stopped());
    }

    SECTION("full manager inbox rejects the enqueue through the sink result") {
        ManagerPumpOptions pump;
        pump.name = "aki.mm";
        pump.inbox_capacity = 1;   // RULE-09：有界收件箱，满即明确拒绝。
        MessageOnlyStack stack{ExecutorOwner::Options{}, pump};
        auto& messages = *stack.messages;

        auto gate = std::make_shared<GateState>();
        stack.adapter.send_gate = gate;

        // 第一条：入队并被排空任务取走，阻塞在 Adapter 门内（单飞在飞）。
        REQUIRE(messages.send_text(DeviceId{"beta"}, MessageId{"m-1"}, "a"));
        REQUIRE(wait_until([&] { return stack.adapter.send_entered.load() == 1; }, 2s));
        // 第二条：收件箱 1/1，admission 成功。
        REQUIRE(messages.send_text(DeviceId{"beta"}, MessageId{"m-2"}, "b"));
        // 第三条：收件箱满 → 明确拒绝（对 Sink/调用方可见，RULE-09）。
        REQUIRE_FALSE(messages.send_text(DeviceId{"beta"}, MessageId{"m-3"}, "c"));

        REQUIRE(messages.stats().inbox_rejections == 1);
        gate->open_gate();
        REQUIRE(messages.flush(2s));

        const auto stats = messages.stats();
        REQUIRE(stats.enqueued == 2);
        REQUIRE(stats.processed == 2);
        drain_until_idle(stack.state_owner);
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(stack.state_owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.messages.messages.size() == 2);

        const auto report = stack.host.executor_owner.shutdown();
        REQUIRE(report.fully_stopped());
    }
}

// ---- 用例 5：DOD-02 执行中取消（M4-04 语义：事件驱动会话，无池任务句柄；
// 取消 = 续接抑制 + Adapter 命令 + 会话回收；IO 路径的执行中取消见
// test_transfer_send_path）----

TEST_CASE("DOD-02 in-flight cancellation: transfer session observes the stop token",
    "[unit][managers][dod02]") {
    TransferManagerOptions transfer_options;
    transfer_options.sender = DeviceId{"local-1"};
    AppStack stack{ExecutorOwner::Options{}, ManagerPumpOptions{}, {},
        MessageManagerOptions{}, transfer_options};
    auto& owner = stack.state_owner;
    auto& fake = stack.adapter;
    auto& transfers = *stack.transfers;

    // 应用发起传输：会话登记（事件驱动状态机，DEC-011；无池任务派生）。
    REQUIRE(transfers.start_transfer(DeviceId{"beta"}, TransferId{"t-1"}, make_file()));
    REQUIRE(transfers.flush(2s));
    REQUIRE(wait_until([&] { return transfers.active_session_count() == 1; }, 2s));
    REQUIRE(fake.transfer_session_known("t-1"));
    quiesce(owner, *stack.devices, *stack.conversations, *stack.messages, transfers);
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.transfers.transfers.size() == 1);
        REQUIRE(snapshot.value.transfers.transfers.front().state == TransferState::Queued);
    }

    // 协作取消：Adapter 命令 + 会话收尾（续接抑制 + 回收）。
    REQUIRE(transfers.cancel_transfer(TransferId{"t-1"}));
    REQUIRE(transfers.flush(2s));
    REQUIRE(transfers.active_session_count() == 0);
    REQUIRE(transfers.cancelled_session_count() == 1);

    // Adapter 侧取消命令可见（SPI 幂等停止）。
    bool adapter_cancel_seen = false;
    for (const auto& command : fake.transfer_commands()) {
        if (command.kind == aki::heyaki::FakeHeyakiAdapter::TransferCommand::Kind::Cancel
            && command.transfer_id == TransferId{"t-1"}) {
            adapter_cancel_seen = true;
        }
    }
    REQUIRE(adapter_cancel_seen);

    // 传输终态由事件宣告：Queued -> Cancelled 是合法边。
    REQUIRE(fake.inject_transfer_completed(TransferId{"t-1"}, TransferState::Cancelled));
    quiesce(owner, *stack.devices, *stack.conversations, *stack.messages, transfers);
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.transfers.transfers.front().state == TransferState::Cancelled);
    }

    // 幂等：会话已回收，再次取消不再重复计数（会话表已空）。
    REQUIRE(transfers.cancel_transfer(TransferId{"t-1"}));
    REQUIRE(transfers.flush(2s));
    REQUIRE(transfers.cancelled_session_count() == 1);
    REQUIRE(transfers.active_session_count() == 0);

    const auto report = stack.host.executor_owner.shutdown();
    REQUIRE(report.fully_stopped());
}

// ---- 用例 5b：重复 TransferId 拒绝——不替换在飞会话记录 ----

TEST_CASE("Duplicate transfer id is rejected without replacing the live session",
    "[unit][managers][dod02]") {
    // StubAdapter 的 start_file_transfer 恒接受：重复 id 的防线必须落在
    // Manager 自身的会话表（拒绝替换在飞 SessionRecord），不能借道 Adapter
    // 去重或状态机非法边（FakeHeyakiAdapter 二者兼备，测不到本防线）。
    TransferManagerOptions transfer_options;
    transfer_options.sender = DeviceId{"local-1"};
    BasicStack<StubAdapter> stack{ExecutorOwner::Options{}, ManagerPumpOptions{}, {},
        MessageManagerOptions{}, transfer_options};
    auto& owner = stack.state_owner;
    auto& transfers = *stack.transfers;

    REQUIRE(transfers.start_transfer(DeviceId{"beta"}, TransferId{"t-1"}, make_file()));
    REQUIRE(transfers.flush(2s));
    REQUIRE(wait_until([&] { return transfers.active_session_count() == 1; }, 2s));

    // 重复 id：命令入队 admission 成功（异步接口，返回值只代表收件箱受理），
    // 但业务侧拒绝（RULE-09，经 handler_rejections 可见）——在飞会话记录不被
    // 替换，旧任务仍可取消，future 仍由 Manager 持有消费。
    REQUIRE(transfers.start_transfer(DeviceId{"beta"}, TransferId{"t-1"}, make_file()));
    REQUIRE(transfers.flush(2s));
    REQUIRE(transfers.stats().handler_rejections == 1);
    REQUIRE(transfers.active_session_count() == 1);

    // 本地记录不受重复发起影响：仍是一条 Queued 记录（守卫先于 Upsert）。
    quiesce(owner, *stack.devices, *stack.conversations, *stack.messages, transfers);
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.transfers.transfers.size() == 1);
        REQUIRE(snapshot.value.transfers.transfers.front().state == TransferState::Queued);
    }

    // 被拒绝的重复发起未留下第二会话：一次取消即回收干净，关闭无悬挂任务。
    REQUIRE(transfers.cancel_transfer(TransferId{"t-1"}));
    REQUIRE(transfers.flush(2s));
    REQUIRE(transfers.active_session_count() == 0);
    REQUIRE(transfers.cancelled_session_count() == 1);

    const auto report = stack.host.executor_owner.shutdown();
    REQUIRE(report.fully_stopped());
}

// ---- 用例 6：DOD-02 超时——排队软超时击杀排空任务，存量不丢 ----

TEST_CASE("DOD-02 timeout: queued drain killed by soft timeout self-heals without loss",
    "[unit][managers][dod02]") {
    ExecutorOwner::Options host_options;
    host_options.executor_config.max_threads = 1;      // 单 worker：排空任务必滞留排队。
    host_options.executor_config.task_timeout_ms = 40; // config 级排队软超时。
    MessageOnlyStack stack{std::move(host_options)};
    auto& owner = stack.state_owner;
    auto& messages = *stack.messages;
    auto& executor = stack.host.executor_owner.executor();



    const auto timeout_before = executor.get_failure_status().timeout_count;

    // 饱和唯一 worker：排空任务排队超过 task_timeout_ms → 被软超时击杀（永不
    // 运行，不会自复位单飞标志）。
    // 排队软超时在出队时按 submit→dequeue 时长判定（运行中任务不中断），
    // 冷启动 worker 在 CI 负载下领取首个任务可能超过 40ms，饱和任务自身会被
    // 击杀（PR #11 Windows job 实测）。因此先等待饱和任务确认开跑再投递排空
    // 任务：started 只可能在提交后 task_timeout_ms 内翻转，等待窗口取 100ms
    // 即可区分"已被击杀"与"仍在排队"；被击杀则消费超时异常并重试（worker
    // 热身后领取延迟趋近于零），至多 3 次。
    std::atomic<bool> saturate_started{false};
    auto submit_saturate = [&executor, &saturate_started] {
        saturate_started.store(false, std::memory_order_release);
        return executor.submit_auto([&saturate_started] {
            saturate_started.store(true, std::memory_order_release);
            std::this_thread::sleep_for(250ms);
        });
    };
    auto saturate = submit_saturate();
    for (int attempt = 0;
         attempt < 3 && !wait_until([&] {
             return saturate_started.load(std::memory_order_acquire);
         }, 100ms); ++attempt) {
        try {
            saturate.get();  // 消费排队软超时的 TimedOutException 后重试。
        } catch (const executor::TimedOutException&) {
        }
        saturate = submit_saturate();
    }
    REQUIRE(saturate_started.load());  // 饱和任务已开跑（worker 被占住）。
    REQUIRE(messages.send_text(DeviceId{"beta"}, MessageId{"m-1"}, "a"));
    saturate.get();

    // 下一次入队消费超时 future（计数 + 复位 + 自愈重排），存量 m-1 不丢。
    REQUIRE(messages.send_text(DeviceId{"beta"}, MessageId{"m-2"}, "b"));
    REQUIRE(messages.flush(2s));

    REQUIRE(wait_until([&] {
        return executor.get_failure_status().timeout_count >= timeout_before + 1;
    }, 2s));

    const auto stats = messages.stats();
    REQUIRE(stats.drain_timeouts == 1);
    REQUIRE(stats.drain_failures == 1);
    REQUIRE(stats.enqueued == 2);
    REQUIRE(stats.processed == 2);   // 被击杀排空的存量经重排完成。

    drain_until_idle(owner);
    executor::comm::Snapshot<AppState> snapshot;
    REQUIRE(owner.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.messages.messages.size() == 2);
    REQUIRE(stack.adapter.sent.size() == 2);

    const auto report = stack.host.executor_owner.shutdown();
    REQUIRE(report.fully_stopped());
}

// ---- 用例 7：DOD-02 shutdown——第 8.3 节关闭钩子顺序（在飞会话 + 存量工作）----

TEST_CASE("DOD-02 shutdown: the composition hook cancels, drains, and closes in order",
    "[unit][managers][dod02]") {
    TransferManagerOptions transfer_options;
    transfer_options.sender = DeviceId{"local-1"};
    AppStack stack{ExecutorOwner::Options{}, ManagerPumpOptions{}, {},
        MessageManagerOptions{}, transfer_options};
    auto& owner = stack.state_owner;
    auto& fake = stack.adapter;

    // 造势：会话 + 事件 + 未排空的发送存量。
    REQUIRE(stack.conversations->ensure_conversation(DeviceId{"local-1"}, DeviceId{"dev-a"}));
    REQUIRE(fake.inject_device_discovered(make_discovered("dev-a")));
    REQUIRE(fake.inject_device_connected(DeviceId{"dev-a"}, ConnectionPath::Lan));
    REQUIRE(stack.devices->flush(2s));
    REQUIRE(stack.conversations->flush(2s));
    REQUIRE(stack.transfers->start_transfer(DeviceId{"dev-a"}, TransferId{"t-1"}, make_file()));
    REQUIRE(stack.transfers->flush(2s));
    REQUIRE(wait_until([&] { return stack.transfers->active_session_count() == 1; }, 2s));
    REQUIRE(stack.messages->send_text(DeviceId{"dev-a"}, MessageId{"m-pending"}, "hello"));

    // EXEC-01 步骤 1 钩子（设计第 8.3 节顺序）：取消 → flush 排空/消费 future →
    // 停 Adapter 投递 → AppStateOwner.close()（本线程即 owner 上下文）。
    const auto report = stack.host.executor_owner.shutdown([&] {
        CHECK(stack.transfers->request_cancel_all());
        CHECK(stack.transfers->flush(2s));
        CHECK(stack.devices->flush(2s));
        CHECK(stack.conversations->flush(2s));
        CHECK(stack.messages->flush(2s));
        fake.set_sink(nullptr);
        fake.stop_discovery();
        owner.close();
    });

    REQUIRE(report.fully_stopped());
    REQUIRE(stack.transfers->cancelled_session_count() == 1);
    REQUIRE(stack.transfers->active_session_count() == 0);
    REQUIRE(owner.is_closed());

    // 停止投递后注入明确拒绝（EXEC-01/AGENTS 规则 10：不静默）。
    REQUIRE_FALSE(fake.inject_message_received(make_message("m-late")));

    // 末次快照可读：存量已在钩子内排空并经 close 发布。
    executor::comm::Snapshot<AppState> snapshot;
    REQUIRE(owner.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.devices.devices.size() == 1);
    REQUIRE(snapshot.value.devices.devices.front().presence == PresenceState::Online);
    REQUIRE(snapshot.value.conversations.conversations.front().state
        == ConversationState::Active);
    REQUIRE(snapshot.value.transfers.transfers.front().state == TransferState::Queued);
    bool pending_sent = false;
    for (const auto& message : snapshot.value.messages.messages) {
        if (message.id == MessageId{"m-pending"}) {
            pending_sent = true;
            REQUIRE(message.state == DeliveryState::Sent);
        }
    }
    REQUIRE(pending_sent);
}

// ---- 用例 8：迟到事件不复活终态（RULE-08，退出-3；经 Manager 路由）----

TEST_CASE("Late transfer events cannot revive a terminal transfer (RULE-08)",
    "[unit][managers][late-events]") {
    AppStack stack;
    auto& owner = stack.state_owner;
    auto& fake = stack.adapter;
    const auto settle = [&] {
        quiesce(owner, *stack.devices, *stack.conversations, *stack.messages,
            *stack.transfers);
    };

    REQUIRE(fake.inject_transfer_started(make_transfer("t-1", TransferState::Transferring)));
    settle();
    REQUIRE(fake.inject_transfer_completed(TransferId{"t-1"}, TransferState::Cancelled));
    settle();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.transfers.transfers.front().state == TransferState::Cancelled);
    }

    // 迟到的进度：Sink/收件箱 admission 成功，状态机应用层拒绝（可观测）。
    REQUIRE(fake.inject_transfer_progress(TransferId{"t-1"}, 999, 1024));
    settle();
    // 迟到的终态宣告：不得让 Cancelled 变成 Completed。
    REQUIRE(fake.inject_transfer_completed(TransferId{"t-1"}, TransferState::Completed));
    settle();

    executor::comm::Snapshot<AppState> snapshot;
    REQUIRE(owner.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.transfers.transfers.front().state == TransferState::Cancelled);
    REQUIRE(snapshot.value.transfers.transfers.front().transferred == 0);
    REQUIRE(owner.stats().updates_rejected == 2);

    const auto report = stack.host.executor_owner.shutdown();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("Late delivery report cannot revive a failed message (RULE-08)",
    "[unit][managers][late-events]") {
    MessageOnlyStack stack{ExecutorOwner::Options{}, ManagerPumpOptions{},
        /*fail_send=*/true};
    auto& owner = stack.state_owner;
    auto& messages = *stack.messages;


    // Adapter 拒绝发送 → 本地记录 Failed（终态，Queued -> Failed 合法边）。
    REQUIRE(messages.send_text(DeviceId{"beta"}, MessageId{"m-f"}, "x"));
    REQUIRE(messages.flush(2s));
    drain_until_idle(owner);
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.messages.messages.size() == 1);
        REQUIRE(snapshot.value.messages.messages.front().state == DeliveryState::Failed);
    }

    // 迟到的送达回报：Failed -> Delivered 非法 → 状态机应用层拒绝。
    REQUIRE(messages.enqueue_message_delivered(ConversationId{"conv-beta"}, MessageId{"m-f"}));
    REQUIRE(messages.flush(2s));

    drain_until_idle(owner);
    executor::comm::Snapshot<AppState> snapshot;
    REQUIRE(owner.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.messages.messages.front().state == DeliveryState::Failed);
    REQUIRE(owner.stats().updates_rejected == 1);

    const auto report = stack.host.executor_owner.shutdown();
    REQUIRE(report.fully_stopped());
}

// DEC-006 映射 4 失败回报面：MM.enqueue_message_send_failed →
// SetDeliveryState(Failed)（Sent -> Failed 合法边）；终态重复回报幂等 no-op；
// 迟到回报不复活 Delivered（RULE-08：handler 仅通道 admission，拒绝经
// owner updates_rejected 增量观测）；空载荷 handler 有界校验拒绝可见。
TEST_CASE("Send-failure report maps to Failed and a late report cannot revive Delivered (RULE-08)",
    "[unit][managers][late-events]") {
    MessageOnlyStack stack;
    auto& owner = stack.state_owner;
    auto& messages = *stack.messages;

    // 正向映射：Sent -> Failed。
    REQUIRE(messages.send_text(DeviceId{"beta"}, MessageId{"m-s"}, "x"));
    REQUIRE(messages.enqueue_message_send_failed(
        ConversationId{"conv-beta"}, MessageId{"m-s"}));
    REQUIRE(messages.flush(2s));
    drain_until_idle(owner);
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.messages.messages.size() == 1);
        REQUIRE(snapshot.value.messages.messages.front().state == DeliveryState::Failed);
        REQUIRE(owner.stats().updates_rejected == 0);
    }

    // 终态幂等：Failed 上的重复失败回报 = 幂等 no-op（接受，不拒绝）。
    REQUIRE(messages.enqueue_message_send_failed(
        ConversationId{"conv-beta"}, MessageId{"m-s"}));
    REQUIRE(messages.flush(2s));
    drain_until_idle(owner);
    REQUIRE(owner.stats().updates_rejected == 0);

    // 迟到的失败回报不得复活 Delivered：Delivered -> Failed 非法，admission
    // 成功但状态机应用层拒绝（updates_rejected 增量，状态保持 Delivered）。
    REQUIRE(messages.send_text(DeviceId{"beta"}, MessageId{"m-d"}, "x"));
    REQUIRE(messages.enqueue_message_delivered(ConversationId{"conv-beta"}, MessageId{"m-d"}));
    REQUIRE(messages.flush(2s));
    drain_until_idle(owner);
    const auto rejected_before = owner.stats().updates_rejected;
    REQUIRE(messages.enqueue_message_send_failed(
        ConversationId{"conv-beta"}, MessageId{"m-d"}));
    REQUIRE(messages.flush(2s));
    drain_until_idle(owner);
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        for (const auto& message : snapshot.value.messages.messages) {
            if (message.id == MessageId{"m-d"}) {
                REQUIRE(message.state == DeliveryState::Delivered);
            }
        }
        REQUIRE(owner.stats().updates_rejected == rejected_before + 1);
    }

    // 空 message id：入队 admission 成功，handler 有界校验拒绝（可观测）。
    REQUIRE(messages.enqueue_message_send_failed(ConversationId{"conv-beta"}, MessageId{""}));
    REQUIRE(messages.flush(2s));
    REQUIRE(messages.stats().handler_rejections == 1);

    const auto report = stack.host.executor_owner.shutdown();
    REQUIRE(report.fully_stopped());
}

// ---- M4-03：图片消息收发面（设计 §6.1；DEC-010）----

// 图片出站路由 + DeliveryState 正向链（Queued->Sent->Delivered）+ 终态幂等
//（RULE-08）+ 任务异常沿图片泵路径可见（DOD-02：正常完成 + 任务异常）。
TEST_CASE("Image send routes through MessageManager with terminal idempotency",
    "[unit][managers][image][dod02]") {
    SECTION("happy path, delivery chain, late-failure idempotency") {
        AppStack stack;
        auto& owner = stack.state_owner;
        auto& fake = stack.adapter;
        const auto settle = [&] {
            quiesce(owner, *stack.devices, *stack.conversations, *stack.messages,
                *stack.transfers);
        };

        REQUIRE(stack.conversations->ensure_conversation(
            DeviceId{"local-1"}, DeviceId{"beta"}));
        settle();

        // 接收侧的 sender 会话也先行（FK 前置校验：MM 按 sender 派生归属）。
        REQUIRE(stack.conversations->ensure_conversation(
            DeviceId{"local-1"}, DeviceId{"alpha"}));
        settle();

        // 出站：Fake 记录 SentImage（消息面仅 metadata + TransferId，RULE-05），
        // 本地行 Sent（admission 语义同文本）。
        const FileMetadata media{"photo.png", 2048, "image/png"};
        REQUIRE(stack.messages->send_image(
            DeviceId{"beta"}, MessageId{"m-img"}, media, TransferId{"t-img"}));
        settle();
        {
            executor::comm::Snapshot<AppState> snapshot;
            REQUIRE(owner.try_load_snapshot(snapshot));
            REQUIRE(snapshot.value.messages.messages.size() == 1);
            const auto& row = snapshot.value.messages.messages.front();
            REQUIRE(row.type == MessageType::Image);
            REQUIRE(row.state == DeliveryState::Sent);
            const auto* image =
                std::get_if<aki::conversation::ImagePayload>(&row.payload);
            REQUIRE(image != nullptr);
            REQUIRE(image->media == media);
            REQUIRE(image->transfer_id == TransferId{"t-img"});
        }
        REQUIRE(fake.sent_images().size() == 1);
        REQUIRE(fake.sent_images().front().transfer_id == TransferId{"t-img"});
        REQUIRE(fake.sent_texts().empty());

        // 送达回报：Sent -> Delivered（正向链对 Image 成立）。
        REQUIRE(fake.inject_message_delivered(
            ConversationId{"conv-beta"}, MessageId{"m-img"}));
        settle();

        // 迟到失败回报：Delivered -> Failed 非法 → 应用层拒绝（RULE-08）。
        const auto rejected_before = owner.stats().updates_rejected;
        REQUIRE(stack.router->on_message_send_failed(
            ConversationId{"conv-beta"}, MessageId{"m-img"}));
        settle();
        {
            executor::comm::Snapshot<AppState> snapshot;
            REQUIRE(owner.try_load_snapshot(snapshot));
            REQUIRE(snapshot.value.messages.messages.front().state
                == DeliveryState::Delivered);
            REQUIRE(owner.stats().updates_rejected == rejected_before + 1);
        }

        // 接收侧：入站 Image typed 消息一律记 Delivered（§6.1② 接收侧语义）。
        Message inbound = make_message("m-img-in");
        inbound.type = MessageType::Image;
        inbound.payload = aki::conversation::ImagePayload{media, TransferId{"t-img"}};
        REQUIRE(fake.inject_message_received(std::move(inbound)));
        settle();
        {
            executor::comm::Snapshot<AppState> snapshot;
            REQUIRE(owner.try_load_snapshot(snapshot));
            REQUIRE(snapshot.value.messages.messages.size() == 2);
            const auto& row = snapshot.value.messages.messages.back();
            REQUIRE(row.id == MessageId{"m-img-in"});
            REQUIRE(row.state == DeliveryState::Delivered);
        }

        const auto report = stack.host.executor_owner.shutdown();
        REQUIRE(report.fully_stopped());
    }

    SECTION("adapter admission failure records Failed; throw self-heals") {
        // Adapter admission 拒绝（闸门外的普通发送失败面）：Queued -> Failed。
        {
            MessageOnlyStack stack{ExecutorOwner::Options{}, ManagerPumpOptions{},
                /*fail_send=*/true};
            REQUIRE(stack.messages->send_image(DeviceId{"beta"}, MessageId{"m-img-f"},
                FileMetadata{"photo.png", 1, "image/png"}, TransferId{"t-img"}));
            REQUIRE(stack.messages->flush(2s));
            drain_until_idle(stack.state_owner);
            executor::comm::Snapshot<AppState> snapshot;
            REQUIRE(stack.state_owner.try_load_snapshot(snapshot));
            REQUIRE(snapshot.value.messages.messages.front().state
                == DeliveryState::Failed);
            REQUIRE(stack.adapter.sent_images.empty());

            const auto report = stack.host.executor_owner.shutdown();
            REQUIRE(report.fully_stopped());
        }
        // 图片路径任务异常：Adapter 抛出经排空 future 可见（AGENTS 规则 3/9），
        // 泵自愈后下一条图片消息正常处理（DOD-02 六项之任务异常沿图片路径）。
        {
            MessageOnlyStack stack{ExecutorOwner::Options{}, ManagerPumpOptions{},
                /*fail_send=*/false, /*throw_send=*/true};
            auto& executor = stack.host.executor_owner.executor();
            const auto exceptions_before =
                executor.get_failure_status().task_exception_count;
            REQUIRE(stack.messages->send_image(DeviceId{"beta"}, MessageId{"m-boom"},
                FileMetadata{"photo.png", 1, "image/png"}, TransferId{"t-1"}));
            REQUIRE(stack.messages->flush(2s));
            stack.adapter.throw_on_send.store(false);
            REQUIRE(stack.messages->send_image(DeviceId{"beta"}, MessageId{"m-ok"},
                FileMetadata{"photo.png", 1, "image/png"}, TransferId{"t-2"}));
            REQUIRE(stack.messages->flush(2s));
            REQUIRE(wait_until([&] {
                return executor.get_failure_status().task_exception_count
                    >= exceptions_before + 1;
            }, 2s));
            REQUIRE(stack.adapter.sent_images.size() == 1);
            REQUIRE(stack.adapter.sent_images.front().file.name == "photo.png");

            const auto report = stack.host.executor_owner.shutdown();
            REQUIRE(report.fully_stopped());
        }
    }
}

// 发送侧准入闸门（§6.1②：先传输准入、后发消息；两向失败路径）。
TEST_CASE("Image send flow gates on transfer admission before the message",
    "[unit][managers][image][flow]") {
    SECTION("transfer admission failure: no send, message row Failed") {
        // 占住 TM 泵 + 收件箱：transfer_gate 关闭 → 第 1 条占住泵、第 2 条占住
        // 收件箱（容量 1），第 3 条 enqueue 拒绝——闸门第 1 步失败的确定性触发。
        TransferManagerOptions transfer_options;
        transfer_options.sender = DeviceId{"local-1"};
        transfer_options.pump.inbox_capacity = 1;
        BasicStack<StubAdapter> stack{ExecutorOwner::Options{}, ManagerPumpOptions{},
            {}, MessageManagerOptions{}, transfer_options};
        auto& owner = stack.state_owner;
        auto gate = std::make_shared<GateState>();
        stack.adapter.transfer_gate = gate;

        // DEC-009 ②：出站消息远端会话先行（FK 前置校验）。
        REQUIRE(stack.conversations->ensure_conversation(
            DeviceId{"local-1"}, DeviceId{"beta"}));
        REQUIRE(stack.conversations->flush(2s));

        REQUIRE(stack.transfers->start_transfer(
            DeviceId{"beta"}, TransferId{"t-occ-1"}, make_file()));
        // 等泵真实占住（item 已离箱、阻塞在 Adapter 门内）再投第二条——
        // 否则第二条会与第一条竞态争用唯一收件箱槽位。
        REQUIRE(wait_until(
            [&] { return stack.adapter.transfer_entered.load() == 1; }, 2s));
        REQUIRE(stack.transfers->start_transfer(
            DeviceId{"beta"}, TransferId{"t-occ-2"}, make_file()));

        const auto result = aki::app::send_image_message_with_transfer(
            *stack.transfers, *stack.messages, DeviceId{"beta"},
            MessageId{"m-gate-1"}, make_file(), TransferId{"t-flow-1"});
        REQUIRE(result == aki::app::ImageSendFlowResult::TransferAdmissionFailed);

        gate->open_gate();
        // 容量 1 的收件箱在泵取走第二条前不接受 flush 哨兵：等第二条真实
        // 进入 Adapter（已离箱）再排空，消除竞态。
        REQUIRE(wait_until(
            [&] { return stack.adapter.transfer_entered.load() == 2; }, 2s));
        REQUIRE(stack.transfers->flush(2s));
        REQUIRE(stack.messages->flush(2s));
        quiesce(owner, *stack.devices, *stack.conversations, *stack.messages,
            *stack.transfers);
        {
            // 消息行 Failed（Queued -> Failed 合法边）；Adapter 零 send 调用
            //（闸门断言对象，§6.1②）。
            executor::comm::Snapshot<AppState> snapshot;
            REQUIRE(owner.try_load_snapshot(snapshot));
            REQUIRE(snapshot.value.messages.messages.size() == 1);
            REQUIRE(snapshot.value.messages.messages.front().id
                == MessageId{"m-gate-1"});
            REQUIRE(snapshot.value.messages.messages.front().state
                == DeliveryState::Failed);
        }
        REQUIRE(stack.adapter.sent_images.empty());
        REQUIRE(stack.adapter.sent.empty());

        // 关闭清理：两个占用会话回收（EXEC-01 关闭纪律）；容量 1 收件箱下
        // 逐条取消并以原子计数同步（取消离箱后 flush 哨兵才可入列）。
        REQUIRE(stack.transfers->cancel_transfer(TransferId{"t-occ-1"}));
        REQUIRE(wait_until(
            [&] { return stack.adapter.cancel_entered.load() == 1; }, 2s));
        REQUIRE(stack.transfers->flush(2s));
        REQUIRE(stack.transfers->cancel_transfer(TransferId{"t-occ-2"}));
        REQUIRE(wait_until(
            [&] { return stack.adapter.cancel_entered.load() == 2; }, 2s));
        REQUIRE(stack.transfers->flush(2s));

        const auto report = stack.host.executor_owner.shutdown();
        REQUIRE(report.fully_stopped());
    }

    SECTION("message admission failure: transfer cancelled") {
        // 占住 MM 泵 + 收件箱（send_gate 关闭，容量 1）→ 闸门第 2 步失败。
        ManagerPumpOptions message_pump;
        message_pump.name = "aki.mm";
        message_pump.inbox_capacity = 1;
        MessageManagerOptions message_options;
        message_options.pump = message_pump;
        message_options.local_device = DeviceId{"local-1"};
        TransferManagerOptions transfer_options;
        transfer_options.sender = DeviceId{"local-1"};
        BasicStack<StubAdapter> stack{ExecutorOwner::Options{}, ManagerPumpOptions{},
            {}, message_options, transfer_options};
        auto& owner = stack.state_owner;
        auto gate = std::make_shared<GateState>();
        stack.adapter.send_gate = gate;

        // DEC-009 ②：出站消息远端会话先行（FK 前置校验）。
        REQUIRE(stack.conversations->ensure_conversation(
            DeviceId{"local-1"}, DeviceId{"beta"}));
        REQUIRE(stack.conversations->flush(2s));

        REQUIRE(stack.messages->send_text(DeviceId{"beta"}, MessageId{"m-occ-1"}, "a"));
        REQUIRE(wait_until([&] { return stack.adapter.send_entered.load() == 1; }, 2s));
        REQUIRE(stack.messages->send_text(DeviceId{"beta"}, MessageId{"m-occ-2"}, "b"));

        const auto result = aki::app::send_image_message_with_transfer(
            *stack.transfers, *stack.messages, DeviceId{"beta"},
            MessageId{"m-gate-2"}, make_file(), TransferId{"t-flow-2"});
        REQUIRE(result == aki::app::ImageSendFlowResult::MessageAdmissionFailed);

        gate->open_gate();
        REQUIRE(stack.messages->flush(2s));
        REQUIRE(stack.transfers->flush(2s));
        quiesce(owner, *stack.devices, *stack.conversations, *stack.messages,
            *stack.transfers);
        {
            // 图片消息从未进入系统（无 m-gate-2 行）；cancel_transfer 到达
            // Adapter（闸门第 2 步的因果链断言——传输行终态经标准事件路径
            // 推进，§7.1②）。
            executor::comm::Snapshot<AppState> snapshot;
            REQUIRE(owner.try_load_snapshot(snapshot));
            for (const auto& message : snapshot.value.messages.messages) {
                REQUIRE(message.id != MessageId{"m-gate-2"});
            }
        }
        bool cancel_seen = false;
        for (const auto& id : stack.adapter.cancelled_transfers) {
            if (id == TransferId{"t-flow-2"}) {
                cancel_seen = true;
            }
        }
        REQUIRE(cancel_seen);
        REQUIRE(stack.adapter.sent_images.empty());

        // 关闭清理：t-flow-2 会话在泵内已派生（StartTransferWork 先于 Cancel），
        // cancel 已入列；会话表由泵内取消回收，shutdown 钩子由栈析构兜底。
        REQUIRE(stack.transfers->request_cancel_all());
        REQUIRE(stack.transfers->flush(2s));

        const auto report = stack.host.executor_owner.shutdown();
        REQUIRE(report.fully_stopped());
    }

    SECTION("duplicate transfer id: gate blind at enqueue, handler rejects") {
        // 重复 TransferId 的业务拒绝发生在 TM 排空 handler（用例 5b 语义：
        // enqueue 返回值只代表收件箱受理），闸门不可见——编排返回 Submitted、
        // 消息照常发送；后置结果按 §6.1② 如实断言（不新增传输行、不替换旧
        // 会话、handler_rejections 可见）。
        TransferManagerOptions transfer_options;
        transfer_options.sender = DeviceId{"local-1"};
        BasicStack<StubAdapter> stack{ExecutorOwner::Options{}, ManagerPumpOptions{},
            {}, MessageManagerOptions{}, transfer_options};
        auto& owner = stack.state_owner;

        // DEC-009 ②：出站消息远端会话先行（FK 前置校验）。
        REQUIRE(stack.conversations->ensure_conversation(
            DeviceId{"local-1"}, DeviceId{"beta"}));
        REQUIRE(stack.conversations->flush(2s));

        REQUIRE(stack.transfers->start_transfer(
            DeviceId{"beta"}, TransferId{"t-dup"}, make_file()));
        REQUIRE(stack.transfers->flush(2s));
        REQUIRE(wait_until(
            [&] { return stack.transfers->active_session_count() == 1; }, 2s));

        const auto result = aki::app::send_image_message_with_transfer(
            *stack.transfers, *stack.messages, DeviceId{"beta"},
            MessageId{"m-dup"}, make_file(), TransferId{"t-dup"});
        // 闸门只看 enqueue admission：双侧收件箱受理 → Submitted（消息已发）。
        REQUIRE(result == aki::app::ImageSendFlowResult::Submitted);

        REQUIRE(stack.transfers->flush(2s));
        REQUIRE(stack.messages->flush(2s));
        quiesce(owner, *stack.devices, *stack.conversations, *stack.messages,
            *stack.transfers);
        {
            executor::comm::Snapshot<AppState> snapshot;
            REQUIRE(owner.try_load_snapshot(snapshot));
            // 消息照常发送并记录 Sent（引用既有 TransferId）。
            REQUIRE(snapshot.value.messages.messages.size() == 1);
            REQUIRE(snapshot.value.messages.messages.front().id
                == MessageId{"m-dup"});
            REQUIRE(snapshot.value.messages.messages.front().state
                == DeliveryState::Sent);
            // 重复发起不新增传输行、不替换旧会话：仍是一条 Queued。
            REQUIRE(snapshot.value.transfers.transfers.size() == 1);
            REQUIRE(snapshot.value.transfers.transfers.front().state
                == TransferState::Queued);
        }
        REQUIRE(stack.transfers->stats().handler_rejections == 1);
        REQUIRE(stack.transfers->active_session_count() == 1);
        REQUIRE(stack.adapter.sent_images.size() == 1);

        // 关闭清理：唯一会话回收。
        REQUIRE(stack.transfers->cancel_transfer(TransferId{"t-dup"}));
        REQUIRE(stack.transfers->flush(2s));

        const auto report = stack.host.executor_owner.shutdown();
        REQUIRE(report.fully_stopped());
    }

    SECTION("transfer admission failure with full MM inbox: failed row lost") {
        // 两级降级①（AGENTS 规则 10）：TM enqueue 拒绝触发补偿 Failed 行写入，
        // 但 MM 收件箱也满 → 补偿写入被拒：无消息行。降级经返回值
        // TransferAdmissionFailedRowLost 可见，不 (void) 吞掉。
        ManagerPumpOptions message_pump;
        message_pump.name = "aki.mm";
        message_pump.inbox_capacity = 1;
        MessageManagerOptions message_options;
        message_options.pump = message_pump;
        message_options.local_device = DeviceId{"local-1"};
        TransferManagerOptions transfer_options;
        transfer_options.sender = DeviceId{"local-1"};
        transfer_options.pump.inbox_capacity = 1;
        BasicStack<StubAdapter> stack{ExecutorOwner::Options{}, ManagerPumpOptions{},
            {}, message_options, transfer_options};
        auto& owner = stack.state_owner;
        auto transfer_gate = std::make_shared<GateState>();
        auto send_gate = std::make_shared<GateState>();
        stack.adapter.transfer_gate = transfer_gate;
        stack.adapter.send_gate = send_gate;

        // DEC-009 ②：出站消息远端会话先行（FK 前置校验）。
        REQUIRE(stack.conversations->ensure_conversation(
            DeviceId{"local-1"}, DeviceId{"beta"}));
        REQUIRE(stack.conversations->flush(2s));

        // 双侧占位：TM 泵 + 收件箱、MM 泵 + 收件箱（容量各 1）。
        REQUIRE(stack.transfers->start_transfer(
            DeviceId{"beta"}, TransferId{"t-occ-1"}, make_file()));
        REQUIRE(wait_until(
            [&] { return stack.adapter.transfer_entered.load() == 1; }, 2s));
        REQUIRE(stack.transfers->start_transfer(
            DeviceId{"beta"}, TransferId{"t-occ-2"}, make_file()));
        REQUIRE(stack.messages->send_text(DeviceId{"beta"}, MessageId{"m-occ-1"}, "a"));
        REQUIRE(wait_until(
            [&] { return stack.adapter.send_entered.load() == 1; }, 2s));
        REQUIRE(stack.messages->send_text(DeviceId{"beta"}, MessageId{"m-occ-2"}, "b"));

        const auto result = aki::app::send_image_message_with_transfer(
            *stack.transfers, *stack.messages, DeviceId{"beta"},
            MessageId{"m-gate-3"}, make_file(), TransferId{"t-flow-3"});
        REQUIRE(result
            == aki::app::ImageSendFlowResult::TransferAdmissionFailedRowLost);

        transfer_gate->open_gate();
        send_gate->open_gate();
        // 容量 1 的收件箱在泵取走第二条前不接受 flush 哨兵：等双侧第二条真实
        // 进入 Adapter（已离箱）再排空，消除竞态。
        REQUIRE(wait_until(
            [&] { return stack.adapter.transfer_entered.load() == 2; }, 2s));
        REQUIRE(wait_until(
            [&] { return stack.adapter.send_entered.load() == 2; }, 2s));
        REQUIRE(stack.transfers->flush(2s));
        REQUIRE(stack.messages->flush(2s));
        quiesce(owner, *stack.devices, *stack.conversations, *stack.messages,
            *stack.transfers);
        {
            // 降级①后置结果：无 m-gate-3 消息行（只有两条占位文本）；Adapter
            // 零图片 send 调用（闸门断言对象不变，§6.1②）。
            executor::comm::Snapshot<AppState> snapshot;
            REQUIRE(owner.try_load_snapshot(snapshot));
            REQUIRE(snapshot.value.messages.messages.size() == 2);
            for (const auto& message : snapshot.value.messages.messages) {
                REQUIRE(message.id != MessageId{"m-gate-3"});
            }
        }
        REQUIRE(stack.adapter.sent_images.empty());

        // 关闭清理：两个占用会话回收（容量 1 收件箱逐条取消同步，同第 1 节）。
        REQUIRE(stack.transfers->cancel_transfer(TransferId{"t-occ-1"}));
        REQUIRE(wait_until(
            [&] { return stack.adapter.cancel_entered.load() == 1; }, 2s));
        REQUIRE(stack.transfers->flush(2s));
        REQUIRE(stack.transfers->cancel_transfer(TransferId{"t-occ-2"}));
        REQUIRE(wait_until(
            [&] { return stack.adapter.cancel_entered.load() == 2; }, 2s));
        REQUIRE(stack.transfers->flush(2s));

        const auto report = stack.host.executor_owner.shutdown();
        REQUIRE(report.fully_stopped());
    }

    SECTION("message admission failure with full TM inbox: cancel lost") {
        // 两级降级②（AGENTS 规则 10）：MM enqueue 拒绝触发补偿 cancel_transfer，
        // 但 TM 收件箱被本编排刚受理的 StartTransferWork 占满（容量 1）→ 补偿
        // 取消被拒：传输持续在飞、传输行停留 Queued。降级经返回值
        // MessageAdmissionFailedCancelLost 可见（同步拒绝，无竞态）。
        ManagerPumpOptions message_pump;
        message_pump.name = "aki.mm";
        message_pump.inbox_capacity = 1;
        MessageManagerOptions message_options;
        message_options.pump = message_pump;
        message_options.local_device = DeviceId{"local-1"};
        TransferManagerOptions transfer_options;
        transfer_options.sender = DeviceId{"local-1"};
        transfer_options.pump.inbox_capacity = 1;
        BasicStack<StubAdapter> stack{ExecutorOwner::Options{}, ManagerPumpOptions{},
            {}, message_options, transfer_options};
        auto& owner = stack.state_owner;
        auto transfer_gate = std::make_shared<GateState>();
        auto send_gate = std::make_shared<GateState>();
        stack.adapter.transfer_gate = transfer_gate;
        stack.adapter.send_gate = send_gate;

        // DEC-009 ②：出站消息远端会话先行（FK 前置校验）。
        REQUIRE(stack.conversations->ensure_conversation(
            DeviceId{"local-1"}, DeviceId{"beta"}));
        REQUIRE(stack.conversations->flush(2s));

        // 占位：TM 泵阻塞在 Adapter 门内（t-occ-1 已离箱、收件箱空）；MM 泵 +
        // 收件箱占满。闸门第 1 步受理的 t-flow-4 占据 TM 唯一收件箱槽位 →
        // 补偿 cancel_transfer 对满箱再拒（同步 enqueue 拒绝，确定性可达）。
        REQUIRE(stack.transfers->start_transfer(
            DeviceId{"beta"}, TransferId{"t-occ-1"}, make_file()));
        REQUIRE(wait_until(
            [&] { return stack.adapter.transfer_entered.load() == 1; }, 2s));
        REQUIRE(stack.messages->send_text(DeviceId{"beta"}, MessageId{"m-occ-1"}, "a"));
        REQUIRE(wait_until(
            [&] { return stack.adapter.send_entered.load() == 1; }, 2s));
        REQUIRE(stack.messages->send_text(DeviceId{"beta"}, MessageId{"m-occ-2"}, "b"));

        const auto result = aki::app::send_image_message_with_transfer(
            *stack.transfers, *stack.messages, DeviceId{"beta"},
            MessageId{"m-gate-4"}, make_file(), TransferId{"t-flow-4"});
        REQUIRE(result
            == aki::app::ImageSendFlowResult::MessageAdmissionFailedCancelLost);

        transfer_gate->open_gate();
        send_gate->open_gate();
        REQUIRE(wait_until(
            [&] { return stack.adapter.transfer_entered.load() == 2; }, 2s));
        REQUIRE(wait_until(
            [&] { return stack.adapter.send_entered.load() == 2; }, 2s));
        REQUIRE(stack.transfers->flush(2s));
        REQUIRE(stack.messages->flush(2s));
        quiesce(owner, *stack.devices, *stack.conversations, *stack.messages,
            *stack.transfers);
        {
            // 降级②后置结果：补偿取消丢失——t-flow-4 传输行照常落库并停留
            // Queued（非 Cancelled）；m-gate-4 消息行未产生。
            executor::comm::Snapshot<AppState> snapshot;
            REQUIRE(owner.try_load_snapshot(snapshot));
            const auto& transfer_rows = snapshot.value.transfers.transfers;
            REQUIRE(transfer_rows.size() == 2);
            const Transfer* flow_row = nullptr;
            for (const auto& row : transfer_rows) {
                if (row.id == TransferId{"t-flow-4"}) {
                    flow_row = &row;
                }
            }
            REQUIRE(flow_row != nullptr);
            REQUIRE(flow_row->state == TransferState::Queued);
            for (const auto& message : snapshot.value.messages.messages) {
                REQUIRE(message.id != MessageId{"m-gate-4"});
            }
        }
        bool cancel_seen = false;
        for (const auto& id : stack.adapter.cancelled_transfers) {
            if (id == TransferId{"t-flow-4"}) {
                cancel_seen = true;
            }
        }
        REQUIRE(!cancel_seen);
        // 会话在飞：t-occ-1 与 t-flow-4 双会话派生（补偿取消丢失的直接后果）。
        REQUIRE(wait_until(
            [&] { return stack.transfers->active_session_count() == 2; }, 2s));

        // 关闭清理：容量 1 收件箱下逐条取消并以原子计数同步（同第 1 节——
        // request_cancel_all + flush 会因哨兵无法入满箱而竞态）。
        REQUIRE(stack.transfers->cancel_transfer(TransferId{"t-occ-1"}));
        REQUIRE(wait_until(
            [&] { return stack.adapter.cancel_entered.load() == 1; }, 2s));
        REQUIRE(stack.transfers->flush(2s));
        REQUIRE(stack.transfers->cancel_transfer(TransferId{"t-flow-4"}));
        REQUIRE(wait_until(
            [&] { return stack.adapter.cancel_entered.load() == 2; }, 2s));
        REQUIRE(stack.transfers->flush(2s));

        const auto report = stack.host.executor_owner.shutdown();
        REQUIRE(report.fully_stopped());
    }
}

// 运行期零传导（§6.1②）：peer ack 事件与传输终态事件互不跨通道回写。
TEST_CASE("Image message delivery and transfer terminal states stay orthogonal "
    "(zero runtime conduction)",
    "[unit][managers][image]") {
    AppStack stack;
    auto& owner = stack.state_owner;
    auto& fake = stack.adapter;
    const auto settle = [&] {
        quiesce(owner, *stack.devices, *stack.conversations, *stack.messages,
            *stack.transfers);
    };

    REQUIRE(stack.conversations->ensure_conversation(
        DeviceId{"local-1"}, DeviceId{"beta"}));
    // 接收侧消息的 sender 会话先行（FK 前置校验：MM 按 sender 派生归属）。
    REQUIRE(stack.conversations->ensure_conversation(
        DeviceId{"local-1"}, DeviceId{"alpha"}));
    settle();

    // 闸门正常通过：双侧准入成功。
    const FileMetadata media{"photo.png", 2048, "image/png"};
    REQUIRE(aki::app::send_image_message_with_transfer(*stack.transfers,
        *stack.messages, DeviceId{"beta"}, MessageId{"m-img"}, media,
        TransferId{"t-img"})
        == aki::app::ImageSendFlowResult::Submitted);
    settle();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.messages.messages.size() == 1);
        REQUIRE(snapshot.value.transfers.transfers.size() == 1);
        REQUIRE(snapshot.value.transfers.transfers.front().id
            == TransferId{"t-img"});
        REQUIRE(snapshot.value.transfers.transfers.front().state
            == TransferState::Queued);
    }
    REQUIRE(fake.sent_images().size() == 1);
    REQUIRE(fake.transfer_session_known("t-img"));

    // 消息 Delivered；随后传输 Failed：消息保持 Delivered（运行期零传导——
    // 传输晚于 peer ack 失败不回写消息面，Delivered -> Failed 非法边由 owner
    // 拒绝，此处甚至无跨通道更新可拒绝）。传输行沿合法边推进：
    // Queued -> Negotiating（inject started）-> Failed（终态宣告）。
    REQUIRE(fake.inject_message_delivered(
        ConversationId{"conv-beta"}, MessageId{"m-img"}));
    settle();
    REQUIRE(fake.inject_transfer_started(
        make_transfer("t-img", TransferState::Negotiating)));
    settle();
    REQUIRE(fake.inject_transfer_completed(TransferId{"t-img"}, TransferState::Failed));
    settle();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.messages.messages.front().state
            == DeliveryState::Delivered);
        REQUIRE(snapshot.value.transfers.transfers.front().state
            == TransferState::Failed);
    }

    // 反向到达顺序：传输先 Completed、消息 ack 后到——传输保持 Completed，
    // 消息照常 Delivered，两行互不影响（接收侧消息一律 Delivered；入站传输
    // 状态仅经 transfers Store join 可见）。
    Message inbound = make_message("m-img-rx");
    inbound.type = MessageType::Image;
    inbound.payload = aki::conversation::ImagePayload{media, TransferId{"t-rx"}};
    REQUIRE(fake.inject_transfer_started(
        make_transfer("t-rx", TransferState::Transferring)));
    settle();
    REQUIRE(fake.inject_message_received(std::move(inbound)));
    settle();
    REQUIRE(fake.inject_transfer_completed(
        TransferId{"t-rx"}, TransferState::Completed));
    settle();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        const auto& messages = snapshot.value.messages.messages;
        REQUIRE(messages.size() == 2);
        REQUIRE(messages.back().id == MessageId{"m-img-rx"});
        REQUIRE(messages.back().state == DeliveryState::Delivered);
        const auto* image =
            std::get_if<aki::conversation::ImagePayload>(&messages.back().payload);
        REQUIRE(image != nullptr);
        REQUIRE(image->transfer_id == TransferId{"t-rx"});
        REQUIRE(snapshot.value.transfers.transfers.size() == 2);
        REQUIRE(snapshot.value.transfers.transfers.back().id
            == TransferId{"t-rx"});
        REQUIRE(snapshot.value.transfers.transfers.back().state
            == TransferState::Completed);
    }

    // 关闭清理：闸门创建的传输会话先取消回收（EXEC-01 关闭纪律——会话在飞
    // 时 shutdown 的有界等待会超时）。
    REQUIRE(stack.transfers->request_cancel_all());
    REQUIRE(stack.transfers->flush(2s));

    const auto report = stack.host.executor_owner.shutdown();
    REQUIRE(report.fully_stopped());
}

int main(int argc, char* argv[]) {
    // 每个用例持有自己的 ExecutorOwner 实例（非单例，AGENTS 规则 7/8；设计
    // 第 8.2 节落点说明），进程级不设共享 owner。
    const int result = Catch::Session().run(argc, argv);
    return result;
}
