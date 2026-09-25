// 传输会话管理器（设计第 7.1 节①/M4-02；§7 七状态机；DEC-008 模式扩展；
// EXEC-05 长任务 + EXEC-07 句柄按业务稳定 ID）。
//
// 职责（§7.1①）：
//   - 七状态机全覆盖：Queued/Negotiating/Transferring/Paused/Completed/
//     Failed/Cancelled；合法/非法转移经 M1 状态机校验（终态幂等、迟到事件
//     不复活，RULE-08）。
//   - 出站四接口（发起/暂停/恢复/取消）全部经单飞排空泵上下文串行处理
//     （§7.1①）：会话任务由泵在 start 命令上下文派生（submit_cancellable +
//     StopToken，EXEC-05），不在调用方线程派生。
//   - 有界收件箱 + 单飞排空泵（DEC-008 模式：CAS 代号单飞、在飞代号自愈、
//     排队软超时击杀后存量经重排不丢）。
//   - TaskHandle 按业务稳定 TransferId 显式持有（EXEC-07）；取消一律
//     request_task_cancel（EXEC-05；executor 对终态/过期句柄幂等返回
//     AlreadyCompleted/NotFound，不写 failure——2026-09-25 探针复证，
//     M4-02 受阻记录的「request_task_cancel 不稳定」系误诊）。
//
// 拒绝可见性（RULE-09/AGENTS 规则 10）：
//   - 四接口返回值 = 参数校验 + 会话表登记 + 收件箱准入；收件箱满返回
//     false 且不留记录、不投递事件。
//   - 泵内提交即拒（executor SubmitRejected failure 事件可见）时回收记录：
//     Queued 从未投递、state_of(id) 复位 nullopt——无幽灵事件。
//   - 对未知 id 的暂停/恢复/取消返回 false；已入箱的迟到命令在泵内幂等
//     无效（不复活已终结/已清理会话）。
//
// 事件面（DEC-008 双 Manager 扇出入口）：状态转移仅在状态实际变化时经
// on_transition 投递一次（同态幂等不重报，终态重复宣告不复活）；进度经
// on_progress。线程契约：回调可能从泵 worker、会话 worker 或 stop_all
// 调用方线程触发——消费方须自行串行化（DEC-008 扇出 sink 为 executor
// 适配的线程安全入口）。
//
// 生命周期契约：stop_all()（EXEC-01 步骤 1 钩子）先把活动态会话推进
// Cancelled 并投递终态，再对全部已派生会话句柄 request_task_cancel，最后
// 在全局 deadline 预算内有界消费全部 future（预算不随会话数放大）；预算
// 耗尽未就绪的 future 由析构最终兜底消费（取消已请求 + io 回调有界 →
// 会话必然退出；会话任务经 this 捕获引用本管理器，析构不得先于其终结）。
// executor 关闭必须晚于管理器析构。
//
// RULE-09：收件箱有界，满即拒绝可见；RULE-10：公开面仅 aki/std（注入回调
// 承载 IO）；RULE-07：并发仅经 executor 公开能力。
#pragma once

#include "transfer/transfer/transfer_types.hpp"

#include <executor/comm/channel.hpp>
#include <executor/executor.hpp>
#include <executor/stop_token.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace aki::transfer {

// 构造选项（命名空间作用域，同仓库既有处理）。
struct TransferSessionManagerOptions {
    std::size_t inbox_capacity = 64;   // RULE-09：命令收件箱预算
    std::size_t drain_batch = 16;      // 单飞排空批量
    std::chrono::milliseconds poll_interval{10};  // 会话循环切片等待
    // stop_all 消费预算：全部 future 共享的全局 deadline（DEC-008 宿主
    // 关闭纪律；budget every wait——总量有界，不随会话数线性放大）。
    std::chrono::milliseconds stop_wait_budget{2000};
};

// 会话控制面（会话循环与命令面共享的原子旗标；shared_ptr 稳定跨清理）。
struct SessionControl {
    // 0 = running, 1 = paused（命令面置位，循环观测）。
    std::atomic<int> control{0};
    // 启动闸门：泵登记句柄并投递 Queued 后置 1。会话循环开跑前等待该
    // 闸门，保证消费者事件顺序恒为 Queued → Negotiating → …（DEC-008
    // 主路径顺序）；排队取消经 stop_token 在此窗口内退出。
    std::atomic<std::uint8_t> start_gate{0};
};

// 事件回调（DEC-008 双 Manager 扇出入口；消费方映射 typed 更新）。
struct TransferSessionEvents {
    // 状态转移成功且状态实际变化（消费方映射 UpsertTransfer/
    // CompleteTransfer，§7.1②）；同态幂等与迟到事件不投递。
    std::function<void(const TransferId&, TransferState)> on_transition;
    // 进度（transferred/total；消费方映射 UpdateTransferProgress）。
    std::function<void(const TransferId&, std::uint64_t, std::uint64_t)>
        on_progress;
};

// 分块 IO 注入回调（M4-04 接真实 .part 分块写入；测试注入网络无关回调）：
// 返回本 tick 传输的字节数；抛出异常 = 本 tick 失败（会话 → Failed）。
using IoChunkHook = std::function<std::uint64_t(const TransferId&)>;

class TransferSessionManager {
public:
    explicit TransferSessionManager(executor::Executor& executor,
        TransferSessionManagerOptions options = {},
        IoChunkHook io_chunk = {},
        TransferSessionEvents events = {})
        : executor_(executor),
          options_(options),
          io_chunk_(std::move(io_chunk)),
          events_(std::move(events)),
          inbox_(executor::comm::ChannelOptions{
              .capacity = options_.inbox_capacity,
              .enable_stats = true,
              .name = "aki.tsm.inbox"}) {}

    TransferSessionManager(const TransferSessionManager&) = delete;
    TransferSessionManager& operator=(const TransferSessionManager&) = delete;

    ~TransferSessionManager() {
        (void)stop_all();
        drain_unconsumed();  // 最终屏障：会话任务引用 this，析构不得先于其终结
    }

    // 发起传输（业务稳定 ID；重复 ID 拒绝——DEC-006：一个会话不可重复
    // 启动）。校验 + 登记 Queued 后经收件箱交泵串行处理（§7.1①）：Queued
    // 事件在泵内提交准入成功后投递，提交即拒则回收记录（见类注释拒绝
    // 可见性）——调用方线程不派生任务、不投递事件。
    [[nodiscard]] bool start_transfer(const TransferId& id,
        std::uint64_t total) {
        if (id.empty() || total == 0U) {
            return false;
        }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (sessions_.count(id.value) != 0U) {
                return false;  // 重复 ID：一个会话不可重复启动
            }
            sessions_.emplace(id.value,
                SessionRecord{std::make_shared<SessionControl>(),
                    TransferState::Queued});
        }
        if (!enqueue(Command{CommandKind::start, id, total})) {
            std::lock_guard<std::mutex> guard(mutex_);
            sessions_.erase(id.value);  // 收件箱满：回收记录，零事件
            return false;
        }
        return true;
    }

    [[nodiscard]] bool pause_transfer(const TransferId& id) {
        return enqueue_session_command(CommandKind::pause, id);
    }

    [[nodiscard]] bool resume_transfer(const TransferId& id) {
        return enqueue_session_command(CommandKind::resume, id);
    }

    [[nodiscard]] bool cancel_transfer(const TransferId& id) {
        return enqueue_session_command(CommandKind::cancel, id);
    }

    [[nodiscard]] std::optional<TransferState> state_of(
        const TransferId& id) const {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = sessions_.find(id.value);
        if (it == sessions_.end()) {
            return std::nullopt;
        }
        return it->second.state;
    }

    [[nodiscard]] std::size_t session_count() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return sessions_.size();
    }

    // 宿主关闭钩子（EXEC-01 步骤 1；沿 M3-07 次序）：活动态会话先推进
    // Cancelled 并投递终态（消费者拿到终态，重启恢复一致），再对全部已派生
    // 会话句柄 request_task_cancel（终态句柄由 executor 幂等兜底），最后在
    // options_.stop_wait_budget 全局 deadline 内有界消费全部 future。预算
    // 耗尽仍未就绪的 future 移入未消费集合（后续 stop_all 重试；析构最终
    // 兜底无界等待——取消已请求且 io 回调有界，会话必然退出，不得成为孤儿
    // UAF）。返回消费的 future 数（零悬挂对账）。
    [[nodiscard]] std::size_t stop_all() {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        struct Pending {
            executor::TaskHandle handle;
            std::future<void> future;
        };
        std::vector<Pending> pending;
        std::vector<TransferId> cancelled_now;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            pending.reserve(sessions_.size());
            cancelled_now.reserve(sessions_.size());
            for (auto& [key, record] : sessions_) {
                if (!is_terminal(record.state)) {
                    (void)record.state_machine.transition_to(
                        TransferState::Cancelled);  // 活动态→Cancelled 恒合法
                    record.state = TransferState::Cancelled;
                    cancelled_now.push_back(TransferId{key});
                }
                // 起始命令仍在收件箱的记录（句柄未登记）没有 future 可消费：
                // 跳过；泵处理 start 命令时记录已清理，不会派生孤儿。
                if (record.handle.valid()) {
                    pending.push_back(Pending{record.handle,
                        std::move(record.future)});
                }
            }
            sessions_.clear();
        }
        // 锁外投递终态（回调可能重入 state_of 等加锁接口）。
        for (const auto& id : cancelled_now) {
            report(id, TransferState::Cancelled);
        }
        for (auto& item : pending) {
            if (item.handle.valid()) {
                executor_.request_task_cancel(item.handle);
            }
        }
        // 并入此前预算耗尽遗留的 future：重复 stop_all 对其继续消费。
        {
            std::lock_guard<std::mutex> guard(futures_mutex_);
            pending.reserve(pending.size() + unconsumed_.size());
            for (auto& future : unconsumed_) {
                pending.push_back(Pending{executor::TaskHandle{},
                    std::move(future)});
            }
            unconsumed_.clear();
        }
        std::vector<std::future<void>> leftover;
        std::size_t consumed = 0;
        const auto deadline =
            std::chrono::steady_clock::now() + options_.stop_wait_budget;
        for (auto& item : pending) {
            auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining < std::chrono::milliseconds::zero()) {
                remaining = std::chrono::milliseconds::zero();
            }
            if (item.future.wait_for(remaining)
                != std::future_status::ready) {
                leftover.push_back(std::move(item.future));  // 不丢弃：析构兜底
                continue;
            }
            try {
                item.future.get();
            } catch (...) {
                // 取消结算（TaskCancelled）等：消费即对账，失败已在
                // executor failure/取消体系可见。
            }
            ++consumed;
        }
        if (!leftover.empty()) {
            std::lock_guard<std::mutex> guard(futures_mutex_);
            unconsumed_.insert(unconsumed_.end(),
                std::make_move_iterator(leftover.begin()),
                std::make_move_iterator(leftover.end()));
        }
        return consumed;
    }

private:
    enum class CommandKind { start, pause, resume, cancel };
    struct Command {
        CommandKind kind;
        TransferId id;
        std::uint64_t total = 0;
    };

    struct SessionRecord {
        std::shared_ptr<SessionControl> control;
        TransferStateMachine state_machine;           // mutex_ 保护
        TransferState state = TransferState::Queued;  // mutex_ 保护
        executor::TaskHandle handle{};                // mutex_ 保护
        std::future<void> future{};                   // mutex_ 保护（stop_all 移出消费）
    };

    [[nodiscard]] bool enqueue_session_command(CommandKind kind,
        const TransferId& id) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (sessions_.count(id.value) == 0U) {
                return false;  // 未知 id：拒绝可见（迟到命令本就幂等无效）
            }
        }
        return enqueue(Command{kind, id, 0U});
    }

    [[nodiscard]] std::uint64_t next_generation() noexcept {
        return generation_counter_.fetch_add(1) + 1;
    }

    [[nodiscard]] bool enqueue(Command command) {
        if (inbox_.try_send(std::move(command))) {
            ensure_pump();
            return true;
        }
        return false;  // 收件箱满：拒绝可见（RULE-09）
    }

    // ---- 单飞排空泵（DEC-008 模式：CAS 代号、在飞自愈、软超时自愈）----

    void ensure_pump() {
        consume_settled_futures();
        spawn_drain();
    }

    void spawn_drain() {
        std::uint64_t expected = 0;
        const std::uint64_t generation = next_generation();
        if (!in_flight_generation_.compare_exchange_strong(expected,
                generation)) {
            return;  // 已有单飞在飞
        }
        std::future<void> future = executor_.submit_auto(
            [this, generation] { drain_loop(generation); });
        {
            std::lock_guard<std::mutex> guard(futures_mutex_);
            pending_futures_.emplace_back(generation, std::move(future));
        }
    }

    // 排空任务主体：MpscChannel 是"单逻辑消费者"通道（channel.hpp 契约），
    // 在飞代号即消费者权——批处理期间不得释放（否则两个排空任务可并发
    // try_receive：命令丢失 + 节点损坏）。释放在且仅在退出决策点，随后
    // 终检续期或让新 spawner 接管，恰其一，维持单消费者。
    void drain_loop(std::uint64_t generation) {
        for (;;) {
            Command command;
            for (std::size_t i = 0;
                i < options_.drain_batch && inbox_.try_receive(command); ++i) {
                handle_command(command);
            }
            if (inbox_.size_approx() == 0U) {
                release_in_flight(generation);
                // 丢失唤醒防护：释放与终检之间入箱的命令，由本循环续期
                // 处理，或由新 spawner 的 CAS 接管（其排空任务已在途）——
                // 本循环 CAS 失败即退出，不再触碰通道。
                if (inbox_.size_approx() == 0U) {
                    return;
                }
                std::uint64_t expected = 0;
                if (!in_flight_generation_.compare_exchange_strong(expected,
                        generation)) {
                    return;
                }
            }
        }
    }

    void release_in_flight(std::uint64_t generation) noexcept {
        std::uint64_t expected = generation;
        in_flight_generation_.compare_exchange_strong(expected, 0);
    }

    void consume_settled_futures() {
        std::lock_guard<std::mutex> guard(futures_mutex_);
        bool consumed_any = true;
        while (consumed_any) {
            consumed_any = false;
            for (auto it = pending_futures_.begin();
                it != pending_futures_.end(); ++it) {
                if (it->second.wait_for(std::chrono::seconds(0))
                    != std::future_status::ready) {
                    continue;
                }
                const auto generation = it->first;
                try {
                    it->second.get();
                } catch (...) {
                    // 排队软超时击杀（TimedOutException）/提交即拒等：失败
                    // 可见于 executor failure 体系；存量经重排不丢。
                }
                pending_futures_.erase(it);
                // 自愈：软超时击杀/提交即拒的排空任务从未运行 drain_loop，
                // 不会自复位单飞标志——消费其 future 时复位（沿
                // manager_runtime.hpp 先例）。缺失则 spawn_drain 的 CAS 永远
                // 失败，收件箱命令静默滞留、取消路径失效（规则 10）。
                release_in_flight(generation);
                consumed_any = true;
                break;
            }
        }
    }

    // ---- 命令处理（排空泵上下文；§7.1① 出站四接口串行点）----

    void handle_command(const Command& command) {
        switch (command.kind) {
            case CommandKind::start:
                start_session(command.id, command.total);
                break;
            case CommandKind::pause:
                set_control(command.id, 1);
                (void)request_transition(command.id, TransferState::Paused);
                break;
            case CommandKind::resume:
                set_control(command.id, 0);
                (void)request_transition(command.id,
                    TransferState::Transferring);
                break;
            case CommandKind::cancel:
                cancel_session(command.id);
                break;
        }
    }

    // 泵上下文派生会话长任务（EXEC-05）。提交准入成功后才登记句柄/future
    // 并投递 Queued——调用方视角的准入与实际活动状态一致。
    // lifecycle_mutex_ 串行化「submit→登记」与 stop_all 快照：提交后、登记
    // 前的窗口内 stop_all 不得清表（否则句柄未登记的活会话被跳过取消，
    // 成为析构后 UAF 的孤儿）；登记后 stop_all 必见句柄并取消+消费。
    void start_session(const TransferId& id, std::uint64_t total) {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        std::shared_ptr<SessionControl> control;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            auto it = sessions_.find(id.value);
            if (it == sessions_.end()
                || it->second.state != TransferState::Queued) {
                return;  // 迟到起始命令（stop_all 竞态/重复）：不复活
            }
            control = it->second.control;
        }
        auto submission = executor_.submit_cancellable(
            [this, id, total, control](
                executor::StopToken stop_token) mutable {
                session_loop(id, total, std::move(control),
                    std::move(stop_token));
            });
        // 同步拒绝信号：executor 的提交拒绝路径（停机 / cancellation
        // registry 容量耗尽 / max_in_flight 耗尽）都在提交调用内以异常结算
        // future——句柄恒先分配（submit_tracked 先 allocate_task_handle 再做
        // registry admission），handle.valid() 不是准入信号。真实会话任务
        // 不可能未调度即就绪。
        const bool rejected = !submission.handle.valid()
            || submission.future.wait_for(std::chrono::seconds(0))
                == std::future_status::ready;
        if (rejected) {
            if (submission.future.valid()) {
                try {
                    submission.future.get();
                } catch (...) {
                    // 拒绝异常已在 executor failure 体系可见
                    // （SubmitRejected），消费即对账（AGENTS 规则 3）。
                }
            }
            std::lock_guard<std::mutex> guard(mutex_);
            sessions_.erase(id.value);  // 回收记录；Queued 从未投递
            return;
        }
        bool deliver = false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            auto it = sessions_.find(id.value);
            if (it == sessions_.end()) {
                // stop_all 竞态清表（lifecycle_mutex_ 下理论不可达，防御保
                // 底）：任务已提交但记录已随关闭清理——立即排队取消并有界
                // 消费，避免孤儿（future 不得无主悬挂）。
                executor_.request_task_cancel(submission.handle);
                if (submission.future.wait_for(options_.stop_wait_budget)
                    == std::future_status::ready) {
                    try {
                        submission.future.get();
                    } catch (...) {
                        // 取消结算：消费即对账。
                    }
                }
                return;
            }
            it->second.handle = submission.handle;
            // future 必须随句柄一并保存（AGENTS 规则 3）：丢弃则 stop_all
            // 消费非法 future 抛 future_error(no_state)、运行中会话失去
            // 消费归属——M4-02 受阻记录 SIGSEGV 根因（2026-09-25 探针复现
            // "[probe] stop_all THREW: no state"）。注意：此处不得以
            // state != Queued 为接管条件——快速调度下会话任务可能先于本
            // 登记完成状态推进（Queued→Negotiating），记录存在即登记。
            it->second.future = std::move(submission.future);
            deliver = true;
        }
        if (deliver) {
            // 先投递 Queued 再放行会话任务（SessionControl::start_gate）：
            // 事件顺序恒为 Queued → Negotiating → …，不因 worker 抢跑而
            // 乱序；投递在锁外（回调可重入 state_of 等加锁接口）。
            report(id, TransferState::Queued);
            control->start_gate.store(1, std::memory_order_release);
        }
    }

    void set_control(const TransferId& id, int value) {
        std::shared_ptr<SessionControl> control;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            auto it = sessions_.find(id.value);
            if (it != sessions_.end()) {
                control = it->second.control;
            }
        }
        if (control != nullptr) {
            control->control.store(value, std::memory_order_relaxed);
        }
    }

    // ---- 状态转移（状态机校验；RULE-08 终态幂等/不复活；DEC-008 主路径
    // 事件只投递一次：仅状态实际变化时上报，同态幂等不重报）----

    [[nodiscard]] bool request_transition(const TransferId& id,
        TransferState to) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            auto it = sessions_.find(id.value);
            if (it == sessions_.end()) {
                return false;  // 迟到命令：会话已终结清理，不复活
            }
            const auto previous = it->second.state;
            if (!it->second.state_machine.transition_to(to)) {
                return false;  // 非法转移/终态迁出：拒绝且保持原状态
            }
            it->second.state = to;
            changed = previous != to;
        }
        if (changed) {
            report(id, to);
        }
        return true;
    }

    void report(const TransferId& id, TransferState state) {
        if (events_.on_transition != nullptr) {
            events_.on_transition(id, state);
        }
    }

    void cancel_session(const TransferId& id) {
        executor::TaskHandle handle;
        if (request_transition(id, TransferState::Cancelled)) {
            std::lock_guard<std::mutex> guard(mutex_);
            auto it = sessions_.find(id.value);
            if (it != sessions_.end()) {
                handle = it->second.handle;
            }
        }
        if (handle.valid()) {
            executor_.request_task_cancel(handle);  // EXEC-05 取消纪律
        }
    }

    // ---- 会话循环（EXEC-05 长任务上下文）----

    void session_loop(const TransferId& id, std::uint64_t total,
        const std::shared_ptr<SessionControl>& control,
        executor::StopToken stop_token) {
        // 启动闸门：等泵投递 Queued（事件顺序契约）；此窗口内的取消经
        // stop_token 退出。
        while (control->start_gate.load(std::memory_order_acquire) == 0) {
            if (stop_token.stop_requested()) {
                (void)request_transition(id, TransferState::Cancelled);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        if (stop_token.stop_requested()) {
            (void)request_transition(id, TransferState::Cancelled);
            return;
        }
        (void)request_transition(id, TransferState::Negotiating);
        if (stop_token.stop_requested()) {
            (void)request_transition(id, TransferState::Cancelled);
            return;
        }
        (void)request_transition(id, TransferState::Transferring);

        std::uint64_t transferred = 0;
        while (!stop_token.stop_requested()) {
            // Paused 观测（命令面置位；泵可能已先行转移，同态幂等）。
            if (control->control.load(std::memory_order_relaxed) == 1) {
                if (request_transition(id, TransferState::Paused)) {
                    while (!stop_token.stop_requested()
                        && control->control.load(std::memory_order_relaxed)
                            == 1) {
                        std::this_thread::sleep_for(
                            options_.poll_interval);
                    }
                    if (stop_token.stop_requested()) {
                        (void)request_transition(id,
                            TransferState::Cancelled);
                        return;
                    }
                    (void)request_transition(id, TransferState::Transferring);
                }
            }

            // 分块 IO（注入回调；M4-04 接真实 .part 写入）。
            std::uint64_t chunk = 0;
            try {
                if (io_chunk_ != nullptr) {
                    chunk = io_chunk_(id);
                }
            } catch (...) {
                // Failed 已报告（RULE-09 可见）；异常不外抛（void 任务）。
                (void)request_transition(id, TransferState::Failed);
                return;
            }
            if (chunk > 0U) {
                transferred += chunk;
                if (events_.on_progress != nullptr) {
                    events_.on_progress(id, transferred, total);
                }
                if (transferred >= total) {
                    (void)request_transition(id, TransferState::Completed);
                    return;
                }
            }
            // 切片等待（可被 stop/pause/resume 及时观测）。
            const auto until =
                std::chrono::steady_clock::now() + options_.poll_interval;
            while (std::chrono::steady_clock::now() < until
                && !stop_token.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
        }
        (void)request_transition(id, TransferState::Cancelled);
    }

    executor::Executor& executor_;
    TransferSessionManagerOptions options_;
    IoChunkHook io_chunk_;
    TransferSessionEvents events_;

    executor::comm::MpscChannel<Command> inbox_;
    std::atomic<std::uint64_t> in_flight_generation_{0};
    std::atomic<std::uint64_t> generation_counter_{0};
    std::mutex futures_mutex_;
    std::vector<std::pair<std::uint64_t, std::future<void>>> pending_futures_;

    mutable std::mutex mutex_;
    std::map<std::string, SessionRecord> sessions_;

    // 会话派生（submit→登记）与 stop_all 快照的互斥：堵住「句柄未登记即
    // 被快照跳过」的孤儿窗口（锁序一律 lifecycle_mutex_ → mutex_ →
    // futures_mutex_）。
    std::mutex lifecycle_mutex_;

    // stop_all 预算耗尽仍未消费的会话 future（futures_mutex_ 保护）：析构
    // 最终兜底无界消费——会话任务捕获 this，这些 future 未就绪前管理器存储
    // 不得视为安全终结。
    std::vector<std::future<void>> unconsumed_;

    void drain_unconsumed() {
        std::vector<std::future<void>> leftovers;
        {
            std::lock_guard<std::mutex> guard(futures_mutex_);
            if (unconsumed_.empty()) {
                return;
            }
            leftovers = std::move(unconsumed_);
            unconsumed_.clear();
        }
        for (auto& future : leftovers) {
            // 取消已在 stop_all 请求且 io 回调有界 → 会话必然退出。
            future.wait();
            try {
                future.get();
            } catch (...) {
                // 取消结算：消费即对账。
            }
        }
    }
};

}  // namespace aki::transfer
