// Aki console 冒烟宿主（M1-06；设计第 8.3 节组合根的最小进程内实现，第 15 节
// MVP 链路演示，v0.1.0 验收载体 / M1 退出-1）。
//
// 进程内 Executor owner 自本项起为正式 ExecutorOwner（设计第 8.2 节落点说明，
// AGENTS 规则 7/8）：Manager 排空泵与传输会话等全部任务都在其生命周期内，
// 无 std::thread / std::async / 自建线程（RULE-07）。
//
// 演示脚本：两台假设备（local-1 本机 / alpha-01 远端）经 FakeHeyakiAdapter 的
// inject_* 编程式注入完成"发现 -> 信任（Pending -> Trusted）-> 文本消息
// （send_text + delivered/received）-> 断开 -> 重连"，经 DoubleBuffer 一致快照与
// 序列号排序的必达事件主路径逐步断言（消息 FIFO 顺序与 TrustState /
// ConversationState / DeliveryState / PresenceState 转换），输出人可读控制台结果；
// 结束按设计第 8.3 节受控关闭钩子顺序收尾，以 ExecutorOwner 的 fully_stopped
// 证据作为 DOD-02 shutdown 项在集成层的可见证据。
//
// 确定性：inject_* 与出站命令全部由主线程串行驱动（FakeHeyakiAdapter 的宿主
// 串行化契约，EXEC-02），每步经 flush（有界预算）+ owner drain 推进到静止后再
// 断言，不依赖时序；同一可执行文件连续多次运行输出一致。
//
// 设备信任确认属用户流程（设计第 4/8.3 节）：M1 无 Trust Manager 与 UI，由宿主
// 经 AppStateOwner 的 UpsertDevice 更新指令模拟用户确认（owner 侧按信任状态机
// 校验合法边），UI 于 M5 接入。
#include "app/application/router_sink.hpp"
#include "app/lifecycle/executor_owner.hpp"
#include "app/state/app_state_owner.hpp"
#include "heyaki/adapter/fake_heyaki_adapter.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
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

int g_checks_failed = 0;

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

int run_demo() {
    std::printf(
        "aki 0.1.0 (M1 smoke: discovery -> trust -> messaging -> disconnect -> "
        "reconnect)\n");
    std::printf(
        "devices: local-1 (this host) / alpha-01 (remote, FakeHeyakiAdapter)\n");

    // ---- 组合根（设计第 8.3 节装配顺序）----
    // 1) ExecutorOwner.initialize()——进程内唯一 Executor 生命周期 owner。
    ExecutorOwner executor_owner;
    if (!executor_owner.initialize()) {
        std::printf("[FATAL] ExecutorOwner.initialize() failed\n");
        return 1;
    }
    // 2) AppStateOwner（owner 上下文 = 主线程，单写者，RULE-02/EXEC-03）。
    AppStateOwner state_owner{};
    // 3) FakeHeyakiAdapter 与四 Manager（构造注入 executor、owner、adapter 与
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

    // 4) RouterSink 经 FakeHeyakiAdapter::set_sink 注册。
    RouterSink router{devices, conversations, messages, transfers};
    adapter.set_sink(&router);

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

    // ---- 步骤 1/6：发现（discovered + connected）----
    std::printf("\n[step 1/6] discovery\n");
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

    // ---- 步骤 2/6：信任（Pending -> Trusted，用户流程模拟）----
    std::printf("\n[step 2/6] trust (Pending -> Trusted)\n");
    DeviceIdentity pending = alpha;
    pending.trust_state = TrustState::Pending;
    report(state_owner.submit_update(UpsertDevice{pending}),
        "trust: Unknown -> Pending accepted by trust state machine");
    quiesce();
    DeviceIdentity trusted = alpha;
    trusted.trust_state = TrustState::Trusted;
    report(state_owner.submit_update(UpsertDevice{trusted}),
        "trust: Pending -> Trusted accepted by trust state machine");
    quiesce();
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

    // ---- 步骤 3/6：会话 + 文本消息（send_text + delivered/received）----
    std::printf("\n[step 3/6] conversation + text messaging\n");
    report(conversations.ensure_conversation(DeviceId{"local-1"}, DeviceId{"alpha-01"}),
        "ensure_conversation(local-1, alpha-01)");
    quiesce();
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
        report(snapshot.value.conversations.conversations.size() == 1,
            "one conversation");
        if (snapshot.value.conversations.conversations.size() == 1) {
            const auto& conversation = snapshot.value.conversations.conversations.front();
            report(conversation.id == ConversationId{"conv-alpha-01"},
                "conversation id == conv-alpha-01");
            report(conversation.state == ConversationState::Active,
                "conversation state == Active ("
                    + enum_text(conversation.state) + ")");
        }
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

    // ---- 步骤 4/6：断开（disconnected）----
    std::printf("\n[step 4/6] disconnect\n");
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
    }
    {
        auto events = consume_events();
        report(events.size() == 1, "one event on the main path");
        if (events.size() == 1) {
            report(std::holds_alternative<DeviceDisconnectedEvent>(events[0].payload),
                "event 5 is DeviceDisconnected");
        }
    }

    // ---- 步骤 5/6：重连（connected -> Active，同一会话与历史保持，RULE-06）----
    std::printf("\n[step 5/6] reconnect\n");
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

    // ---- 步骤 6/6：受控关闭（设计第 8.3 节钩子顺序 -> EXEC-01 步骤 2~5）----
    std::printf("\n[step 6/6] controlled shutdown\n");
    const auto shutdown_report = executor_owner.shutdown([&] {
        // EXEC-01 步骤 1 钩子（设计第 8.3 节顺序）：
        (void)transfers.request_cancel_all();  // ① 取消在途可取消任务（无在飞时幂等）
        (void)transfers.flush(2s);             // ② flush 各 Manager 至泵静止并消费 future
        (void)devices.flush(2s);
        (void)conversations.flush(2s);
        (void)messages.flush(2s);
        adapter.set_sink(nullptr);             // ③ 停 Adapter 投递
        adapter.stop_discovery();
        state_owner.close();                   // ④ comm 关闭（主线程 = owner 上下文）
    });
    report(shutdown_report.fully_stopped(),
        "shutdown fully_stopped (Completed + lifecycle Stopped + wait_timeout_count==0)");
    report(state_owner.is_closed(), "AppStateOwner closed");
    report(!adapter.inject_message_received(reply),
        "injection after delivery stopped is rejected");

    std::printf(
        "\nsmoke: %s (%d check(s) failed)\n",
        g_checks_failed == 0 ? "PASS" : "FAIL", g_checks_failed);
    return g_checks_failed == 0 ? 0 : 1;
}

}  // namespace

int main() {
    // 进程内唯一 Executor owner 为 run_demo 内的 ExecutorOwner（AGENTS 规则 7/8；
    // 设计第 8.2/8.3 节）：受控关闭在 run_demo 内显式完成，析构兜底仅覆盖
    // 异常提前返回的路径。
    return run_demo();
}
