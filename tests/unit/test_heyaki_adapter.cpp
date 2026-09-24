// M1-03：Heyaki Adapter SPI 与 FakeHeyakiAdapter 单测。
//
// 覆盖（验收标准 ①②）：
//   - SPI 为纯虚抽象接口且公开头文件仅依赖领域类型（RULE-01/RULE-10，
//     本编译单元只包含 SPI 头 + 领域头 + Catch2，可评审复查）；
//   - 注入发现 → AppStateOwner 快照出现新设备（经应用层桥接 Sink → 事件入口 +
//     状态更新，设计第 8.1 节）；
//   - 注入连接路径变化 → LatestMailbox 更新；
//   - 注入消息 → 必达事件主路径 FIFO 可见；
//   - 发送失败回报（SPI 第 10 方法）经 BridgeSink 映射 SetDeliveryState(Failed)
//     （DEC-006 映射 4；迟到回报不复活终态，RULE-08）；
//   - 终态/取消后注入迟到事件被状态机应用层拒绝（RULE-08）；
//   - DOD-02 六项沿注入管线覆盖：正常完成、任务异常、提交拒绝、执行中取消、
//     超时、shutdown（owner 通道自身的关闭/排空语义详见 test_app_state）。
//
// 本测试进程唯一的 Executor owner 是文件末尾的 main()（AGENTS.md 规则 7）；
// 并发任务一律经 pinned executor 的 submit_auto 承载，Catch2 断言只在 main 线程。
#include "app/state/app_events.hpp"
#include "app/state/app_state.hpp"
#include "app/state/app_state_owner.hpp"
#include "app/state/app_state_updates.hpp"
#include "heyaki/adapter/fake_heyaki_adapter.hpp"
#include "heyaki/adapter/heyaki_adapter.hpp"

#include <executor/executor.hpp>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

executor::Executor* g_test_executor = nullptr;

}  // namespace

namespace aki::test {

// 本进程唯一 Executor owner 是 main()（见文件末尾）；用例经此获取执行上下文。
[[nodiscard]] executor::Executor& executor() {
    return *g_test_executor;
}

void use_executor(executor::Executor& executor) {
    g_test_executor = &executor;
}

}  // namespace aki::test

namespace {

using namespace std::chrono_literals;

// 失败计数相对 future 结算是异步记账（executor 包装器先满足 future 再记失败）：
// 断言计数前以有界轮询等待可见，与 test_executor_lifecycle 的处理一致。
bool wait_until(const std::function<bool()>& predicate, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

using aki::app::AppEvent;
using aki::app::AppEventPayload;
using aki::app::AppState;
using aki::app::AppStateOwner;
using aki::app::AppStateOwnerOptions;
using aki::app::ConnectionPathChangedEvent;
using aki::app::DeviceConnectedEvent;
using aki::app::DeviceDiscoveredEvent;
using aki::app::DeviceDisconnectedEvent;
using aki::app::MessageDeliveredEvent;
using aki::app::MessageReceivedEvent;
using aki::app::SetConnectionPath;
using aki::app::TransferCompletedEvent;
using aki::app::TransferProgressEvent;
using aki::app::TransferStartedEvent;
using aki::app::UpsertDevice;
using aki::app::UpsertMessage;
using aki::app::UpsertTransfer;
using aki::app::UpdateTransferProgress;

using aki::conversation::ConversationId;
using aki::conversation::DeliveryState;
using aki::conversation::Message;
using aki::conversation::MessageId;
using aki::conversation::MessageType;
using aki::device::ConnectionPath;
using aki::device::DeviceId;
using aki::device::DeviceIdentity;
using aki::device::DiscoveredDevice;
using aki::device::DiscoveryMethod;
using aki::device::PresenceState;
using aki::device::TrustState;
using aki::heyaki::FakeHeyakiAdapter;
using aki::heyaki::HeyakiAdapter;
using aki::heyaki::HeyakiAdapterSink;
static_assert(std::is_base_of_v<aki::heyaki::HeyakiAdapter, FakeHeyakiAdapter>,
    "Fake must implement the M1 HeyakiAdapter SPI (same contract as the real adapter)");
using aki::transfer::FileMetadata;
using aki::transfer::Transfer;
using aki::transfer::TransferId;
using aki::transfer::TransferState;

// 应用层桥接（设计第 8.1 节）：Sink 事件 → owner 事件入口 + 对应状态更新。
// 这是 M1-05 Manager 事件处理的雏形；M1 内仅供测试与冒烟宿主使用。
class BridgeSink final : public HeyakiAdapterSink {
public:
    explicit BridgeSink(AppStateOwner& owner) noexcept : owner_(owner) {}

    bool on_device_discovered(DiscoveredDevice device) override {
        const bool posted = post(DeviceDiscoveredEvent{device});
        const bool applied = owner_.submit_update(UpsertDevice{device.identity});
        return posted && applied;
    }

    bool on_device_connected(DeviceId device, ConnectionPath path) override {
        return post(DeviceConnectedEvent{std::move(device), path});
    }

    bool on_device_disconnected(DeviceId device) override {
        return post(DeviceDisconnectedEvent{std::move(device)});
    }

    bool on_message_received(Message message) override {
        // 测试桥接：会话按需 ensure（幂等 upsert），再提交带归属的消息
        //（DEC-009 ② FK 前置校验要求会话先在）。
        aki::conversation::Conversation conversation;
        conversation.id = ConversationId{"conv-test"};
        conversation.local_device = DeviceId{"local"};
        conversation.remote_device = message.sender;
        (void)owner_.submit_update(
            aki::app::UpsertConversation{conversation});
        const bool posted = post(MessageReceivedEvent{message});
        const bool applied = owner_.submit_update(UpsertMessage{
            std::move(message), ConversationId{"conv-test"}});
        return posted && applied;
    }

    bool on_message_delivered(ConversationId conversation, MessageId message) override {
        return post(MessageDeliveredEvent{std::move(conversation), std::move(message)});
    }

    bool on_message_send_failed(ConversationId conversation, MessageId message) override {
        // 测试桥接：失败回报映射 SetDeliveryState(Failed)（终态，RULE-08）。
        (void)conversation;
        const bool applied = owner_.submit_update(
            aki::app::SetDeliveryState{std::move(message),
                aki::conversation::DeliveryState::Failed});
        return applied;
    }

    bool on_transfer_started(Transfer transfer) override {
        const bool posted = post(TransferStartedEvent{transfer});
        const bool applied = owner_.submit_update(UpsertTransfer{std::move(transfer)});
        return posted && applied;
    }

    bool on_transfer_progress(TransferId transfer, std::uint64_t transferred,
        std::uint64_t total) override {
        const bool posted = post(TransferProgressEvent{transfer, transferred, total});
        const bool applied = owner_.submit_update(
            UpdateTransferProgress{std::move(transfer), transferred, total});
        return posted && applied;
    }

    bool on_transfer_completed(TransferId transfer, TransferState final_state) override {
        return post(TransferCompletedEvent{std::move(transfer), final_state});
    }

    bool on_connection_path_changed(DeviceId device, ConnectionPath from,
        ConnectionPath to) override {
        const bool posted =
            post(ConnectionPathChangedEvent{std::move(device), from, to});
        const bool applied = owner_.submit_update(SetConnectionPath{to});
        return posted && applied;
    }

private:
    bool post(AppEventPayload payload) {
        AppEvent event;
        event.payload = std::move(payload);
        return owner_.post_event(std::move(event));
    }

    AppStateOwner& owner_;
};

DiscoveredDevice make_discovered(std::string id, TrustState trust = TrustState::Unknown) {
    DiscoveredDevice device;
    device.identity.id = DeviceId{std::move(id)};
    device.identity.display_name = device.identity.id.value;
    device.identity.trust_state = trust;
    device.identity.presence = PresenceState::Online;
    device.method = DiscoveryMethod::LanDiscovery;
    return device;
}

Message make_message(std::string id, DeliveryState state = DeliveryState::Sent) {
    Message message;
    message.id = MessageId{std::move(id)};
    message.sender = DeviceId{"alpha"};
    message.receiver = DeviceId{"beta"};
    message.type = MessageType::Text;
    message.state = state;
    return message;
}

Transfer make_transfer(std::string id, TransferState state, std::uint64_t transferred = 0) {
    Transfer transfer;
    transfer.id = TransferId{std::move(id)};
    transfer.sender = DeviceId{"alpha"};
    transfer.receiver = DeviceId{"beta"};
    transfer.file = FileMetadata{"model.gguf", 1024, "application/octet-stream"};
    transfer.transferred = transferred;
    transfer.total = 1024;
    transfer.state = state;
    return transfer;
}

std::uint64_t outbox_watermark(AppStateOwner& owner) {
    return owner.stats().events_forwarded + owner.stats().events_dropped;
}

void drain_until_idle(AppStateOwner& owner) {
    for (;;) {
        const std::uint64_t updates_before =
            owner.stats().updates_applied + owner.stats().updates_rejected;
        const std::uint64_t events_before = outbox_watermark(owner);
        owner.drain();
        if ((owner.stats().updates_applied + owner.stats().updates_rejected) == updates_before
            && outbox_watermark(owner) == events_before) {
            return;
        }
    }
}

}  // namespace

TEST_CASE("HeyakiAdapter SPI stays abstract with domain-only surface",
    "[unit][heyaki_adapter]") {
    // RULE-10：SPI 为纯虚接口（不可实例化）；本编译单元仅包含 SPI 头、领域头与
    // Catch2，无 executor / Heyaki / 平台类型，可评审复查 include 列表。
    STATIC_REQUIRE(std::is_abstract_v<HeyakiAdapter>);
    STATIC_REQUIRE(std::is_abstract_v<HeyakiAdapterSink>);
    STATIC_REQUIRE(std::is_final_v<FakeHeyakiAdapter>);
    STATIC_REQUIRE(!std::is_copy_constructible_v<HeyakiAdapter>);

    FakeHeyakiAdapter fake;
    AppStateOwner owner;
    BridgeSink bridge(owner);  // 独立编译以证明 Sink 可脱离 executor 使用。
    (void) bridge;
    HeyakiAdapter& adapter = fake;  // IS-A 检查。
    (void) adapter;
}

TEST_CASE("Injected discovery reaches the AppState snapshot and the event outbox",
    "[unit][heyaki_adapter][dod02]") {
    AppStateOwner owner;
    BridgeSink bridge(owner);
    FakeHeyakiAdapter fake;
    fake.set_sink(&bridge);
    auto& executor = aki::test::executor();

    // EXEC-02：注入发生在回调线程（此处为 executor 任务），经有界校验后投递。
    auto injected = executor.submit_auto(
        [&fake] { return fake.inject_device_discovered(make_discovered("dev-1")); });

    REQUIRE(injected.get());
    owner.drain();

    executor::comm::Snapshot<AppState> snapshot;
    REQUIRE(owner.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.devices.devices.size() == 1);
    REQUIRE(snapshot.value.devices.devices.front().id == DeviceId{"dev-1"});

    // 事件主路径同步可见：DeviceDiscoveredEvent 进入必达 FIFO。
    AppEvent event;
    REQUIRE(owner.try_receive_event(event));
    REQUIRE(event.sequence == 1);
    REQUIRE(std::holds_alternative<DeviceDiscoveredEvent>(event.payload));
}

TEST_CASE("Injected connection path change drives the LatestMailbox summary",
    "[unit][heyaki_adapter][latest_mailbox]") {
    AppStateOwner owner;
    BridgeSink bridge(owner);
    FakeHeyakiAdapter fake;
    fake.set_sink(&bridge);

    REQUIRE(fake.inject_connection_path_changed(DeviceId{"dev-1"}, ConnectionPath::Unknown,
        ConnectionPath::Lan));
    REQUIRE(fake.inject_connection_path_changed(DeviceId{"dev-1"}, ConnectionPath::Lan,
        ConnectionPath::P2p));
    REQUIRE(fake.inject_connection_path_changed(DeviceId{"dev-1"}, ConnectionPath::P2p,
        ConnectionPath::Relay));
    owner.drain();

    ConnectionPath path = ConnectionPath::Unknown;
    REQUIRE(owner.try_load_connection_path(path));
    REQUIRE(path == ConnectionPath::Relay);

    // 路径事件按序进入主路径。
    for (const std::uint64_t expected_sequence : {1U, 2U, 3U}) {
        AppEvent event;
        REQUIRE(owner.try_receive_event(event));
        REQUIRE(event.sequence == expected_sequence);
        REQUIRE(std::holds_alternative<ConnectionPathChangedEvent>(event.payload));
    }

    // 设计第 10.1 节：连接路径摘要不落入 Store，不触发快照发布。
    REQUIRE(owner.snapshot_sequence() == 0);
    REQUIRE(owner.stats().snapshots_published == 0);
}

TEST_CASE("Injected messages keep FIFO order on the mandatory event path",
    "[unit][heyaki_adapter]") {
    AppStateOwner owner;
    BridgeSink bridge(owner);
    FakeHeyakiAdapter fake;
    fake.set_sink(&bridge);

    for (std::uint64_t i = 1; i <= 3; ++i) {
        REQUIRE(fake.inject_message_received(make_message("msg-" + std::to_string(i))));
    }
    owner.drain();

    for (std::uint64_t i = 1; i <= 3; ++i) {
        AppEvent event;
        REQUIRE(owner.try_receive_event(event));
        REQUIRE(event.sequence == i);
        REQUIRE(std::holds_alternative<MessageReceivedEvent>(event.payload));
        const auto& received = std::get<MessageReceivedEvent>(event.payload);
        REQUIRE(received.message.id == MessageId{"msg-" + std::to_string(i)});
    }
}

TEST_CASE("Late injected events cannot revive terminal entities (RULE-08)",
    "[unit][heyaki_adapter][late-events]") {
    AppStateOwner owner;
    BridgeSink bridge(owner);
    FakeHeyakiAdapter fake;
    fake.set_sink(&bridge);

    SECTION("late progress after cancellation is rejected by the state machine") {
        REQUIRE(fake.inject_transfer_started(make_transfer("t-1", TransferState::Transferring)));
        owner.drain();
        REQUIRE(owner.submit_update(UpsertTransfer{make_transfer("t-1", TransferState::Cancelled)}));
        owner.drain();

        // 迟到的传输进度：注入/Sink 投递成功（admission），状态机应用层拒绝。
        REQUIRE(fake.inject_transfer_progress(TransferId{"t-1"}, 999, 1024));
        owner.drain();

        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        const auto& transfers = snapshot.value.transfers.transfers;
        REQUIRE(transfers.size() == 1);
        REQUIRE(transfers.front().state == TransferState::Cancelled);
        REQUIRE(transfers.front().transferred == 0);
        REQUIRE(owner.stats().updates_rejected == 1);
    }

    SECTION("late trust change on a revoked device is rejected") {
        // 信任流走合法边：Unknown -> Pending -> Trusted -> Revoked（设计第 4 节）。
        REQUIRE(fake.inject_device_discovered(make_discovered("dev-1", TrustState::Unknown)));
        owner.drain();
        REQUIRE(owner.submit_update(UpsertDevice{make_discovered("dev-1", TrustState::Pending).identity}));
        REQUIRE(owner.submit_update(UpsertDevice{make_discovered("dev-1", TrustState::Trusted).identity}));
        REQUIRE(owner.submit_update(UpsertDevice{make_discovered("dev-1", TrustState::Revoked).identity}));
        owner.drain();

        // 迟到的发现事件携带 Trusted：投递成功，状态机应用层拒绝（终态不复活）。
        REQUIRE(fake.inject_device_discovered(make_discovered("dev-1", TrustState::Trusted)));
        owner.drain();

        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.devices.devices.front().trust_state == TrustState::Revoked);
        REQUIRE(owner.stats().updates_rejected == 1);
    }
}

TEST_CASE("Send-failure report maps to SetDeliveryState(Failed) on the bridge (DEC-006 mapping 4)",
    "[unit][heyaki_adapter]") {
    AppStateOwner owner;
    BridgeSink bridge(owner);

    // 预置：会话 conv-test + Sent 消息 m-1（DEC-009 ② FK 前置校验要求会话先在）。
    aki::conversation::Conversation conversation;
    conversation.id = ConversationId{"conv-test"};
    conversation.local_device = DeviceId{"local"};
    conversation.remote_device = DeviceId{"alpha"};
    REQUIRE(owner.submit_update(aki::app::UpsertConversation{conversation}));
    REQUIRE(owner.submit_update(
        UpsertMessage{make_message("m-1", DeliveryState::Sent),
            ConversationId{"conv-test"}}));
    owner.drain();

    // 失败回报面（sink 第 10 方法）：BridgeSink → SetDeliveryState(Failed)
    //（Sent -> Failed 合法边；无主路径事件）。Fake 无此注入面，经 Sink 直驱。
    REQUIRE(bridge.on_message_send_failed(ConversationId{"conv-test"}, MessageId{"m-1"}));
    owner.drain();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.messages.messages.size() == 1);
        REQUIRE(snapshot.value.messages.messages.front().state == DeliveryState::Failed);
    }

    // 终态幂等：Failed 上的重复失败回报 = 幂等 no-op（接受，不拒绝）。
    const auto rejected_before_repeat = owner.stats().updates_rejected;
    REQUIRE(bridge.on_message_send_failed(ConversationId{"conv-test"}, MessageId{"m-1"}));
    owner.drain();
    REQUIRE(owner.stats().updates_rejected == rejected_before_repeat);

    // 迟到的失败回报不得复活 Delivered（RULE-08）：Sink 投递成功（admission），
    // 状态机应用层拒绝（updates_rejected 增量，Delivered -> Failed 非法）。
    REQUIRE(owner.submit_update(
        UpsertMessage{make_message("m-2", DeliveryState::Delivered),
            ConversationId{"conv-test"}}));
    owner.drain();
    const auto rejected_before_late = owner.stats().updates_rejected;
    REQUIRE(bridge.on_message_send_failed(ConversationId{"conv-test"}, MessageId{"m-2"}));
    owner.drain();
    {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        for (const auto& message : snapshot.value.messages.messages) {
            if (message.id == MessageId{"m-2"}) {
                REQUIRE(message.state == DeliveryState::Delivered);
            }
        }
        REQUIRE(owner.stats().updates_rejected == rejected_before_late + 1);
    }
}

TEST_CASE("Fake rejects injections without sink, with invalid payloads, or under backpressure",
    "[unit][heyaki_adapter][dod02]") {
    AppStateOwner owner;
    BridgeSink bridge(owner);
    FakeHeyakiAdapter naked;
    FakeHeyakiAdapter fake;
    fake.set_sink(&bridge);

    SECTION("no sink attached: every injection is visibly rejected") {
        REQUIRE_FALSE(naked.inject_device_discovered(make_discovered("dev-1")));
        REQUIRE_FALSE(naked.inject_message_received(make_message("m-1")));
        REQUIRE_FALSE(naked.inject_connection_path_changed(DeviceId{"d"}, ConnectionPath::Lan,
            ConnectionPath::P2p));
        REQUIRE_FALSE(naked.inject_transfer_completed(TransferId{"t-1"}, TransferState::Completed));
    }

    SECTION("bounded payload validation (EXEC-02)") {
        REQUIRE_FALSE(fake.inject_device_discovered(make_discovered("")));
        REQUIRE_FALSE(fake.inject_message_received(make_message("")));
        REQUIRE_FALSE(fake.inject_message_delivered(ConversationId{""}, MessageId{"m-1"}));
        REQUIRE_FALSE(fake.inject_device_connected(DeviceId{""}, ConnectionPath::Lan));
        REQUIRE_FALSE(fake.inject_transfer_started(make_transfer("", TransferState::Queued)));
        REQUIRE_FALSE(fake.inject_transfer_progress(TransferId{""}, 1, 2));
        // on_transfer_completed 的 final_state 仅取终态（设计第 10.1 节）。
        REQUIRE_FALSE(fake.inject_transfer_completed(TransferId{"t-1"}, TransferState::Transferring));
        REQUIRE(fake.inject_transfer_completed(TransferId{"t-1"}, TransferState::Failed));
    }

    SECTION("event inbox backpressure is visible through the sink result") {
        AppStateOwner tiny{AppStateOwnerOptions{.event_inbox_capacity = 1}};
        BridgeSink tiny_bridge(tiny);
        FakeHeyakiAdapter tiny_fake;
        tiny_fake.set_sink(&tiny_bridge);

        REQUIRE(tiny_fake.inject_message_received(make_message("m-1")));
        REQUIRE_FALSE(tiny_fake.inject_message_received(make_message("m-2")));  // inbox 满
        tiny.drain();
        REQUIRE(tiny_fake.inject_message_received(make_message("m-3")));
    }

    SECTION("outbound command validation and TransferId semantics") {
        REQUIRE_FALSE(fake.send_text_message(DeviceId{""}, MessageId{"m-1"}, "hi"));
        REQUIRE_FALSE(fake.send_text_message(DeviceId{"beta"}, MessageId{"m-1"}, ""));
        REQUIRE(fake.send_text_message(DeviceId{"beta"}, MessageId{"m-1"}, "hello"));

        const FileMetadata file{"model.gguf", 1024, "application/octet-stream"};
        REQUIRE(fake.start_file_transfer(DeviceId{"beta"}, TransferId{"t-1"}, file));
        REQUIRE_FALSE(fake.start_file_transfer(DeviceId{"beta"}, TransferId{"t-1"}, file));
        REQUIRE(fake.pause_transfer(TransferId{"t-1"}));
        REQUIRE(fake.resume_transfer(TransferId{"t-1"}));
        REQUIRE(fake.cancel_transfer(TransferId{"t-1"}));
        REQUIRE_FALSE(fake.pause_transfer(TransferId{"unknown"}));
        REQUIRE(fake.transfer_session_known("t-1"));
        REQUIRE(fake.transfer_commands().size() == 4);
        REQUIRE(fake.sent_texts().size() == 1);
        REQUIRE(fake.sent_texts().front().text == "hello");
    }
}

TEST_CASE("Discovery and outbound text sending follow the SPI contract",
    "[unit][heyaki_adapter]") {
    AppStateOwner owner;
    BridgeSink bridge(owner);
    FakeHeyakiAdapter fake;
    fake.set_sink(&bridge);

    REQUIRE(fake.start_discovery(DiscoveryMethod::LanDiscovery));
    REQUIRE(fake.discovery_running());
    REQUIRE(fake.start_discovery(DiscoveryMethod::Relay));
    REQUIRE(fake.discovery_started().size() == 2);
    fake.stop_discovery();
    REQUIRE_FALSE(fake.discovery_running());
    fake.stop_discovery();  // 幂等。

    REQUIRE(fake.send_text_message(DeviceId{"beta"}, MessageId{"m-1"}, "hello"));
    const auto& sent = fake.sent_texts();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent.front().to == DeviceId{"beta"});
    REQUIRE(sent.front().message_id == MessageId{"m-1"});
}

TEST_CASE("DOD-02 task exception: visible to caller, injected work survives",
    "[unit][heyaki_adapter][dod02]") {
    AppStateOwner owner;
    BridgeSink bridge(owner);
    FakeHeyakiAdapter fake;
    fake.set_sink(&bridge);
    auto& executor = aki::test::executor();

    const auto before = executor.get_failure_status().task_exception_count;
    auto failing = executor.submit_auto([&fake] {
        if (fake.inject_message_received(make_message("m-crash"))) {
            throw std::runtime_error("boom");
        }
        return false;
    });
    REQUIRE_THROWS_AS(failing.get(), std::runtime_error);
    REQUIRE(wait_until([&] {
        return executor.get_failure_status().task_exception_count >= before + 1;
    }, 2s));

    owner.drain();
    AppEvent event;
    REQUIRE(owner.try_receive_event(event));  // 已入队事件不因任务崩溃丢失。
    REQUIRE(std::holds_alternative<MessageReceivedEvent>(event.payload));
}

TEST_CASE("DOD-02 in-flight cancellation: close releases a waiting event consumer",
    "[unit][heyaki_adapter][dod02]") {
    AppStateOwner owner;
    BridgeSink bridge(owner);
    FakeHeyakiAdapter fake;
    fake.set_sink(&bridge);
    auto& executor = aki::test::executor();

    std::atomic<bool> waiting{false};
    auto consumer = executor.submit_auto([&owner, &waiting] {
        AppEvent event;
        waiting.store(true);
        return owner.receive_event_for(event, std::chrono::seconds(30));
    });
    while (!waiting.load()) {
        std::this_thread::yield();
    }

    owner.close();
    const auto result = consumer.get();
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error_code == executor::comm::CommErrorCode::Closed);

    // 关闭后的注入路径同样明确拒绝（不再有事件入口）。
    REQUIRE_FALSE(fake.inject_message_received(make_message("m-late")));
}

TEST_CASE("DOD-02 timeout: waiting on a quiet event outbox returns Timeout",
    "[unit][heyaki_adapter][dod02]") {
    AppStateOwner owner;
    AppEvent event;
    const auto result = owner.receive_event_for(event, 20ms);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error_code == executor::comm::CommErrorCode::Timeout);
    REQUIRE(owner.event_outbox_stats().timeout_count == 1);
}

TEST_CASE("DOD-02 shutdown: close drains injected backlog and rejects new work",
    "[unit][heyaki_adapter][dod02]") {
    AppStateOwner owner;
    BridgeSink bridge(owner);
    FakeHeyakiAdapter fake;
    fake.set_sink(&bridge);

    REQUIRE(fake.inject_message_received(make_message("m-1")));
    REQUIRE(fake.inject_message_received(make_message("m-2")));
    owner.close();  // lifecycle 顺序：先停生产者（本测试线程），再 close。

    // close 前已入队的注入不丢失：主路径先排空，再返回 Closed。
    AppEvent event;
    REQUIRE(owner.try_receive_event(event));
    REQUIRE(event.sequence == 1);
    REQUIRE(owner.try_receive_event(event));
    REQUIRE(event.sequence == 2);
    const auto after_drain = owner.receive_event_for(event, 20ms);
    REQUIRE_FALSE(after_drain.ok);
    REQUIRE(after_drain.error_code == executor::comm::CommErrorCode::Closed);

    REQUIRE_FALSE(fake.inject_message_received(make_message("m-3")));
    drain_until_idle(owner);
    REQUIRE(owner.stats().events_forwarded == 2);

    executor::comm::Snapshot<AppState> snapshot;
    REQUIRE(owner.try_load_snapshot(snapshot));  // 末次快照仍可读。
    (void) fake.sink();
}

int main(int argc, char* argv[]) {
    // 本进程唯一 Executor owner（AGENTS.md 规则 7）。
    // owner 纪律落点说明（设计第 8.2 节）：此处的裸 Executor 实例是 M1-03 落地时
    // 的测试形态——正式 owner 为 app/lifecycle 的 ExecutorOwner（M1-04 已交付，
    // 见 test_executor_lifecycle）；自 M1-06 冒烟宿主起进程内改用 ExecutorOwner。
    executor::Executor executor;
    const auto initialized = executor.initialize_ex({});
    if (!initialized.ok) {
        std::fputs("executor initialize_ex failed\n", stderr);
        return 1;
    }
    aki::test::use_executor(executor);

    const int result = Catch::Session().run(argc, argv);

    executor.shutdown(true);
    return result;
}
