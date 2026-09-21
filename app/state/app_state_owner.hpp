// Application State 单写者 owner（设计第 10/10.1 节，EXEC-03，DEC-002）。
//
// 跨上下文交付到 pinned executor `executor::comm` 组件的映射：
//   - Manager -> owner 更新汇聚：MpscChannel<AppStateUpdate>（有界，满即拒绝）
//   - Manager -> owner 事件入口：MpscChannel<AppEvent>（有界，满即拒绝）
//   - owner -> 渲染侧一致快照：DoubleBuffer<AppState>（SWMR，单写者硬契约）
//   - 单值最新状态（连接路径摘要）：LatestMailbox<ConnectionPath>（覆盖式）
//   - owner -> 观察者（诊断/日志/Agent）：Topic<AppEventPtr>（best-effort 广播）
//   - owner -> 应用事件消费主路径：MpscChannel<AppEvent>（9 类必达事件，FIFO）
//
// 线程契约（设计第 10.1 节硬约束 1，由调用方纪律保证，不加自建锁）：
//   - submit_update()/post_event()：任意上下文（Manager 侧）。
//   - drain()/close()/stats()：仅状态 owner 的单一执行上下文。
//   - try_load_snapshot()/load_snapshot_newer_than()/try_receive_event()/
//     receive_event_for()/subscribe_observer()/连接路径读取：任意上下文。
//   - close() 由外部 lifecycle owner 在停止生产者之后调用（EXEC-01；
//     正式 owner 由 M1-04 提供）。comm 组件不参与 Executor shutdown。
#pragma once

#include "app/state/app_events.hpp"
#include "app/state/app_state.hpp"
#include "app/state/app_state_updates.hpp"

#include <executor/comm/channel.hpp>
#include <executor/comm/double_buffer.hpp>
#include <executor/comm/mailbox.hpp>
#include <executor/comm/topic.hpp>
#include <executor/comm/types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace aki::app {

// owner 侧计数（RULE-09 / EXEC-06：拒绝与丢弃必须可观测，不得静默）。
// 普通（非原子）计数遵循单写者契约：仅 owner 上下文累加与读取。
struct AppStateOwnerStats {
    std::uint64_t updates_applied = 0;
    std::uint64_t updates_rejected = 0;  // 状态机拒绝 / 终态复活 / 容量超限
    std::uint64_t events_forwarded = 0;  // 进入必达主路径并扇出观察者
    std::uint64_t events_dropped = 0;    // 有界移交失败（预期为 0，出现即缺陷信号）
    std::uint64_t observer_rejects = 0;  // Topic 慢订阅者被拒（RejectNewest）
    std::uint64_t snapshots_published = 0;
};

// 构造选项置于命名空间作用域：类内默认实参引用嵌套类型的 NSDMI 在 GCC 下非法
// （“required before the end of its enclosing class”）。
struct AppStateOwnerOptions {
    std::string name = "aki.app_state";
    AppStateLimits limits{};
    std::size_t update_capacity = 1024;
    std::size_t event_inbox_capacity = 1024;
    std::size_t event_outbox_capacity = 1024;
};

class AppStateOwner {
public:
    using Options = AppStateOwnerOptions;

    static constexpr std::size_t kDefaultDrainUpdates = 64;
    static constexpr std::size_t kDefaultDrainEvents = 128;

    explicit AppStateOwner(AppStateOwnerOptions options = {}, AppState initial = AppState{})
        : options_(std::move(options)),
          limits_(options_.limits),
          snapshot_(std::move(initial), options_.name + ".snapshot"),
          current_(snapshot_.load().value),
          updates_(executor::comm::ChannelOptions{.capacity = options_.update_capacity,
              .enable_stats = true,
              .name = options_.name + ".updates"}),
          event_inbox_(executor::comm::ChannelOptions{.capacity = options_.event_inbox_capacity,
              .enable_stats = true,
              .name = options_.name + ".event_inbox"}),
          event_outbox_(executor::comm::ChannelOptions{.capacity = options_.event_outbox_capacity,
              .enable_stats = true,
              .name = options_.name + ".event_outbox"}),
          connection_path_(options_.name + ".connection_path"),
          observers_(options_.name + ".observers") {}

    AppStateOwner(const AppStateOwner&) = delete;
    AppStateOwner& operator=(const AppStateOwner&) = delete;

    // ---- Manager 侧（任意上下文）：入队即 admission，不等于已生效 ----
    [[nodiscard]] bool submit_update(AppStateUpdate update) {
        return updates_.try_send(std::move(update));
    }

    // 满载时的有界等待投递适配（非实时；超时/关闭均为明确结果，不静默重试）。
    template <class Rep, class Period>
    [[nodiscard]] executor::comm::CommResult submit_update_for(
        AppStateUpdate update, std::chrono::duration<Rep, Period> timeout) {
        return updates_.send_for(std::move(update), timeout);
    }

    [[nodiscard]] bool post_event(AppEvent event) {
        return event_inbox_.try_send(std::move(event));
    }

    // ---- 状态 owner 上下文（单写者）----

    // 有界工作单元（AGENTS.md“每个推进步骤是有界工作单元”）：至多处理
    // max_updates 条更新与 max_events 条事件；Store 有变更时发布一次快照。
    void drain(std::size_t max_updates = kDefaultDrainUpdates,
        std::size_t max_events = kDefaultDrainEvents) {
        drain_updates(max_updates);
        forward_events(max_events);
        publish_if_dirty();
    }

    // 关闭顺序（设计第 10.1 节硬约束 3）：关闭两个入口通道 -> 排空存量
    // （close 前已入队的更新与事件不丢失，comm 关闭语义）-> 发布末次快照 ->
    // 关闭观察者 Topic -> 关闭必达主路径。之后 submit/post 返回 false，
    // 快照与主路径存量仍可读，主路径排空后 receive 返回 Closed。
    // 若 outbox 已满且消费侧不再排空，inbox 存量无法移交：循环按“水位无进展”终止，
    // 积压经 event_inbox_stats() 可观测（RULE-09 背压可见，不静默丢弃）。
    void close() {
        if (closed_) {
            return;
        }
        closed_ = true;
        updates_.close();
        event_inbox_.close();
        for (;;) {
            const std::uint64_t before = progress_watermark();
            drain();
            if (progress_watermark() == before) {
                break;
            }
        }
        observers_.close();
        event_outbox_.close();
    }

    [[nodiscard]] bool is_closed() const noexcept { return closed_; }

    // ---- 渲染侧 / 消费侧（任意上下文）----

    // DoubleBuffer 一致快照：try_load 失败（槽位忙）可重试；消费侧应保存
    // sequence 并用 load_snapshot_newer_than 去重，避免重复消费同一快照。
    [[nodiscard]] bool try_load_snapshot(executor::comm::Snapshot<AppState>& out) const {
        return snapshot_.try_load(out);
    }

    [[nodiscard]] bool load_snapshot_newer_than(
        std::uint64_t last_seen_sequence, executor::comm::Snapshot<AppState>& out) const {
        return snapshot_.load_newer_than(last_seen_sequence, out);
    }

    [[nodiscard]] std::uint64_t snapshot_sequence() const noexcept {
        return snapshot_.sequence();
    }

    // 9 类必达事件主路径：FIFO；receive_event_for 为可解除阻塞的等待适配
    // （超时返回 Timeout；close 后先排空存量再返回 Closed）。
    [[nodiscard]] bool try_receive_event(AppEvent& out) {
        return event_outbox_.try_receive(out);
    }

    template <class Rep, class Period>
    [[nodiscard]] executor::comm::CommResult receive_event_for(
        AppEvent& out, std::chrono::duration<Rep, Period> timeout) {
        return event_outbox_.receive_for(out, timeout);
    }

    // 覆盖式单值状态摘要（LatestMailbox latest-wins）。
    [[nodiscard]] bool try_load_connection_path(aki::device::ConnectionPath& out) const {
        return connection_path_.try_load(out);
    }

    [[nodiscard]] bool try_load_connection_path_newer_than(std::uint64_t last_seen_sequence,
        aki::device::ConnectionPath& out, std::uint64_t& new_sequence) const {
        return connection_path_.try_load_newer_than(last_seen_sequence, out, new_sequence);
    }

    [[nodiscard]] std::uint64_t connection_path_sequence() const noexcept {
        return connection_path_.sequence();
    }

    // best-effort 观察者订阅：只收到订阅创建之后的发布，无重放；
    // 慢订阅者按自身 DropPolicy 被拒，不影响其他订阅者。
    [[nodiscard]] executor::comm::TopicSubscription<AppEventPtr> subscribe_observer(
        executor::comm::TopicSubscriptionOptions options = {}) {
        return observers_.subscribe(std::move(options));
    }

    // ---- 可观测性（EXEC-06；stats 仅 owner 上下文读取）----
    [[nodiscard]] const AppStateOwnerStats& stats() const noexcept { return stats_; }
    [[nodiscard]] executor::comm::CommStats update_channel_stats() const noexcept {
        return updates_.stats();
    }
    [[nodiscard]] executor::comm::CommStats event_inbox_stats() const noexcept {
        return event_inbox_.stats();
    }
    [[nodiscard]] executor::comm::CommStats event_outbox_stats() const noexcept {
        return event_outbox_.stats();
    }
    [[nodiscard]] executor::comm::CommStats connection_path_stats() const noexcept {
        return connection_path_.stats();
    }

private:
    std::uint64_t progress_watermark() const noexcept {
        return stats_.updates_applied + stats_.updates_rejected + stats_.events_forwarded
            + stats_.events_dropped;
    }

    void drain_updates(std::size_t max_updates) {
        for (std::size_t i = 0; i < max_updates; ++i) {
            AppStateUpdate update;
            if (!updates_.try_receive(update)) {
                break;
            }
            if (apply(update)) {
                ++stats_.updates_applied;
            } else {
                ++stats_.updates_rejected;
            }
        }
    }

    void forward_events(std::size_t max_events) {
        for (std::size_t i = 0; i < max_events; ++i) {
            // 有界移交：outbox 只由 owner 单写者投递，预留容量检查后发送不会满；
            // 满时停止本轮转发，待消费侧排空后继续（背压可见于 event_inbox 水位）。
            if (event_outbox_.size_approx() >= options_.event_outbox_capacity) {
                break;
            }
            AppEvent event;
            if (!event_inbox_.try_receive(event)) {
                break;
            }
            event.sequence = next_event_sequence_++;
            fan_out_to_observers(event);
            if (event_outbox_.try_send(std::move(event))) {
                ++stats_.events_forwarded;
            } else {
                ++stats_.events_dropped;
                break;
            }
        }
    }

    void fan_out_to_observers(const AppEvent& event) {
        if (observers_.subscriber_count() == 0) {
            return;
        }
        const AppEventPtr shared = std::make_shared<const AppEvent>(event);
        const auto result = observers_.publish(shared);
        stats_.observer_rejects += result.rejected_subscribers;
    }

    void publish_if_dirty() {
        if (!snapshot_dirty_) {
            return;
        }
        snapshot_dirty_ = false;
        snapshot_.publish(current_);
        ++stats_.snapshots_published;
    }

    // 返回 false 表示更新被拒绝（非法状态转移、终态复活或容量超限）。
    bool apply(const AppStateUpdate& update) {
        bool accepted = false;
        std::visit(
            [this, &accepted](const auto& concrete) {
                accepted = apply_impl(concrete);
            },
            update);
        return accepted;
    }

    bool apply_impl(const UpsertDevice& upsert) {
        auto& devices = current_.devices.devices;
        for (auto& existing : devices) {
            if (!(existing.id == upsert.device.id)) {
                continue;
            }
            if (!trust_allows(existing.trust_state, upsert.device.trust_state)) {
                return false;
            }
            existing = upsert.device;
            snapshot_dirty_ = true;
            return true;
        }
        if (devices.size() >= limits_.max_devices) {
            return false;
        }
        devices.push_back(upsert.device);
        snapshot_dirty_ = true;
        return true;
    }

    bool apply_impl(const UpsertConversation& upsert) {
        auto& conversations = current_.conversations.conversations;
        for (auto& existing : conversations) {
            if (!(existing.id == upsert.conversation.id)) {
                continue;
            }
            if (!state_machine_allows(existing.state, upsert.conversation.state)) {
                return false;
            }
            existing = upsert.conversation;
            snapshot_dirty_ = true;
            return true;
        }
        if (conversations.size() >= limits_.max_conversations) {
            return false;
        }
        conversations.push_back(upsert.conversation);
        snapshot_dirty_ = true;
        return true;
    }

    bool apply_impl(const UpsertMessage& upsert) {
        auto& messages = current_.messages.messages;
        for (auto& existing : messages) {
            if (!(existing.id == upsert.message.id)) {
                continue;
            }
            if (!state_machine_allows(existing.state, upsert.message.state)) {
                return false;
            }
            existing = upsert.message;
            snapshot_dirty_ = true;
            return true;
        }
        if (messages.size() >= limits_.max_messages) {
            return false;
        }
        messages.push_back(upsert.message);
        snapshot_dirty_ = true;
        return true;
    }

    bool apply_impl(const UpsertTransfer& upsert) {
        auto& transfers = current_.transfers.transfers;
        for (auto& existing : transfers) {
            if (!(existing.id == upsert.transfer.id)) {
                continue;
            }
            if (!state_machine_allows(existing.state, upsert.transfer.state)) {
                return false;
            }
            existing = upsert.transfer;
            snapshot_dirty_ = true;
            return true;
        }
        if (transfers.size() >= limits_.max_transfers) {
            return false;
        }
        transfers.push_back(upsert.transfer);
        snapshot_dirty_ = true;
        return true;
    }

    bool apply_impl(const UpdateTransferProgress& progress) {
        for (auto& existing : current_.transfers.transfers) {
            if (!(existing.id == progress.transfer)) {
                continue;
            }
            // 迟到的进度不得复活已终结的传输（RULE-08，设计第 10.1 节）。
            if (aki::transfer::is_terminal(existing.state)) {
                return false;
            }
            existing.transferred = progress.transferred;
            existing.total = progress.total;
            snapshot_dirty_ = true;
            return true;
        }
        return false;
    }

    bool apply_impl(const SetConnectionPath& set_path) {
        connection_path_.publish(set_path.path);
        return true;  // 生效于 LatestMailbox，不触发 Store 快照发布。
    }

    // 状态机校验：目标与当前一致视为幂等 no-op（终态重复宣告合法）；
    // 其余转移必须落在对应状态机的合法边上（M1-01 领域状态机）。
    static bool trust_allows(aki::device::TrustState from, aki::device::TrustState to) {
        return from == to || aki::device::can_transition(from, to);
    }

    template <typename State>
    static bool state_machine_allows(State from, State to) {
        return from == to || can_transition(from, to);
    }

    Options options_;
    AppStateLimits limits_;
    executor::comm::DoubleBuffer<AppState> snapshot_;
    AppState current_;  // 仅 owner 上下文访问的合成中的状态。
    executor::comm::MpscChannel<AppStateUpdate> updates_;
    executor::comm::MpscChannel<AppEvent> event_inbox_;
    executor::comm::MpscChannel<AppEvent> event_outbox_;
    executor::comm::LatestMailbox<aki::device::ConnectionPath> connection_path_;
    executor::comm::Topic<AppEventPtr> observers_;
    AppStateOwnerStats stats_;
    std::uint64_t next_event_sequence_ = 1;
    bool snapshot_dirty_ = false;  // 仅 owner 上下文访问。
    bool closed_ = false;          // 仅 owner 上下文置位；读取方以通道结果为准。
};

}  // namespace aki::app
