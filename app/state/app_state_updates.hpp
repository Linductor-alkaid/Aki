// 状态更新指令（设计第 10/10.1 节）：Manager 侧产生，经 MpscChannel 汇聚到
// 单一状态 owner；owner 是唯一应用更新的地方（单写者，DEC-002 / RULE-02）。
// 对已存在条目的 upsert 按对应领域状态机（M1-01）校验转移合法性：
// 目标状态一致视为幂等 no-op；非法转移与终态复活被拒绝并可观测。
#pragma once

#include "app/state/app_state.hpp"
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

using AppStateUpdate = std::variant<UpsertDevice,
    UpsertConversation,
    UpsertMessage,
    UpsertTransfer,
    UpdateTransferProgress,
    SetConnectionPath>;

}  // namespace aki::app
