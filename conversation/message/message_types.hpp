// 消息领域类型与状态机（设计第 6 节）。
// MessageType 自第一版起类型化（RULE-03）；文件消息只携带 metadata 与 TransferId
// （RULE-05）。DeliveryState 正向链：Queued -> Sending -> Sent -> Delivered，
// 任意非终态可进入 Failed；Delivered / Failed 是终态且幂等。
#pragma once

#include "device/device/device_types.hpp"
#include "transfer/transfer/transfer_types.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace aki::conversation {

using aki::device::DeviceId;
using aki::transfer::FileMetadata;
using aki::transfer::TransferId;

struct MessageId {
    std::string value;

    bool empty() const noexcept { return value.empty(); }
    friend bool operator==(const MessageId&, const MessageId&) = default;
};

enum class MessageType : std::uint8_t {
    Text,
    Image,
    Video,
    File,
    System,
};

constexpr std::string_view to_string(MessageType type) noexcept {
    switch (type) {
        case MessageType::Text: return "Text";
        case MessageType::Image: return "Image";
        case MessageType::Video: return "Video";
        case MessageType::File: return "File";
        case MessageType::System: return "System";
    }
    return "Unknown";
}

enum class DeliveryState : std::uint8_t {
    Queued,
    Sending,
    Sent,
    Delivered,
    Failed,
};

constexpr std::string_view to_string(DeliveryState state) noexcept {
    switch (state) {
        case DeliveryState::Queued: return "Queued";
        case DeliveryState::Sending: return "Sending";
        case DeliveryState::Sent: return "Sent";
        case DeliveryState::Delivered: return "Delivered";
        case DeliveryState::Failed: return "Failed";
    }
    return "Unknown";
}

constexpr bool is_terminal(DeliveryState state) noexcept {
    return state == DeliveryState::Delivered || state == DeliveryState::Failed;
}

constexpr bool can_transition(DeliveryState from, DeliveryState to) noexcept {
    switch (from) {
        case DeliveryState::Queued:
            return to == DeliveryState::Sending || to == DeliveryState::Failed;
        case DeliveryState::Sending:
            return to == DeliveryState::Sent || to == DeliveryState::Failed;
        case DeliveryState::Sent:
            return to == DeliveryState::Delivered || to == DeliveryState::Failed;
        case DeliveryState::Delivered:
        case DeliveryState::Failed:
            return false;
    }
    return false;
}

struct DeliveryStateMachine {
    DeliveryState state{DeliveryState::Queued};

    // 目标与当前一致时视为幂等 no-op 并返回 true；非法转移返回 false 且保持原状态。
    constexpr bool transition_to(DeliveryState to) noexcept {
        if (to == state) {
            return true;
        }
        if (!can_transition(state, to)) {
            return false;
        }
        state = to;
        return true;
    }
};

struct TextPayload {
    std::string text;

    friend bool operator==(const TextPayload&, const TextPayload&) = default;
};

// 图片消息：消息面仅 metadata + TransferId（RULE-05；M4-03 起与 FilePayload
// 同形，图片本体经传输链路）。wire 契约与收发状态联动见设计 §6.1/DEC-010。
struct ImagePayload {
    FileMetadata media;
    TransferId transfer_id;

    friend bool operator==(const ImagePayload&, const ImagePayload&) = default;
};

// 视频消息在 M4 后接入真实数据链路；metadata 结构自本版本固定
// （transfer_id 同构缺口按设计 §6.1 先例随接入补齐）。
struct VideoPayload {
    FileMetadata media;

    friend bool operator==(const VideoPayload&, const VideoPayload&) = default;
};

// 文件消息与文件数据分离：会话内只保存 metadata 与 TransferId（RULE-05）。
struct FilePayload {
    FileMetadata file;
    TransferId transfer_id;

    friend bool operator==(const FilePayload&, const FilePayload&) = default;
};

struct SystemPayload {
    std::string text;

    friend bool operator==(const SystemPayload&, const SystemPayload&) = default;
};

using MessagePayload = std::variant<TextPayload, ImagePayload, VideoPayload, FilePayload, SystemPayload>;

struct Message {
    MessageId id;
    DeviceId sender;
    DeviceId receiver;
    std::chrono::system_clock::time_point timestamp{};
    MessageType type = MessageType::Text;
    MessagePayload payload{TextPayload{}};
    DeliveryState state = DeliveryState::Queued;

    friend bool operator==(const Message&, const Message&) = default;
};

} // namespace aki::conversation
