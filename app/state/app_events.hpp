// 设计第 10 节事件模型的类型化 C++ 表示（RULE-03 纪律同样适用于事件 schema）。
// 共 9 类事件；其中 transfer completed 承载传输终态结果
// （Completed / Failed / Cancelled），迟到事件不得让已终结的传输回到活动状态
// （RULE-08）。事件序列号由状态 owner 单写者分配（设计第 10.1 节）。
#pragma once

#include "app/state/app_state.hpp"
#include "device/discovery/discovery_types.hpp"

#include <cstdint>
#include <memory>
#include <variant>

namespace aki::app {

struct DeviceDiscoveredEvent {
    aki::device::DiscoveredDevice device;
};

struct DeviceConnectedEvent {
    aki::device::DeviceId device;
    aki::device::ConnectionPath path = aki::device::ConnectionPath::Unknown;
};

struct DeviceDisconnectedEvent {
    aki::device::DeviceId device;
};

struct MessageReceivedEvent {
    aki::conversation::Message message;
};

struct MessageDeliveredEvent {
    aki::conversation::ConversationId conversation;
    aki::conversation::MessageId message;
};

struct TransferStartedEvent {
    aki::transfer::Transfer transfer;
};

struct TransferProgressEvent {
    aki::transfer::TransferId transfer;
    std::uint64_t transferred = 0;
    std::uint64_t total = 0;
};

// 终态结果：final_state 仅取 Completed / Failed / Cancelled（设计第 10.1 节）。
struct TransferCompletedEvent {
    aki::transfer::TransferId transfer;
    aki::transfer::TransferState final_state = aki::transfer::TransferState::Completed;
};

struct ConnectionPathChangedEvent {
    aki::device::DeviceId device;
    aki::device::ConnectionPath from = aki::device::ConnectionPath::Unknown;
    aki::device::ConnectionPath to = aki::device::ConnectionPath::Unknown;
};

using AppEventPayload = std::variant<DeviceDiscoveredEvent,
    DeviceConnectedEvent,
    DeviceDisconnectedEvent,
    MessageReceivedEvent,
    MessageDeliveredEvent,
    TransferStartedEvent,
    TransferProgressEvent,
    TransferCompletedEvent,
    ConnectionPathChangedEvent>;

// sequence 由状态 owner 在事件进入投递主路径时分配，单调递增；0 表示未分配。
struct AppEvent {
    std::uint64_t sequence = 0;
    AppEventPayload payload{};
};

// Topic 广播载荷（设计第 10.1 节）：不可变事件以 shared_ptr 扇出，
// 避免 Topic 对大事件逐订阅者复制；shared_ptr 本身可复制，满足 Topic<T> 契约。
using AppEventPtr = std::shared_ptr<const AppEvent>;

}  // namespace aki::app
