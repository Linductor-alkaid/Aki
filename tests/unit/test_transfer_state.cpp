// TransferState 状态机单测（M1-01；M4-06 重启降级边）：正常完成、暂停恢复、
// 取消/失败入口、重启降级（Queued/Negotiating -> Paused，DEC-013）、终态幂等。
#include "transfer/transfer/transfer_types.hpp"

#include <catch2/catch_test_macros.hpp>

using aki::transfer::FileMetadata;
using aki::transfer::Transfer;
using aki::transfer::TransferId;
using aki::transfer::TransferState;
using aki::transfer::TransferStateMachine;
using aki::transfer::can_transition;
using aki::transfer::is_terminal;
using aki::transfer::to_string;

TEST_CASE("TransferState follows the happy path to completion", "[unit][transfer]") {
    TransferStateMachine machine;
    REQUIRE(machine.state == TransferState::Queued);
    REQUIRE(machine.transition_to(TransferState::Negotiating));
    REQUIRE(machine.transition_to(TransferState::Transferring));
    REQUIRE(machine.transition_to(TransferState::Completed));
    REQUIRE(machine.state == TransferState::Completed);
}

TEST_CASE("TransferState supports pause and resume", "[unit][transfer]") {
    TransferStateMachine machine;
    machine.state = TransferState::Transferring;
    REQUIRE(machine.transition_to(TransferState::Paused));
    REQUIRE(machine.transition_to(TransferState::Transferring));
    REQUIRE(machine.state == TransferState::Transferring);
}

// M4-06（DEC-013）：重启降级边——Queued/Negotiating -> Paused 合法
//（恢复段把非终态行改写 Paused，运行期语义一致）。
TEST_CASE("TransferState allows restart demotion into Paused", "[unit][transfer]") {
    for (const TransferState from :
        {TransferState::Queued, TransferState::Negotiating}) {
        INFO(to_string(from) << " -> Paused (restart demotion)");
        TransferStateMachine machine;
        machine.state = from;
        REQUIRE(can_transition(from, TransferState::Paused));
        REQUIRE(machine.transition_to(TransferState::Paused));
        REQUIRE(machine.state == TransferState::Paused);
        // 降级后恢复仍走既有合法边。
        REQUIRE(machine.transition_to(TransferState::Transferring));
        REQUIRE(machine.state == TransferState::Transferring);
    }
}

TEST_CASE("TransferState can be cancelled from every active state", "[unit][transfer]") {
    for (const TransferState from : {TransferState::Queued, TransferState::Negotiating,
             TransferState::Transferring, TransferState::Paused}) {
        INFO(to_string(from) << " -> Cancelled");
        TransferStateMachine machine;
        machine.state = from;
        REQUIRE(machine.transition_to(TransferState::Cancelled));
        REQUIRE(machine.state == TransferState::Cancelled);
    }
}

TEST_CASE("TransferState can fail from negotiating, transferring and paused",
          "[unit][transfer]") {
    for (const TransferState from : {TransferState::Negotiating, TransferState::Transferring,
             TransferState::Paused}) {
        INFO(to_string(from) << " -> Failed");
        TransferStateMachine machine;
        machine.state = from;
        REQUIRE(machine.transition_to(TransferState::Failed));
        REQUIRE(machine.state == TransferState::Failed);
    }
}

TEST_CASE("TransferState rejects illegal transitions", "[unit][transfer]") {
    struct Case {
        TransferState from;
        TransferState to;
    };
    const Case illegal[] = {
        {TransferState::Queued, TransferState::Transferring},
        {TransferState::Queued, TransferState::Completed},
        {TransferState::Queued, TransferState::Failed},
        {TransferState::Negotiating, TransferState::Completed},
        {TransferState::Transferring, TransferState::Negotiating},
        {TransferState::Paused, TransferState::Completed},
        {TransferState::Paused, TransferState::Negotiating},
    };

    for (const Case& c : illegal) {
        INFO(to_string(c.from) << " -> " << to_string(c.to));
        TransferStateMachine machine;
        machine.state = c.from;
        REQUIRE_FALSE(can_transition(c.from, c.to));
        REQUIRE_FALSE(machine.transition_to(c.to));
        REQUIRE(machine.state == c.from);
    }
}

TEST_CASE("TransferState terminals are idempotent and final", "[unit][transfer]") {
    REQUIRE(is_terminal(TransferState::Completed));
    REQUIRE(is_terminal(TransferState::Failed));
    REQUIRE(is_terminal(TransferState::Cancelled));
    REQUIRE_FALSE(is_terminal(TransferState::Paused));

    for (const TransferState terminal :
         {TransferState::Completed, TransferState::Failed, TransferState::Cancelled}) {
        INFO(to_string(terminal));
        TransferStateMachine machine;
        machine.state = terminal;
        REQUIRE(machine.transition_to(terminal));
        for (const TransferState other :
             {TransferState::Transferring, TransferState::Completed, TransferState::Failed,
                 TransferState::Cancelled}) {
            if (other == terminal) {
                continue;
            }
            INFO("  -> " << to_string(other));
            REQUIRE_FALSE(machine.transition_to(other));
        }
        REQUIRE(machine.state == terminal);
    }

    SECTION("late progress must not revive a cancelled transfer") {
        TransferStateMachine machine;
        machine.state = TransferState::Cancelled;
        REQUIRE_FALSE(machine.transition_to(TransferState::Transferring));
        REQUIRE(machine.state == TransferState::Cancelled);
    }
}

TEST_CASE("Transfer binds file metadata and devices without file data", "[unit][transfer]") {
    const Transfer transfer{TransferId{"t-1"}, aki::device::DeviceId{"a"},
        aki::device::DeviceId{"b"}, FileMetadata{"policy.pt", 7340032,
            "application/octet-stream", ""},
        0, 7340032, TransferState::Queued};

    // RULE-05：消息里只有 metadata 与 TransferId，没有文件内容字段。
    REQUIRE(transfer.file.name == "policy.pt");
    REQUIRE(transfer.total == 7340032);
    REQUIRE(transfer.state == TransferState::Queued);
}
