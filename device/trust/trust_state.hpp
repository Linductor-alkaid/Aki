// 信任状态机（设计第 4 节）。
// 转移规则：Unknown -> Pending -> Trusted；Pending -> Rejected；Trusted -> Revoked。
// Rejected / Revoked 是终态；终态幂等，迟到的事件不得让已终结的信任关系回到活动状态。
#pragma once

#include <cstdint>
#include <string_view>

namespace aki::device {

enum class TrustState : std::uint8_t {
    Unknown,
    Pending,
    Trusted,
    Rejected,
    Revoked,
};

constexpr std::string_view to_string(TrustState state) noexcept {
    switch (state) {
        case TrustState::Unknown: return "Unknown";
        case TrustState::Pending: return "Pending";
        case TrustState::Trusted: return "Trusted";
        case TrustState::Rejected: return "Rejected";
        case TrustState::Revoked: return "Revoked";
    }
    return "Unknown";
}

constexpr bool is_terminal(TrustState state) noexcept {
    return state == TrustState::Rejected || state == TrustState::Revoked;
}

// 合法转移边（不含同状态 no-op，见 TrustStateMachine::transition_to）。
constexpr bool can_transition(TrustState from, TrustState to) noexcept {
    switch (from) {
        case TrustState::Unknown:
            return to == TrustState::Pending;
        case TrustState::Pending:
            return to == TrustState::Trusted || to == TrustState::Rejected;
        case TrustState::Trusted:
            return to == TrustState::Revoked;
        case TrustState::Rejected:
        case TrustState::Revoked:
            return false;
    }
    return false;
}

struct TrustStateMachine {
    TrustState state{TrustState::Unknown};

    // 尝试转移到 to。目标与当前一致时视为幂等 no-op 并返回 true（覆盖终态重复宣告）；
    // 非法转移返回 false 且保持原状态。
    constexpr bool transition_to(TrustState to) noexcept {
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

} // namespace aki::device
