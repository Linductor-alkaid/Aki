// 状态更新指令（设计第 10/10.1 节）：Manager 侧产生，经 MpscChannel 汇聚到
// 单一状态 owner；owner 是唯一应用更新的地方（单写者，DEC-002 / RULE-02）。
// 对已存在条目的 upsert 按对应领域状态机（M1-01）校验转移合法性：
// 目标状态一致视为幂等 no-op；非法转移与终态复活被拒绝并可观测。
#pragma once

#include "app/state/app_state.hpp"
#include "conversation/message/message_types.hpp"
#include "device/device/device_types.hpp"

#include <cstdint>
#include <variant>

namespace aki::app {

struct UpsertDevice {
    aki::device::DeviceIdentity device;
};

struct UpsertConversation {
    aki::conversation::Conversation conversation;
};

struct UpsertMessage {
    aki::conversation::Message message;
    // 会话归属（DEC-009 ②，M3-05 落地）：MESSAGE 行 FK 的权威来源；owner
    // 前置校验该会话必须已在 ConversationStore（未知会话拒绝可观测）。
    aki::conversation::ConversationId conversation;
};

struct UpsertTransfer {
    aki::transfer::Transfer transfer;
};

// 仅更新既有传输的进度字段；对处于终态（Completed/Failed/Cancelled）的传输
// 一律拒绝——迟到的进度不得复活已终结的传输（RULE-08）。
struct UpdateTransferProgress {
    aki::transfer::TransferId transfer;
    std::uint64_t transferred = 0;
    std::uint64_t total = 0;
};

// 覆盖式单值状态摘要（设计第 10.1 节 LatestMailbox 落点）：owner 应用该更新时
// 同步发布到连接路径 LatestMailbox；不修改任何 Store、不触发快照发布。
struct SetConnectionPath {
    aki::device::ConnectionPath path = aki::device::ConnectionPath::Unknown;
};

// 设备在线状态部分更新（设计第 8.3 节，DeviceManager 依据 connected/disconnected
// 事件维护）：仅改 presence 字段，不触发信任状态机；未知 id 拒绝并可观测。
struct SetPresence {
    aki::device::DeviceId device;
    aki::device::PresenceState presence = aki::device::PresenceState::Offline;
};

// 送达回报部分更新（设计第 8.3 节，MessageManager）：经 DeliveryState 状态机
// 校验；终态幂等，迟到的回报不得让已终结的消息回到活动状态（RULE-08）。
struct SetDeliveryState {
    aki::conversation::MessageId message;
    aki::conversation::DeliveryState state = aki::conversation::DeliveryState::Queued;
};

// 传输终态宣告（设计第 8.3/10.1 节，TransferManager）：final_state 仅取
// Completed / Failed / Cancelled；经 TransferState 状态机校验，非法转移
// （如 Paused -> Completed）与终态复活拒绝并可观测（RULE-08）。
struct CompleteTransfer {
    aki::transfer::TransferId transfer;
    aki::transfer::TransferState final_state = aki::transfer::TransferState::Completed;
};

using AppStateUpdate = std::variant<UpsertDevice,
    UpsertConversation,
    UpsertMessage,
    UpsertTransfer,
    UpdateTransferProgress,
    SetConnectionPath,
    SetPresence,
    SetDeliveryState,
    CompleteTransfer>;

}  // namespace aki::app
