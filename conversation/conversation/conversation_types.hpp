// Conversation 领域类型与状态机（设计第 5 节）。
// ConversationState：Active <-> Disconnected；Active / Disconnected -> Archived；
// Archived 是终态。连接路径切换不改变会话与消息历史（RULE-06）。
#pragma once

#include "device/device/device_types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace aki::conversation {

using aki::device::DeviceId;

struct ConversationId {
    std::string value;

    bool empty() const noexcept { return value.empty(); }
    friend bool operator==(const ConversationId&, const ConversationId&) = default;
};

enum class ConversationState : std::uint8_t {
    Active,
    Disconnected,
    Archived,
};

constexpr std::string_view to_string(ConversationState state) noexcept {
    switch (state) {
        case ConversationState::Active: return "Active";
        case ConversationState::Disconnected: return "Disconnected";
        case ConversationState::Archived: return "Archived";
    }
    return "Unknown";
}

constexpr bool is_terminal(ConversationState state) noexcept {
    return state == ConversationState::Archived;
}

constexpr bool can_transition(ConversationState from, ConversationState to) noexcept {
    switch (from) {
        case ConversationState::Active:
            return to == ConversationState::Disconnected || to == ConversationState::Archived;
        case ConversationState::Disconnected:
            return to == ConversationState::Active || to == ConversationState::Archived;
        case ConversationState::Archived:
            return false;
    }
    return false;
}

struct ConversationStateMachine {
    ConversationState state{ConversationState::Active};

    // 目标与当前一致时视为幂等 no-op 并返回 true；非法转移返回 false 且保持原状态。
    constexpr bool transition_to(ConversationState to) noexcept {
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

struct Conversation {
    ConversationId id;
    DeviceId local_device;
    DeviceId remote_device;
    ConversationState state = ConversationState::Active;

    friend bool operator==(const Conversation&, const Conversation&) = default;
};

} // namespace aki::conversation
