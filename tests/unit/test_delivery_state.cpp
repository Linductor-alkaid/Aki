// DeliveryState 状态机单测（M1-01）：正向链、失败入口、终态幂等。
#include "conversation/message/message_types.hpp"

#include <catch2/catch_test_macros.hpp>

using aki::conversation::DeliveryState;
using aki::conversation::DeliveryStateMachine;
using aki::conversation::can_transition;
using aki::conversation::is_terminal;
using aki::conversation::to_string;

TEST_CASE("DeliveryState follows the forward chain", "[unit][delivery]") {
    DeliveryStateMachine machine;
    REQUIRE(machine.state == DeliveryState::Queued);
    REQUIRE(machine.transition_to(DeliveryState::Sending));
    REQUIRE(machine.transition_to(DeliveryState::Sent));
    REQUIRE(machine.transition_to(DeliveryState::Delivered));
    REQUIRE(machine.state == DeliveryState::Delivered);
}

TEST_CASE("DeliveryState can fail from any non-terminal state", "[unit][delivery]") {
    for (const DeliveryState from : {DeliveryState::Queued, DeliveryState::Sending,
             DeliveryState::Sent}) {
        INFO(to_string(from) << " -> Failed");
        DeliveryStateMachine machine;
        machine.state = from;
        REQUIRE(machine.transition_to(DeliveryState::Failed));
        REQUIRE(machine.state == DeliveryState::Failed);
    }
}

TEST_CASE("DeliveryState rejects skips and backwards transitions", "[unit][delivery]") {
    struct Case {
        DeliveryState from;
        DeliveryState to;
    };
    const Case illegal[] = {
        {DeliveryState::Queued, DeliveryState::Sent},
        {DeliveryState::Queued, DeliveryState::Delivered},
        {DeliveryState::Sending, DeliveryState::Delivered},
        {DeliveryState::Sent, DeliveryState::Sending},
        {DeliveryState::Sent, DeliveryState::Queued},
    };

    for (const Case& c : illegal) {
        INFO(to_string(c.from) << " -> " << to_string(c.to));
        DeliveryStateMachine machine;
        machine.state = c.from;
        REQUIRE_FALSE(can_transition(c.from, c.to));
        REQUIRE_FALSE(machine.transition_to(c.to));
        REQUIRE(machine.state == c.from);
    }
}

TEST_CASE("DeliveryState terminals are idempotent and final", "[unit][delivery]") {
    REQUIRE(is_terminal(DeliveryState::Delivered));
    REQUIRE(is_terminal(DeliveryState::Failed));
    REQUIRE_FALSE(is_terminal(DeliveryState::Queued));

    SECTION("late sent report must not revive a delivered message") {
        DeliveryStateMachine machine;
        machine.state = DeliveryState::Delivered;
        REQUIRE(machine.transition_to(DeliveryState::Delivered));
        REQUIRE_FALSE(machine.transition_to(DeliveryState::Sent));
        REQUIRE(machine.state == DeliveryState::Delivered);
    }

    SECTION("a failed message does not restart from a late event") {
        DeliveryStateMachine machine;
        machine.state = DeliveryState::Failed;
        REQUIRE(machine.transition_to(DeliveryState::Failed));
        REQUIRE_FALSE(machine.transition_to(DeliveryState::Sending));
        REQUIRE(machine.state == DeliveryState::Failed);
    }
}
