// ConversationState 状态机单测（M1-01）：断线恢复往返、归档终态、路径切换不换会话。
#include "conversation/conversation/conversation_types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using aki::conversation::Conversation;
using aki::conversation::ConversationId;
using aki::conversation::ConversationState;
using aki::conversation::ConversationStateMachine;
using aki::conversation::can_transition;
using aki::conversation::is_terminal;
using aki::device::ConnectionPath;
using aki::device::DeviceId;

TEST_CASE("Conversation survives disconnect and reconnect", "[unit][conversation]") {
    Conversation conversation{
        ConversationId{"conv-1"}, DeviceId{"local"}, DeviceId{"remote"},
        ConversationState::Active};

    ConversationStateMachine machine;
    machine.state = conversation.state;

    // 断线与恢复不创建新会话（设计第 5/15 节，SCOPE-11）。
    REQUIRE(machine.transition_to(ConversationState::Disconnected));
    conversation.state = machine.state;
    REQUIRE(machine.transition_to(ConversationState::Active));
    conversation.state = machine.state;

    REQUIRE(conversation.id == ConversationId{"conv-1"});
    REQUIRE(conversation.state == ConversationState::Active);

    // 连接路径变化不属于 ConversationState。
    STATIC_REQUIRE(std::string_view{aki::device::to_string(ConnectionPath::Lan)}
                   == "LAN");
}

TEST_CASE("ConversationState transitions", "[unit][conversation]") {
    SECTION("Active -> Disconnected -> Active") {
        ConversationStateMachine machine;
        REQUIRE(machine.transition_to(ConversationState::Disconnected));
        REQUIRE(machine.transition_to(ConversationState::Active));
    }

    SECTION("Active -> Archived") {
        ConversationStateMachine machine;
        REQUIRE(machine.transition_to(ConversationState::Archived));
        REQUIRE(machine.state == ConversationState::Archived);
    }

    SECTION("Disconnected -> Archived") {
        ConversationStateMachine machine;
        REQUIRE(machine.transition_to(ConversationState::Disconnected));
        REQUIRE(machine.transition_to(ConversationState::Archived));
        REQUIRE(machine.state == ConversationState::Archived);
    }
}

TEST_CASE("ConversationState terminals are idempotent and final", "[unit][conversation]") {
    REQUIRE(is_terminal(ConversationState::Archived));
    REQUIRE_FALSE(is_terminal(ConversationState::Disconnected));

    ConversationStateMachine machine;
    machine.state = ConversationState::Archived;
    REQUIRE(machine.transition_to(ConversationState::Archived));
    REQUIRE_FALSE(machine.transition_to(ConversationState::Active));
    REQUIRE_FALSE(machine.transition_to(ConversationState::Disconnected));
    REQUIRE(machine.state == ConversationState::Archived);
}
