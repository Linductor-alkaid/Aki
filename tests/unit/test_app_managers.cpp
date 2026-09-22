// M1-05：Device / Conversation / Message / Transfer Manager 骨架测试（DEC-008）。
//
// 覆盖（验收标准 ①②，设计第 8.3 节契约）：
//   - 9 类 Sink 事件经 RouterSink 路由 → Manager 排空 → AppState 快照与必达
//     事件主路径 FIFO；connected/disconnected 扇出（DM presence + CM 会话推导，
//     主路径事件只投递一次）；ensure_conversation 显式建会话；
//   - 并发入队不丢（单飞泵丢失唤醒防护）；
//   - DOD-02 六项沿 Manager 任务路径：正常完成（用例 1/2）、任务异常（Adapter
//     抛出经 future + task_exception_count 可见且泵自愈）、提交拒绝
//     （max_in_flight_tasks 准入 + 收件箱满，均可见）、执行中取消
//     （submit_cancellable + request_task_cancel + StopToken 轮询退出）、超时
//     （独立 owner 排队软超时击杀排空任务且存量不丢）、shutdown（第 8.3 节
//     关闭钩子顺序 → fully_stopped）；
//   - 迟到事件不复活终态（RULE-08，退出-3）：迟到的进度/终态宣告、迟到的
//     送达回报经 Manager 路由后被状态机应用层拒绝（updates_rejected）。
//
// 每个用例持有独立的 ExecutorOwner（AGENTS 规则 7/8；设计第 8.2 节落点说明：
// 被测对象即组合根的一部分，EXEC-01 五步由用例显式驱动）。并发入队者经
// executor 任务承载（AGENTS 规则 2）；Catch2 断言只在主线程。
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
    std::atomic<bool> throw_on_send{false};
    std::atomic<bool> fail_send{false};
    std::atomic<int> send_entered{0};

    struct SentText {
        DeviceId to;
        MessageId message_id;
        std::string text;
    };
    std::vector<SentText> sent;

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

    bool start_file_transfer(const DeviceId&, const TransferId&,
        const FileMetadata&) override {
        return true;
    }
    bool pause_transfer(const TransferId&) override { return true; }
    bool resume_transfer(const TransferId&) override { return true; }
    bool cancel_transfer(const TransferId&) override { return true; }
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
        : host(std::move(host_options)) {
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
    AppStateOwner state_owner{};
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

// ---- 用例 1：9 类事件路由、Store 归属、扇出与 FIFO（DOD-02 正常完成）----

TEST_CASE("RouterSink routes the nine events to per-domain stores in FIFO order",
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
    REQUIRE(fake.inject_message_received(make_message("m-in")));
    settle();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.messages.messages.size() == 1);
        REQUIRE(snapshot.value.messages.messages.front().state == DeliveryState::Delivered);
    }

    // ⑥ send_text（MM 出站）：Adapter admission 成功 → 本地 Sent。
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

// ---- 用例 5：DOD-02 执行中取消——submit_cancellable + request_task_cancel ----

TEST_CASE("DOD-02 in-flight cancellation: transfer session observes the stop token",
    "[unit][managers][dod02]") {
    TransferManagerOptions transfer_options;
    transfer_options.sender = DeviceId{"local-1"};
    transfer_options.session_poll_interval = 2ms;
    transfer_options.session_reap_wait = 1s;
    AppStack stack{ExecutorOwner::Options{}, ManagerPumpOptions{}, {},
        MessageManagerOptions{}, transfer_options};
    auto& owner = stack.state_owner;
    auto& fake = stack.adapter;
    auto& transfers = *stack.transfers;
    auto& executor = stack.host.executor_owner.executor();

    // 应用发起传输：会话任务派生（TaskHandle 按 TransferId 归 Manager 持有）。
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

    // 协作取消：Executor 侧 request_task_cancel（排队/运行期对 Executor 可见）。
    REQUIRE(transfers.cancel_transfer(TransferId{"t-1"}));
    REQUIRE(transfers.flush(2s));  // 取消 + 会话回收（future 已消费）。
    REQUIRE(transfers.active_session_count() == 0);
    REQUIRE(transfers.cancelled_session_count() == 1);

    const auto cancellation = executor.get_cancellation_status();
    REQUIRE(cancellation.request_count == 1);
    REQUIRE(cancellation.running_request_count == 1);

    // Adapter 侧取消命令同样可见（SPI 幂等停止）。
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

    // 幂等：会话已回收，再次取消不再产生新的取消请求。
    REQUIRE(transfers.cancel_transfer(TransferId{"t-1"}));
    REQUIRE(transfers.flush(2s));
    REQUIRE(executor.get_cancellation_status().request_count == 1);

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
    transfer_options.session_poll_interval = 2ms;
    transfer_options.session_reap_wait = 1s;
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
    transfer_options.session_poll_interval = 2ms;
    transfer_options.session_reap_wait = 1s;
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
    REQUIRE(stack.host.executor_owner.executor().get_cancellation_status().request_count
        == 1);
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

int main(int argc, char* argv[]) {
    // 每个用例持有自己的 ExecutorOwner 实例（非单例，AGENTS 规则 7/8；设计
    // 第 8.2 节落点说明），进程级不设共享 owner。
    const int result = Catch::Session().run(argc, argv);
    return result;
}
