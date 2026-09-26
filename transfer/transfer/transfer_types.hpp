// Transfer 领域类型与状态机（设计第 7 节）。
// TransferState：Queued -> Negotiating -> Transferring；Transferring <-> Paused；
// 任意活动态可进入 Failed / Cancelled。Completed / Failed / Cancelled 是终态，
// 终态幂等：迟到的进度或结果事件不得复活已终结的传输（RULE-08）。
#pragma once

#include "device/device/device_types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace aki::transfer {

using aki::device::DeviceId;

struct TransferId {
    std::string value;

    bool empty() const noexcept { return value.empty(); }
    friend bool operator==(const TransferId&, const TransferId&) = default;
};

// 文件 metadata（设计第 7/11 节）。文件本体不经消息通道传输（RULE-05）；
// 发送侧本地存储路径不经本类型（本结构随消息载荷对端可见，携带本地路径即信息
// 外泄）——经出站 SPI 参数传递（设计 §7.1⑤/§8.1，2026-09-24 定案）。
// stored_sha256（M4-04，§7.1④/DEC-010 字段 5）：发送方对源文件发送前的
// 流式 SHA-256（64 字符小写 hex），随载荷携带供接收方对账——**wire + 内存
// 字段**：message 表无对应列，消息行重启重建时为空（传输行 stored_* 回写列
// 是持久权威，DEC-011 风险登记）。
struct FileMetadata {
    std::string name;
    std::uint64_t size_bytes = 0;
    std::string mime_type;
    std::string stored_sha256;

    friend bool operator==(const FileMetadata&, const FileMetadata&) = default;
};

enum class TransferState : std::uint8_t {
    Queued,
    Negotiating,
    Transferring,
    Paused,
    Completed,
    Failed,
    Cancelled,
};

constexpr std::string_view to_string(TransferState state) noexcept {
    switch (state) {
        case TransferState::Queued: return "Queued";
        case TransferState::Negotiating: return "Negotiating";
        case TransferState::Transferring: return "Transferring";
        case TransferState::Paused: return "Paused";
        case TransferState::Completed: return "Completed";
        case TransferState::Failed: return "Failed";
        case TransferState::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

constexpr bool is_terminal(TransferState state) noexcept {
    return state == TransferState::Completed || state == TransferState::Failed
        || state == TransferState::Cancelled;
}

constexpr bool can_transition(TransferState from, TransferState to) noexcept {
    switch (from) {
        case TransferState::Queued:
            return to == TransferState::Negotiating || to == TransferState::Cancelled;
        case TransferState::Negotiating:
            return to == TransferState::Transferring || to == TransferState::Failed
                || to == TransferState::Cancelled;
        case TransferState::Transferring:
            return to == TransferState::Paused || to == TransferState::Completed
                || to == TransferState::Failed || to == TransferState::Cancelled;
        case TransferState::Paused:
            // 暂停态仍持有会话资源，允许恢复、取消或观察到失败；完成必须先恢复。
            return to == TransferState::Transferring || to == TransferState::Failed
                || to == TransferState::Cancelled;
        case TransferState::Completed:
        case TransferState::Failed:
        case TransferState::Cancelled:
            return false;
    }
    return false;
}

struct TransferStateMachine {
    TransferState state{TransferState::Queued};

    // 目标与当前一致时视为幂等 no-op 并返回 true（覆盖终态重复宣告）；
    // 非法转移返回 false 且保持原状态。
    constexpr bool transition_to(TransferState to) noexcept {
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

// 传输会话（设计第 7 节）。字段更新由 Transfer Manager 在其执行上下文内完成。
struct Transfer {
    TransferId id;
    DeviceId sender;
    DeviceId receiver;
    FileMetadata file;
    std::uint64_t transferred = 0;
    std::uint64_t total = 0;
    TransferState state = TransferState::Queued;

    friend bool operator==(const Transfer&, const Transfer&) = default;
};

} // namespace aki::transfer
