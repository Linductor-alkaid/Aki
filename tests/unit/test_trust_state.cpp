// TrustState 状态机单测（M1-01）：合法/非法转移、终态幂等、迟到事件不复活。
#include "device/trust/trust_state.hpp"

#include <catch2/catch_test_macros.hpp>

using aki::device::TrustState;
using aki::device::TrustStateMachine;
using aki::device::can_transition;
using aki::device::is_terminal;
using aki::device::to_string;

TEST_CASE("TrustState follows the design transition graph", "[unit][trust]") {
    SECTION("Unknown -> Pending -> Trusted -> Revoked") {
        TrustStateMachine machine;
        REQUIRE(machine.state == TrustState::Unknown);
        REQUIRE(machine.transition_to(TrustState::Pending));
        REQUIRE(machine.transition_to(TrustState::Trusted));
        REQUIRE(machine.transition_to(TrustState::Revoked));
        REQUIRE(machine.state == TrustState::Revoked);
    }

    SECTION("Pending -> Rejected") {
        TrustStateMachine machine;
        REQUIRE(machine.transition_to(TrustState::Pending));
        REQUIRE(machine.transition_to(TrustState::Rejected));
        REQUIRE(machine.state == TrustState::Rejected);
    }
}

TEST_CASE("TrustState rejects illegal transitions", "[unit][trust]") {
    struct Case {
        TrustState from;
        TrustState to;
    };
    const Case illegal[] = {
        {TrustState::Unknown, TrustState::Trusted},
        {TrustState::Unknown, TrustState::Rejected},
        {TrustState::Unknown, TrustState::Revoked},
        {TrustState::Pending, TrustState::Revoked},
        {TrustState::Trusted, TrustState::Pending},
        {TrustState::Trusted, TrustState::Rejected},
        {TrustState::Rejected, TrustState::Pending},
        {TrustState::Rejected, TrustState::Trusted},
        {TrustState::Revoked, TrustState::Pending},
        {TrustState::Revoked, TrustState::Trusted},
    };

    for (const Case& c : illegal) {
        INFO(to_string(c.from) << " -> " << to_string(c.to));
        TrustStateMachine machine;
        machine.state = c.from;
        REQUIRE_FALSE(can_transition(c.from, c.to));
        REQUIRE_FALSE(machine.transition_to(c.to));
        REQUIRE(machine.state == c.from);
    }
}

TEST_CASE("TrustState terminals are idempotent and final", "[unit][trust]") {
    REQUIRE(is_terminal(TrustState::Rejected));
    REQUIRE(is_terminal(TrustState::Revoked));
    REQUIRE_FALSE(is_terminal(TrustState::Unknown));
    REQUIRE_FALSE(is_terminal(TrustState::Pending));
    REQUIRE_FALSE(is_terminal(TrustState::Trusted));

    SECTION("re-announcing a terminal state is a no-op success") {
        TrustStateMachine revoked;
        revoked.state = TrustState::Revoked;
        REQUIRE(revoked.transition_to(TrustState::Revoked));
        REQUIRE(revoked.state == TrustState::Revoked);

        TrustStateMachine rejected;
        rejected.state = TrustState::Rejected;
        REQUIRE(rejected.transition_to(TrustState::Rejected));
        REQUIRE(rejected.state == TrustState::Rejected);
    }

    SECTION("late trust confirmation must not revive a rejected device") {
        TrustStateMachine machine;
        REQUIRE(machine.transition_to(TrustState::Pending));
        REQUIRE(machine.transition_to(TrustState::Rejected));
        REQUIRE_FALSE(machine.transition_to(TrustState::Trusted));
        REQUIRE(machine.state == TrustState::Rejected);
    }

    SECTION("revoked trust cannot be restored by a late event") {
        TrustStateMachine machine;
        machine.state = TrustState::Trusted;
        REQUIRE(machine.transition_to(TrustState::Revoked));
        REQUIRE_FALSE(machine.transition_to(TrustState::Trusted));
        REQUIRE(machine.state == TrustState::Revoked);
    }
}
