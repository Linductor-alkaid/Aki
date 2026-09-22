// M1-02：Application State 单写者边界与设计第 10 节事件模型单测。
//
// 覆盖（验收标准 ①②④）：
//   - DoubleBuffer：单写者 drain 后快照一致可见、sequence 去重、批量发布；
//   - LatestMailbox：连接路径 latest-wins、try_load_newer_than 去重、覆盖计数；
//   - Topic：订阅广播 FIFO 顺序、慢订阅者 RejectNewest 拒绝可见、无重放；
//   - DOD-02 六项（以 comm 关闭/排空为主）：正常完成、任务异常、提交拒绝、
//     执行中取消（close 解除阻塞等待）、超时、shutdown（关闭顺序与排空）；
//   - 迟到事件不得复活已取消任务（RULE-08，与 M1 退出-3 呼应）；
//   - 并发生产者经 MpscChannel 汇聚到单写者不丢更新（SWMR 纪律）。
//
// 本测试进程唯一的 Executor owner 是文件末尾的 main()（AGENTS.md 规则 7；
// 正式 owner 由 M1-04 的 app/lifecycle 提供）。并发任务一律经 pinned executor
// 的公开能力（submit_auto）承载，不使用 std::thread / std::async；Catch2 断言
// 只在 main 线程执行，worker 任务只返回值。
#include "app/state/app_events.hpp"
#include "app/state/app_state.hpp"
#include "app/state/app_state_owner.hpp"
#include "app/state/app_state_updates.hpp"

#include <executor/comm/types.hpp>
#include <executor/executor.hpp>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

executor::Executor* g_test_executor = nullptr;

}  // namespace

namespace aki::test {

// 本进程唯一 Executor owner 是 main()（见文件末尾）；用例经此获取执行上下文。
// AGENTS.md 规则 7/8：owner 显式、传递显式，不隐藏生命周期。
[[nodiscard]] executor::Executor& executor() {
    return *g_test_executor;
}

void use_executor(executor::Executor& executor) {
    g_test_executor = &executor;
}

}  // namespace aki::test

namespace {

using namespace std::chrono_literals;

// 失败计数相对 future 结算是异步记账（executor 包装器先满足 future 再记失败）：
// 断言计数前以有界轮询等待可见，与 test_executor_lifecycle 的处理一致。
bool wait_until(const std::function<bool()>& predicate, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

using aki::app::AppEvent;
using aki::app::AppState;
using aki::app::AppStateOwner;
using aki::app::AppStateOwnerOptions;
using aki::app::AppStateOwnerStats;
using aki::app::AppStateUpdate;
using aki::app::DeviceConnectedEvent;
using aki::app::MessageReceivedEvent;
using aki::app::SetConnectionPath;
using aki::app::TransferProgressEvent;
using aki::app::UpsertConversation;
using aki::app::UpsertDevice;
using aki::app::UpsertMessage;
using aki::app::UpsertTransfer;
using aki::app::UpdateTransferProgress;

using aki::conversation::Conversation;
using aki::conversation::ConversationId;
using aki::conversation::ConversationState;
using aki::conversation::DeliveryState;
using aki::conversation::Message;
using aki::conversation::MessageId;
using aki::device::ConnectionPath;
using aki::device::DeviceId;
using aki::device::DeviceIdentity;
using aki::device::TrustState;
using aki::transfer::Transfer;
using aki::transfer::TransferId;
using aki::transfer::TransferState;

std::uint64_t forward_watermark(const AppStateOwnerStats& stats) {
    return stats.updates_applied + stats.updates_rejected + stats.events_forwarded
        + stats.events_dropped;
}

// 排空 owner 直至水位不再前进（owner 上下文 = 当前测试线程）。
void drain_until_idle(AppStateOwner& owner) {
    for (;;) {
        const auto before = forward_watermark(owner.stats());
        owner.drain(256, 256);
        if (forward_watermark(owner.stats()) == before) {
            return;
        }
    }
}

DeviceIdentity make_device(std::string id, TrustState trust = TrustState::Unknown) {
    DeviceIdentity device;
    device.id = DeviceId{std::move(id)};
    device.display_name = device.id.value;
    device.trust_state = trust;
    device.presence = aki::device::PresenceState::Online;
    return device;
}

Message make_message(std::string id, DeliveryState state) {
    Message message;
    message.id = MessageId{std::move(id)};
    message.sender = DeviceId{"a"};
    message.receiver = DeviceId{"b"};
    message.type = aki::conversation::MessageType::Text;
    message.state = state;
    return message;
}

Conversation make_conversation(std::string id, ConversationState state) {
    Conversation conversation;
    conversation.id = ConversationId{std::move(id)};
    conversation.local_device = DeviceId{"a"};
    conversation.remote_device = DeviceId{"b"};
    conversation.state = state;
    return conversation;
}

Transfer make_transfer(std::string id, TransferState state, std::uint64_t transferred = 0) {
    Transfer transfer;
    transfer.id = TransferId{std::move(id)};
    transfer.sender = DeviceId{"a"};
    transfer.receiver = DeviceId{"b"};
    transfer.file.name = "policy.pt";
    transfer.transferred = transferred;
    transfer.total = 10;
    transfer.state = state;
    return transfer;
}

AppEvent make_text_event(std::uint64_t marker) {
    AppEvent event;
    event.payload = MessageReceivedEvent{make_message("evt-" + std::to_string(marker),
        DeliveryState::Sent)};
    return event;
}

}  // namespace

TEST_CASE("AppState fixes the four design stores and nine event kinds",
    "[unit][app_state]") {
    STATIC_REQUIRE(std::variant_size_v<aki::app::AppEventPayload> == 9);
    STATIC_REQUIRE(std::is_default_constructible_v<AppState>);

    const AppState initial;
    REQUIRE(initial.devices.devices.empty());
    REQUIRE(initial.conversations.conversations.empty());
    REQUIRE(initial.messages.messages.empty());
    REQUIRE(initial.transfers.transfers.empty());
}

TEST_CASE("Single-writer drain publishes one consistent snapshot per batch",
    "[unit][app_state][double_buffer]") {
    AppStateOwner owner;

    // 未 drain 前，admission 不等于已生效：快照仍是初始状态。
    REQUIRE(owner.submit_update(UpsertDevice{make_device("dev-1")}));
    REQUIRE(owner.submit_update(UpsertDevice{make_device("dev-2", TrustState::Pending)}));
    REQUIRE(owner.submit_update(UpsertMessage{make_message("msg-1", DeliveryState::Queued)}));
    const std::uint64_t before = owner.snapshot_sequence();

    owner.drain();

    executor::comm::Snapshot<AppState> snapshot;
    REQUIRE(owner.try_load_snapshot(snapshot));
    REQUIRE(snapshot.sequence > before);
    REQUIRE(snapshot.value.devices.devices.size() == 2);
    REQUIRE(snapshot.value.messages.messages.size() == 1);
    REQUIRE(owner.stats().snapshots_published == 1);  // 每批次只发布一次
    REQUIRE(owner.stats().updates_applied == 3);
    REQUIRE(owner.stats().updates_rejected == 0);

    // sequence 去重：load_newer_than 对旧序列取到新快照，对最新序列返回 false。
    executor::comm::Snapshot<AppState> newer;
    REQUIRE(owner.load_snapshot_newer_than(before, newer));
    REQUIRE(newer.sequence == snapshot.sequence);
    REQUIRE_FALSE(owner.load_snapshot_newer_than(snapshot.sequence, newer));
}

TEST_CASE("LatestMailbox carries only the newest connection path",
    "[unit][app_state][latest_mailbox]") {
    AppStateOwner owner;

    REQUIRE(owner.submit_update(SetConnectionPath{ConnectionPath::Lan}));
    REQUIRE(owner.submit_update(SetConnectionPath{ConnectionPath::P2p}));
    REQUIRE(owner.submit_update(SetConnectionPath{ConnectionPath::Relay}));
    owner.drain();

    ConnectionPath path = ConnectionPath::Unknown;
    REQUIRE(owner.try_load_connection_path(path));
    REQUIRE(path == ConnectionPath::Relay);

    // 最新值语义：对最新序列没有更新可取（stale read 显式返回 false）。
    const std::uint64_t seq = owner.connection_path_sequence();
    std::uint64_t new_sequence = 0;
    REQUIRE_FALSE(owner.try_load_connection_path_newer_than(seq, path, new_sequence));

    // 覆盖计数可观测：3 次发布，前 2 次被覆盖。
    REQUIRE(owner.connection_path_stats().overwritten_count == 2);

    // 单值摘要不触发 Store 快照发布（设计第 10.1 节：不在 AppState 内）。
    REQUIRE(owner.snapshot_sequence() == 0);
    REQUIRE(owner.stats().snapshots_published == 0);
}

TEST_CASE("Topic fans out events in order and makes rejection visible",
    "[unit][app_state][topic]") {
    AppStateOwner owner;

    auto observer_a = owner.subscribe_observer(
        executor::comm::TopicSubscriptionOptions{.capacity = 8, .name = "obs-a"});
    auto observer_b = owner.subscribe_observer(
        executor::comm::TopicSubscriptionOptions{.capacity = 8, .name = "obs-b"});

    for (std::uint64_t marker = 1; marker <= 3; ++marker) {
        REQUIRE(owner.post_event(make_text_event(marker)));
    }
    owner.drain();

    // 观察者收到同样的 3 条，FIFO 顺序一致，序列号严格递增。
    for (auto* observer : {&observer_a, &observer_b}) {
        for (std::uint64_t marker = 1; marker <= 3; ++marker) {
            aki::app::AppEventPtr event;
            REQUIRE(observer->try_receive(event));
            REQUIRE(event->sequence == marker);
            REQUIRE(std::holds_alternative<MessageReceivedEvent>(event->payload));
        }
        aki::app::AppEventPtr extra;
        REQUIRE_FALSE(observer->try_receive(extra));
    }

    // 必达主路径同样按序 1..3。
    for (std::uint64_t marker = 1; marker <= 3; ++marker) {
        AppEvent event;
        REQUIRE(owner.try_receive_event(event));
        REQUIRE(event.sequence == marker);
    }

    REQUIRE(owner.stats().events_forwarded == 3);
    REQUIRE(owner.stats().observer_rejects == 0);

    // 慢订阅者（capacity=1, RejectNewest）：自己的队列被拒，不影响他人，
    // 拒绝计入 owner 统计（RULE-09 / EXEC-06）。
    auto observer_c = owner.subscribe_observer(
        executor::comm::TopicSubscriptionOptions{.capacity = 1, .name = "obs-c"});
    REQUIRE(owner.post_event(make_text_event(4)));
    REQUIRE(owner.post_event(make_text_event(5)));
    owner.drain();

    aki::app::AppEventPtr event;
    REQUIRE(observer_c.try_receive(event));   // 收到第 4 条
    REQUIRE_FALSE(observer_c.try_receive(event));  // 第 5 条被 RejectNewest 丢弃
    REQUIRE(observer_a.try_receive(event));   // 观察者 a 两条都收到
    REQUIRE(observer_a.try_receive(event));
    REQUIRE(owner.stats().observer_rejects >= 1);

    // 无重放：新订阅者只收创建之后的发布。
    auto observer_d = owner.subscribe_observer(
        executor::comm::TopicSubscriptionOptions{.capacity = 8, .name = "obs-d"});
    aki::app::AppEventPtr replay;
    REQUIRE_FALSE(observer_d.try_receive(replay));
}

TEST_CASE("DOD-02 normal completion: update flows through executor tasks to snapshot",
    "[unit][app_state][dod02]") {
    AppStateOwner owner;
    auto& executor = aki::test::executor();

    auto admitted = executor.submit_auto(
        [&owner] { return owner.submit_update(UpsertDevice{make_device("dev-ok")}); });
    REQUIRE(admitted.get());

    auto applied = executor.submit_auto([&owner] {
        owner.drain();
        return owner.stats().updates_applied;
    });
    REQUIRE(applied.get() == 1);

    executor::comm::Snapshot<AppState> snapshot;
    REQUIRE(owner.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.devices.devices.size() == 1);
    REQUIRE(snapshot.value.devices.devices.front().id == DeviceId{"dev-ok"});
}

TEST_CASE("DOD-02 task exception: visible to caller, admitted work survives",
    "[unit][app_state][dod02]") {
    AppStateOwner owner;
    auto& executor = aki::test::executor();

    const auto before = executor.get_failure_status().task_exception_count;
    auto failing = executor.submit_auto([&owner] {
        if (owner.submit_update(UpsertDevice{make_device("dev-crash")})) {
            throw std::runtime_error("boom");
        }
        return false;  // admission 失败时走这里，令 REQUIRE_THROWS_AS 失败而暴露。
    });
    REQUIRE_THROWS_AS(failing.get(), std::runtime_error);
    REQUIRE(wait_until([&] {
        return executor.get_failure_status().task_exception_count >= before + 1;
    }, 2s));

    // admission 与执行分离：已入队的更新不因任务崩溃丢失，异常也不被吞掉。
    owner.drain();
    executor::comm::Snapshot<AppState> snapshot;
    REQUIRE(owner.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.devices.devices.size() == 1);
}

TEST_CASE("DOD-02 submission rejection: full and closed channels are explicit",
    "[unit][app_state][dod02]") {
    AppStateOwner owner{AppStateOwnerOptions{.update_capacity = 1,
        .event_inbox_capacity = 1}};
    REQUIRE_FALSE(owner.is_closed());

    REQUIRE(owner.submit_update(UpsertDevice{make_device("dev-1")}));
    REQUIRE_FALSE(owner.submit_update(UpsertDevice{make_device("dev-2")}));
    REQUIRE(owner.update_channel_stats().dropped_count == 1);

    REQUIRE(owner.post_event(make_text_event(1)));
    REQUIRE_FALSE(owner.post_event(make_text_event(2)));
    REQUIRE(owner.event_inbox_stats().dropped_count == 1);

    owner.drain();
    REQUIRE(owner.stats().updates_applied == 1);
    REQUIRE(owner.stats().updates_rejected == 0);

    owner.close();
    REQUIRE(owner.is_closed());
    REQUIRE_FALSE(owner.submit_update(UpsertDevice{make_device("dev-3")}));
    REQUIRE_FALSE(owner.post_event(make_text_event(3)));
    REQUIRE(owner.update_channel_stats().closed_send_count == 1);
    REQUIRE(owner.event_inbox_stats().closed_send_count == 1);
}

TEST_CASE("DOD-02 in-flight cancellation: close releases a waiting consumer",
    "[unit][app_state][dod02]") {
    AppStateOwner owner;
    auto& executor = aki::test::executor();

    std::atomic<bool> waiting{false};
    auto consumer = executor.submit_auto([&owner, &waiting] {
        AppEvent event;
        waiting.store(true);
        return owner.receive_event_for(event, std::chrono::seconds(30));
    });
    while (!waiting.load()) {
        std::this_thread::yield();
    }

    owner.close();  // lifecycle 顺序：关闭通道解除阻塞等待（可取消路径）。

    const auto result = consumer.get();
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error_code == executor::comm::CommErrorCode::Closed);
}

TEST_CASE("DOD-02 timeout: empty outbox and full inbox return explicit Timeout",
    "[unit][app_state][dod02]") {
    AppStateOwner owner{AppStateOwnerOptions{.update_capacity = 1}};

    AppEvent event;
    const auto receive = owner.receive_event_for(event, 20ms);
    REQUIRE_FALSE(receive.ok);
    REQUIRE(receive.error_code == executor::comm::CommErrorCode::Timeout);
    REQUIRE(owner.event_outbox_stats().timeout_count == 1);

    // 满队列上的有界等待发送同样超时可见。
    REQUIRE(owner.submit_update(UpsertDevice{make_device("dev-1")}));
    const auto send = owner.submit_update_for(UpsertDevice{make_device("dev-2")}, 20ms);
    REQUIRE_FALSE(send.ok);
    REQUIRE(send.error_code == executor::comm::CommErrorCode::Timeout);
    REQUIRE(owner.update_channel_stats().timeout_count == 1);
}

TEST_CASE("DOD-02 shutdown: close drains backlog, rejects new work, state readable",
    "[unit][app_state][dod02]") {
    AppStateOwner owner;

    REQUIRE(owner.submit_update(UpsertDevice{make_device("dev-1")}));
    REQUIRE(owner.submit_update(UpsertDevice{make_device("dev-2", TrustState::Trusted)}));
    REQUIRE(owner.post_event(make_text_event(1)));
    REQUIRE(owner.post_event(make_text_event(2)));

    owner.close();

    // close 前已入队的更新与事件不丢失（comm 排空语义）。
    REQUIRE(owner.stats().updates_applied == 2);
    REQUIRE(owner.stats().events_forwarded == 2);

    // 新工作被明确拒绝。
    REQUIRE_FALSE(owner.submit_update(UpsertDevice{make_device("dev-3")}));
    REQUIRE_FALSE(owner.post_event(make_text_event(3)));
    REQUIRE(owner.update_channel_stats().closed_send_count >= 1);
    drain_until_idle(owner);  // 关闭后 drain 为无害 no-op。
    REQUIRE(owner.stats().updates_applied == 2);

    // 末次快照仍可读。
    executor::comm::Snapshot<AppState> snapshot;
    REQUIRE(owner.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.devices.devices.size() == 2);
    REQUIRE(snapshot.value.devices.devices.back().trust_state == TrustState::Trusted);

    // 必达主路径：先排空存量，再返回 Closed。
    AppEvent event;
    REQUIRE(owner.try_receive_event(event));
    REQUIRE(event.sequence == 1);
    REQUIRE(owner.try_receive_event(event));
    REQUIRE(event.sequence == 2);
    const auto after_drain = owner.receive_event_for(event, 20ms);
    REQUIRE_FALSE(after_drain.ok);
    REQUIRE(after_drain.error_code == executor::comm::CommErrorCode::Closed);

    // 关闭后的订阅返回已关闭句柄；重复 close 幂等。
    auto late_observer = owner.subscribe_observer();
    REQUIRE(late_observer.is_closed());
    owner.close();
    REQUIRE(owner.is_closed());
}

TEST_CASE("Late events cannot revive terminal entities (RULE-08)",
    "[unit][app_state][late-events]") {
    // 契约：入队（admission）总是成功到有界通道；“拒绝”发生在状态 owner 应用更新时，
    // 以 updates_rejected 计数暴露，不得复活终态（admission ≠ 已生效）。
    AppStateOwner owner;

    SECTION("late progress cannot revive a cancelled transfer") {
        REQUIRE(owner.submit_update(UpsertTransfer{make_transfer("t-1",
            TransferState::Transferring, 1)}));
        REQUIRE(owner.submit_update(UpsertTransfer{make_transfer("t-1",
            TransferState::Cancelled, 1)}));

        // 迟到的进度与迟到的不合法转移：入队成功，应用时被拒绝。
        REQUIRE(owner.submit_update(UpdateTransferProgress{TransferId{"t-1"}, 9, 10}));
        REQUIRE(owner.submit_update(UpsertTransfer{make_transfer("t-1",
            TransferState::Transferring, 9)}));
        owner.drain();

        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        const auto& transfers = snapshot.value.transfers.transfers;
        REQUIRE(transfers.size() == 1);
        REQUIRE(transfers.front().state == TransferState::Cancelled);
        REQUIRE(transfers.front().transferred == 1);
        REQUIRE(owner.stats().updates_applied == 2);
        REQUIRE(owner.stats().updates_rejected == 2);
    }

    SECTION("late delivery cannot revive a failed message") {
        REQUIRE(owner.submit_update(UpsertMessage{make_message("m-1", DeliveryState::Sent)}));
        REQUIRE(owner.submit_update(UpsertMessage{make_message("m-1", DeliveryState::Failed)}));
        // 终态重复宣告：幂等 no-op，合法。
        REQUIRE(owner.submit_update(UpsertMessage{make_message("m-1", DeliveryState::Failed)}));
        REQUIRE(owner.submit_update(UpsertMessage{make_message("m-1", DeliveryState::Queued)}));
        owner.drain();

        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.messages.messages.front().state == DeliveryState::Failed);
        REQUIRE(owner.stats().updates_applied == 3);
        REQUIRE(owner.stats().updates_rejected == 1);
    }

    SECTION("late trust change cannot revive a revoked device") {
        REQUIRE(owner.submit_update(UpsertDevice{make_device("d-1", TrustState::Trusted)}));
        REQUIRE(owner.submit_update(UpsertDevice{make_device("d-1", TrustState::Revoked)}));
        REQUIRE(owner.submit_update(UpsertDevice{make_device("d-1", TrustState::Trusted)}));
        owner.drain();

        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.devices.devices.front().trust_state == TrustState::Revoked);
        REQUIRE(owner.stats().updates_rejected == 1);
    }

    SECTION("archived conversation rejects reactivation") {
        REQUIRE(owner.submit_update(UpsertConversation{make_conversation("c-1",
            ConversationState::Active)}));
        REQUIRE(owner.submit_update(UpsertConversation{make_conversation("c-1",
            ConversationState::Archived)}));
        REQUIRE(owner.submit_update(UpsertConversation{make_conversation("c-1",
            ConversationState::Active)}));
        owner.drain();

        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.conversations.conversations.front().state
            == ConversationState::Archived);
        REQUIRE(owner.stats().updates_rejected == 1);
    }
}

TEST_CASE("Store capacity limits reject overflow explicitly (RULE-09)",
    "[unit][app_state][limits]") {
    AppStateOwner owner{AppStateOwnerOptions{.limits = {.max_devices = 1}}};

    REQUIRE(owner.submit_update(UpsertDevice{make_device("dev-1")}));
    REQUIRE(owner.submit_update(UpsertDevice{make_device("dev-2")}));
    owner.drain();

    REQUIRE(owner.stats().updates_applied == 1);
    REQUIRE(owner.stats().updates_rejected == 1);
    executor::comm::Snapshot<AppState> snapshot;
    REQUIRE(owner.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.devices.devices.size() == 1);
}

TEST_CASE("Concurrent producers converge through the single writer without loss",
    "[unit][app_state][swmr][dod02]") {
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kUpdatesPerProducer = 40;

    AppStateOwner owner;
    auto& executor = aki::test::executor();

    struct ReaderResult {
        std::uint64_t loads = 0;
        std::uint64_t last_sequence = 0;
        std::size_t last_size = 0;
        bool sequence_monotonic = true;
        bool size_monotonic = true;
        bool size_bounded = true;
    };
    std::atomic<bool> stop_readers{false};
    std::atomic<bool> reader_started{false};

    // 先等 reader 确认开跑再放行生产者：快照 store 构造即播种发布
    // （SnapshotStore(T initial)），reader 首次自旋即可读到；若不同步启动，
    // ASAN 等重负载下任务启动延迟可能让 160 个微小生产者任务在 reader 首次
    // 被调度前全部完成、stop_readers 已置位，loads==0 误报（CI 实测）。
    auto reader = executor.submit_auto([&owner, &stop_readers, &reader_started] {
        reader_started.store(true, std::memory_order_release);
        ReaderResult result;
        executor::comm::Snapshot<AppState> snapshot;
        while (!stop_readers.load() && result.loads < 100000) {
            if (owner.try_load_snapshot(snapshot)) {
                if (snapshot.sequence < result.last_sequence) {
                    result.sequence_monotonic = false;
                }
                result.last_sequence = snapshot.sequence;
                const std::size_t device_count = snapshot.value.devices.devices.size();
                if (device_count < result.last_size) {
                    result.size_monotonic = false;
                }
                result.last_size = device_count;
                if (device_count > kProducers * kUpdatesPerProducer) {
                    result.size_bounded = false;
                }
                ++result.loads;
            }
            std::this_thread::yield();
        }
        return result;
    });

    REQUIRE(wait_until(
        [&] { return reader_started.load(std::memory_order_acquire); }, 2s));

    std::vector<std::future<bool>> producers;
    for (std::size_t p = 0; p < kProducers; ++p) {
        producers.push_back(executor.submit_auto([&owner, p] {
            bool all_admitted = true;
            for (std::size_t i = 0; i < kUpdatesPerProducer; ++i) {
                all_admitted = owner.submit_update(UpsertDevice{make_device(
                    "dev-" + std::to_string(p) + "-" + std::to_string(i))})
                    && all_admitted;
            }
            return all_admitted;
        }));
    }

    for (auto& producer : producers) {
        REQUIRE(producer.get());
    }
    stop_readers.store(true);

    const ReaderResult reads = reader.get();
    REQUIRE(reads.loads > 0);
    REQUIRE(reads.sequence_monotonic);
    REQUIRE(reads.size_monotonic);
    REQUIRE(reads.size_bounded);

    // 单写者排空：无丢失、无拒绝，最终快照完整可见。
    drain_until_idle(owner);
    REQUIRE(owner.stats().updates_applied == kProducers * kUpdatesPerProducer);
    REQUIRE(owner.stats().updates_rejected == 0);

    executor::comm::Snapshot<AppState> snapshot;
    REQUIRE(owner.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.devices.devices.size() == kProducers * kUpdatesPerProducer);
}

int main(int argc, char* argv[]) {
    // 本进程唯一 Executor owner（AGENTS.md 规则 7）：main 显式初始化，
    // 全部用例结束后由非 worker 线程执行 shutdown(true)。
    // owner 纪律落点说明（设计第 8.2 节）：此处的裸 Executor 实例是 M1-02 落地时
    // 的测试形态——正式 owner 为 app/lifecycle 的 ExecutorOwner（M1-04 已交付，
    // 见 test_executor_lifecycle）；自 M1-06 冒烟宿主起进程内改用 ExecutorOwner。
    executor::Executor executor;
    const auto initialized = executor.initialize_ex({});
    if (!initialized.ok) {
        std::fputs("executor initialize_ex failed\n", stderr);
        return 1;
    }
    aki::test::use_executor(executor);

    const int result = Catch::Session().run(argc, argv);

    executor.shutdown(true);
    return result;
}
