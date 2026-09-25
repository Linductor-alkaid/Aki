// Message Manager 骨架（设计第 8.3 节，DEC-008；M1-05）。
//
// 只写 messages Store：received 事件按第 6 节把收到的消息记录为 Delivered；
// delivered 事件 → SetDeliveryState（终态幂等，迟到的回报不复活终态，RULE-08）；
// 文本发送是本域出站操作（→ Adapter）：入队时本地记录 Queued，Adapter admission
// 成功记录 Sent、失败记录 Failed（M3 起由真实投递回报推进中间状态）。
// 图片消息发送面（M4-03，设计 §6.1/§8.1）：同型 admission 语义，载荷仅
// FileMetadata + TransferId（RULE-05）；发送侧准入闸门在编排层
// （image_flow.hpp），MM 经 transfer_admitted 标记接收闸门结论、不感知
// TransferManager。
//
// 生命周期（EXEC-07）同 DeviceManager；发送命令经单飞泵串行化，Fake/真实
// Adapter 的出站调用只在 Manager 上下文发生（EXEC-02）。
#pragma once

#include "app/application/manager_runtime.hpp"
#include "app/state/app_events.hpp"
#include "app/state/app_state_owner.hpp"
#include "app/state/app_state_updates.hpp"
#include "conversation/message/message_types.hpp"
#include "device/device/device_types.hpp"
#include "heyaki/adapter/heyaki_adapter.hpp"
#include "transfer/transfer/transfer_types.hpp"

#include <chrono>
#include <string>
#include <utility>
#include <variant>

namespace aki::app {

struct MessageReceivedWork {
    aki::conversation::Message message;
};

struct MessageDeliveredWork {
    aki::conversation::ConversationId conversation;
    aki::conversation::MessageId message;
};

struct MessageDeliveryFailedWork {
    aki::conversation::ConversationId conversation;
    aki::conversation::MessageId message;
};

struct SendTextWork {
    aki::device::DeviceId to;
    aki::conversation::MessageId message_id;
    std::string text;
};

// 图片消息出站（M4-03，设计 §6.1/§8.1；DEC-010）。transfer_admitted =
// false 表示发送侧准入闸门的传输半边已失败（设计 §6.1②：不发送消息、消息行
// 记 Failed、Adapter 零 send 调用）——编排层（image_flow.hpp）置位，MM 不
// 感知 TransferManager。
struct SendImageWork {
    aki::device::DeviceId to;
    aki::conversation::MessageId message_id;
    aki::transfer::FileMetadata media;
    aki::transfer::TransferId transfer_id;
    bool transfer_admitted = true;
};

using MessageManagerWork = std::variant<MessageReceivedWork,
    MessageDeliveredWork,
    MessageDeliveryFailedWork,
    SendTextWork,
    SendImageWork>;

// 构造选项置于命名空间作用域（同 AppStateOwnerOptions 处理，GCC 纪律）。
struct MessageManagerOptions {
    ManagerPumpOptions pump{};
    aki::device::DeviceId local_device;  // 出站消息的 sender。
    // 会话 id 派生前缀（与 ConversationManagerOptions::conversation_id_prefix
    // 一致；DEC-009 ②：UpsertMessage 归属的权威解析——对端设备 → 会话）。
    std::string conversation_id_prefix = "conv-";

    // 对端设备 → 会话 id（ensure_conversation 的既有 id 方案）。
    [[nodiscard]] aki::conversation::ConversationId conversation_for(
        const aki::device::DeviceId& remote) const noexcept {
        return aki::conversation::ConversationId{
            conversation_id_prefix + remote.value};
    }
};

class MessageManager {
public:
    using Options = MessageManagerOptions;

    MessageManager(executor::Executor& executor, AppStateOwner& state_owner,
        aki::heyaki::HeyakiAdapter& adapter, MessageManagerOptions options = {})
        : options_(std::move(options)),
          state_owner_(state_owner),
          adapter_(adapter),
          pump_(executor, options_.pump,
              [this](MessageManagerWork& work) { return handle(work); }) {}

    MessageManager(const MessageManager&) = delete;
    MessageManager& operator=(const MessageManager&) = delete;

    // ---- Sink 路由入口 ----

    [[nodiscard]] bool enqueue_message_received(aki::conversation::Message message) {
        return pump_.enqueue(MessageReceivedWork{std::move(message)});
    }

    // 出站投递回报终态失败（DEC-006 映射 4）：Failed 终态（幂等，RULE-08）。
    [[nodiscard]] bool enqueue_message_send_failed(
        aki::conversation::ConversationId conversation,
        aki::conversation::MessageId message) {
        return pump_.enqueue(MessageDeliveryFailedWork{
            std::move(conversation), std::move(message)});
    }

    [[nodiscard]] bool enqueue_message_delivered(aki::conversation::ConversationId conversation,
        aki::conversation::MessageId message) {
        return pump_.enqueue(
            MessageDeliveredWork{std::move(conversation), std::move(message)});
    }

    // ---- 本域出站操作：文本消息发送（设计第 6 节；RULE-08 稳定 id）----

    [[nodiscard]] bool send_text(aki::device::DeviceId to,
        aki::conversation::MessageId message_id, std::string text) {
        return pump_.enqueue(
            SendTextWork{std::move(to), std::move(message_id), std::move(text)});
    }

    // ---- 本域出站操作：图片消息发送（M4-03，设计 §6.1/§8.1）----
    // 消息面仅 metadata + TransferId（RULE-05）；transfer_admitted 语义见
    // SendImageWork（编排层闸门，默认 true = 正常发送路径）。

    [[nodiscard]] bool send_image(aki::device::DeviceId to,
        aki::conversation::MessageId message_id,
        aki::transfer::FileMetadata media, aki::transfer::TransferId transfer_id,
        bool transfer_admitted = true) {
        return pump_.enqueue(SendImageWork{std::move(to),
            std::move(message_id), std::move(media), std::move(transfer_id),
            transfer_admitted});
    }

    [[nodiscard]] bool flush(std::chrono::milliseconds budget) {
        return pump_.flush(budget);
    }

    [[nodiscard]] ManagerPumpStats::Snapshot stats() const noexcept {
        return pump_.stats();
    }

private:
    bool handle(MessageManagerWork& work) {
        return std::visit([this](auto& item) { return handle(item); }, work);
    }

    bool handle(MessageReceivedWork& work) {
        if (work.message.id.empty()) {
            return false;
        }
        // 收到的消息在本地记录为 Delivered（设计第 6 节）；会话归属由
        // sender 派生（DEC-009 ②；会话须已由 ensure_conversation 建立，
        // 未知会话经 owner FK 前置校验拒绝可观测）。
        work.message.state = aki::conversation::DeliveryState::Delivered;
        const bool posted = post_event(MessageReceivedEvent{work.message});
        const bool applied = state_owner_.submit_update(UpsertMessage{
            work.message, options_.conversation_for(work.message.sender)});
        return posted && applied;
    }

    bool handle(MessageDeliveredWork& work) {
        if (work.message.empty()) {
            return false;
        }
        const bool posted =
            post_event(MessageDeliveredEvent{work.conversation, work.message});
        const bool applied = state_owner_.submit_update(SetDeliveryState{
            work.message, aki::conversation::DeliveryState::Delivered});
        return posted && applied;
    }

    // DEC-006 映射 4：send_failed/peer_rejected/ack_timeout/session_closed →
    // Failed 终态（无主路径事件；迟到回报不复活终态，RULE-08 owner 侧校验）。
    bool handle(MessageDeliveryFailedWork& work) {
        if (work.message.empty()) {
            return false;
        }
        return state_owner_.submit_update(SetDeliveryState{
            work.message, aki::conversation::DeliveryState::Failed});
    }

    bool handle(SendTextWork& work) {
        if (work.to.empty() || work.message_id.empty() || work.text.empty()) {
            return false;
        }
        aki::conversation::Message message;
        message.id = work.message_id;
        message.sender = options_.local_device;
        message.receiver = work.to;
        message.timestamp = std::chrono::system_clock::now();
        message.type = aki::conversation::MessageType::Text;
        message.payload = aki::conversation::TextPayload{work.text};
        message.state = aki::conversation::DeliveryState::Queued;
        const bool accepted =
            adapter_.send_text_message(work.to, work.message_id, work.text);
        // M1 语义：Adapter admission 成功 = Sent；失败 = Failed（Queued -> Failed
        // 是合法边）。真实投递回报经 on_message_delivered 推进（M3 起含中间态）。
        message.state = accepted ? aki::conversation::DeliveryState::Sent
                                 : aki::conversation::DeliveryState::Failed;
        return state_owner_.submit_update(UpsertMessage{std::move(message),
            options_.conversation_for(work.to)});
    }

    // 图片消息出站（M4-03）：admission 语义同文本（成功 = Sent、失败 =
    // Failed）；transfer_admitted = false 时跳过 Adapter 调用直接记 Failed
    //（设计 §6.1② 闸门的传输半边失败路径——Fake adapter 零 send 调用）。
    bool handle(SendImageWork& work) {
        if (work.to.empty() || work.message_id.empty()
            || work.media.name.empty() || work.transfer_id.empty()) {
            return false;
        }
        aki::conversation::Message message;
        message.id = work.message_id;
        message.sender = options_.local_device;
        message.receiver = work.to;
        message.timestamp = std::chrono::system_clock::now();
        message.type = aki::conversation::MessageType::Image;
        message.payload =
            aki::conversation::ImagePayload{work.media, work.transfer_id};
        message.state = aki::conversation::DeliveryState::Queued;
        bool accepted = false;
        if (work.transfer_admitted) {
            accepted = adapter_.send_image_message(
                work.to, work.message_id, work.media, work.transfer_id);
        }
        message.state = accepted ? aki::conversation::DeliveryState::Sent
                                 : aki::conversation::DeliveryState::Failed;
        return state_owner_.submit_update(UpsertMessage{std::move(message),
            options_.conversation_for(work.to)});
    }

    template <typename Payload>
    bool post_event(Payload payload) {
        AppEvent event;
        event.payload = std::move(payload);
        return state_owner_.post_event(std::move(event));
    }

    Options options_;
    AppStateOwner& state_owner_;
    aki::heyaki::HeyakiAdapter& adapter_;
    ManagerPump<MessageManagerWork> pump_;
};

}  // namespace aki::app
