// Manager 单飞有界排空泵（设计第 8.3 节，DEC-008；EXEC-02/04/06/07）。
//
// 四个 Manager 共用的事件/命令执行上下文：工作项入有界 MpscChannel 收件箱 →
// CAS 抢占单飞标志 → submit_auto 一个排空任务（有界批量；保留并消费其 future——
// 提交成功不等于执行成功，影响任务结果的异常不得被丢弃）。事件与宿主命令共用
// 同一收件箱串行处理，Manager 内部状态只在排空上下文访问。
//
// 并发契约（executor-integration tasks-and-lifecycle 卡 + AGENTS 规则 3/9/10）：
//   - enqueue()/flush()/stats()：任意上下文；收件箱是有界资源，满即明确拒绝，
//     不静默重试。
//   - handler_ 与收件箱消费：仅排空任务上下文（单飞保证同一 Manager 时刻至多
//     一个排空任务在飞）。
//   - pending_futures_ 句柄槽由 futures_mutex_ 保护：只裁决排空任务 future 的
//     所有权移交（入队者生产、flush 消费），不是对 executor::comm 的替代——
//     跨上下文数据通道只有 MpscChannel。
//   - 排队软超时（task_timeout_ms）击杀、或提交即拒（max_in_flight_tasks 耗尽
//     → CapacityExhaustedException 即时就绪）的排空任务永远不会运行，也就不会
//     自复位单飞标志；下一次 enqueue/flush 消费其就绪 future（异常入统计）、
//     复位单飞代号自愈，随后按存量重排排空任务（存量不丢）。
//   - handler 异常不在泵内吞掉：排空任务 future 携带异常，Executor 的
//     task_exception_count 同时可见（AGENTS 规则 9，事实源是 Executor 设施）。
#pragma once

#include <executor/comm/channel.hpp>
#include <executor/comm/types.hpp>
#include <executor/executor.hpp>
#include <executor/types.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace aki::app {

// 构造选项置于命名空间作用域：类内默认实参引用嵌套类型的 NSDMI 在 GCC 下非法
// （同 app_state_owner.hpp 的处理）。
struct ManagerPumpOptions {
    std::string name;                        // 收件箱与诊断名（EXEC-06 可观测）
    std::size_t inbox_capacity = 256;        // RULE-09：收件箱容量预算
    std::size_t drain_batch = 32;            // 有界工作单元（AGENTS 工程约束）
};

class ManagerPumpStats {
public:
    void add_enqueued(std::uint64_t n = 1) noexcept { enqueued_.fetch_add(n); }
    void add_inbox_rejections(std::uint64_t n = 1) noexcept {
        inbox_rejections_.fetch_add(n);
    }
    void add_processed(std::uint64_t n = 1) noexcept { processed_.fetch_add(n); }
    void add_handler_rejections(std::uint64_t n = 1) noexcept {
        handler_rejections_.fetch_add(n);
    }
    void add_spawn_count(std::uint64_t n = 1) noexcept { spawn_count_.fetch_add(n); }
    void add_submit_rejections(std::uint64_t n = 1) noexcept {
        task_submit_rejections_.fetch_add(n);
    }
    void add_drain_failures(std::uint64_t n = 1) noexcept {
        drain_failures_.fetch_add(n);
    }
    void add_drain_timeouts(std::uint64_t n = 1) noexcept {
        drain_timeouts_.fetch_add(n);
    }

    struct Snapshot {
        std::uint64_t enqueued = 0;
        std::uint64_t inbox_rejections = 0;        // 收件箱满（背压可见）
        std::uint64_t processed = 0;
        std::uint64_t handler_rejections = 0;      // 业务侧拒绝（owner 背压等）
        std::uint64_t spawn_count = 0;             // 排空任务抢占/重排次数
        std::uint64_t task_submit_rejections = 0;  // 排空任务提交被拒（RULE-09）
        std::uint64_t drain_failures = 0;          // 排队软超时/异常终结的排空任务
        std::uint64_t drain_timeouts = 0;          // 其中排队软超时击杀数
    };

    [[nodiscard]] Snapshot snapshot() const noexcept {
        Snapshot s;
        s.enqueued = enqueued_.load();
        s.inbox_rejections = inbox_rejections_.load();
        s.processed = processed_.load();
        s.handler_rejections = handler_rejections_.load();
        s.spawn_count = spawn_count_.load();
        s.task_submit_rejections = task_submit_rejections_.load();
        s.drain_failures = drain_failures_.load();
        s.drain_timeouts = drain_timeouts_.load();
        return s;
    }

private:
    // 原子仅用于跨上下文可读的域名义计数；任务健康的事实源仍是 Executor
    // 监控设施（AGENTS 规则 9），此处不复刻其计数。
    std::atomic<std::uint64_t> enqueued_{0};
    std::atomic<std::uint64_t> inbox_rejections_{0};
    std::atomic<std::uint64_t> processed_{0};
    std::atomic<std::uint64_t> handler_rejections_{0};
    std::atomic<std::uint64_t> spawn_count_{0};
    std::atomic<std::uint64_t> task_submit_rejections_{0};
    std::atomic<std::uint64_t> drain_failures_{0};
    std::atomic<std::uint64_t> drain_timeouts_{0};
};

// 单飞有界排空泵。Work 须可移动；Handler 为 bool(Work&)，返回 false 表示该工作
// 项被业务侧拒绝（可观测，不静默）。
template <typename Work>
class ManagerPump {
public:
    using Handler = std::function<bool(Work&)>;

    ManagerPump(executor::Executor& executor, ManagerPumpOptions options,
        Handler handler)
        : executor_(executor),
          options_(std::move(options)),
          inbox_(executor::comm::ChannelOptions{.capacity = options_.inbox_capacity,
              .enable_stats = true,
              .name = options_.name + ".inbox"}),
          handler_(std::move(handler)) {}

    ManagerPump(const ManagerPump&) = delete;
    ManagerPump& operator=(const ManagerPump&) = delete;

    // 入队即 admission；随后确保单飞排空在飞。返回 false 表示收件箱满
    // （背压可见，RULE-09）。
    [[nodiscard]] bool enqueue(Work work) {
        if (!inbox_.try_send(std::move(work))) {
            stats_.add_inbox_rejections();
            return false;
        }
        stats_.add_enqueued();
        ensure_pump();
        return true;
    }

    // 有界等待泵静止：收件箱空、无在飞排空任务且排空 future 全部已消费。
    // 预算耗尽返回 false（证据不伪造）。宿主关闭钩子与测试使用。
    [[nodiscard]] bool flush(std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        for (;;) {
            consume_settled_futures();
            if (in_flight_generation_.load() == 0) {
                if (inbox_.size_approx() == 0) {
                    if (pending_empty()) {
                        return true;  // 泵静止（future 全部已消费）。
                    }
                    // 在飞为 0 但仍有未消费 future：任务已释放单飞、promise 尚
                    // 未结算的短暂窗口——不加新排空任务，退让等待结算后由顶部
                    // consume 消费（避免多派一个空转排空任务）。
                } else {
                    // 存量未排空（首次入队自愈重排、或提交即拒/软超时后的重试）：
                    // 抢占单飞并重排排空任务。
                    spawn_drain();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else if (!wait_current_pump(deadline)) {
                return false;  // 预算内未静止，如实上报（证据不伪造）。
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                consume_settled_futures();
                return in_flight_generation_.load() == 0 && inbox_.size_approx() == 0
                    && pending_empty();
            }
        }
    }

    [[nodiscard]] ManagerPumpStats::Snapshot stats() const noexcept {
        return stats_.snapshot();
    }

    [[nodiscard]] executor::comm::CommStats inbox_stats() const noexcept {
        return inbox_.stats();
    }

    [[nodiscard]] const std::string& name() const noexcept { return options_.name; }

private:
    // 消费所有已就绪的排空任务 future（异常入统计，不静默，AGENTS 规则 3/9）。
    // 就绪顺序不保证与提交顺序一致（前代任务的 promise 结算可晚于后代任务
    // 完成），因此扫描整个句柄槽。排队软超时击杀或提交即拒的任务从未运行、
    // 不会自复位单飞标志，消费其 future 时复位，实现自愈（后续 enqueue/flush
    // 按存量重排）。
    void consume_settled_futures() {
        std::lock_guard<std::mutex> guard(futures_mutex_);
        bool consumed_any = true;
        while (consumed_any) {
            consumed_any = false;
            for (auto it = pending_futures_.begin(); it != pending_futures_.end(); ++it) {
                if (it->second.wait_for(std::chrono::seconds(0))
                    != std::future_status::ready) {
                    continue;
                }
                const auto generation = it->first;
                consume_drain_future(std::move(it->second));
                pending_futures_.erase(it);
                release_in_flight_if_current(generation);
                consumed_any = true;
                break;
            }
        }
    }

    void consume_drain_future(std::future<void> future) {
        try {
            future.get();
        } catch (const executor::CapacityExhaustedException&) {
            stats_.add_submit_rejections();  // max_in_flight_tasks 准入拒绝。
        } catch (const executor::TimedOutException&) {
            stats_.add_drain_timeouts();
            stats_.add_drain_failures();
        } catch (...) {
            stats_.add_drain_failures();
        }
    }

    [[nodiscard]] bool pending_empty() {
        std::lock_guard<std::mutex> guard(futures_mutex_);
        return pending_futures_.empty();
    }

    void ensure_pump() {
        consume_settled_futures();
        spawn_drain();
    }

    // 抢占单飞并提交排空任务；已有单飞在飞时不做任何事。提交即拒（如
    // max_in_flight_tasks 耗尽）时 future 即时就绪，由下一轮 consume 计数并
    // 自愈——拒绝可见，不静默（RULE-09）。
    void spawn_drain() {
        std::uint64_t expected = 0;
        const std::uint64_t generation = next_generation();
        if (!in_flight_generation_.compare_exchange_strong(expected, generation)) {
            return;  // 已有单飞在飞：无需新排空任务。
        }
        std::future<void> future =
            executor_.submit_auto([this, generation] { drain_loop(generation); });
        stats_.add_spawn_count();
        std::lock_guard<std::mutex> guard(futures_mutex_);
        pending_futures_.emplace_back(generation, std::move(future));
    }

    // 排空任务主体：MpscChannel 是"单逻辑消费者"通道（channel.hpp 契约），
    // 在飞代号即消费者权——批处理期间不得释放（否则两个排空任务可并发
    // try_receive：工作项丢失 + 节点损坏；M4-02 观察项①登记、随 M4-03 CI
    // 修复收口，修正模式同 transfer_session_manager.hpp drain_loop）。释放
    // 在且仅在退出决策点，随后终检续期或让新 spawner 接管，恰其一，维持单
    // 消费者；释放与终检之间入箱的工作项由本循环续期处理（丢失唤醒防护，
    // DEC-008 风险 1）。handler 异常不在此捕获——future 携带异常上浮。
    void drain_loop(std::uint64_t generation) {
        for (;;) {
            Work work;
            for (std::size_t i = 0;
                i < options_.drain_batch && inbox_.try_receive(work); ++i) {
                if (handler_(work)) {
                    stats_.add_processed();
                } else {
                    stats_.add_handler_rejections();
                }
            }
            if (inbox_.size_approx() == 0) {
                release_in_flight_if_current(generation);
                if (inbox_.size_approx() == 0) {
                    return;
                }
                std::uint64_t expected = 0;
                if (!in_flight_generation_.compare_exchange_strong(
                        expected, generation)) {
                    return;  // 新 spawner 已接管后续排空（其任务在途）。
                }
            }
        }
    }

    // 释放单飞标志：仅当 generation 仍是当前在飞代号（终态只释放一次；
    // 后续 spawn 已换新代号时不得误清）。
    void release_in_flight_if_current(std::uint64_t generation) noexcept {
        std::uint64_t expected = generation;
        in_flight_generation_.compare_exchange_strong(expected, 0);
    }

    // 有界等待当前在飞排空任务终结并消费其 future。等待为短临界区轮询
    //（1ms 粒度）：不持 futures_mutex_ 等待未结算 future——M4-04 起排空
    // handler 内存在自续接入队（进度聚合/回调重入，transfer_manager.hpp），
    // 入队侧 ensure_pump 需要同一 mutex，持锁等待会与其互锁至预算耗尽
    //（排空 future 的结算依赖 handler 完成）。轮询形态下锁只覆盖就绪
    // 检查与消费；flush 语义（静止收敛、预算耗尽如实返回 false）不变。
    [[nodiscard]] bool wait_current_pump(std::chrono::steady_clock::time_point deadline) {
        for (;;) {
            std::uint64_t generation = 0;
            bool pending_found = false;
            {
                std::lock_guard<std::mutex> guard(futures_mutex_);
                generation = in_flight_generation_.load();
                for (auto it = pending_futures_.begin();
                    it != pending_futures_.end(); ++it) {
                    if (it->first != generation) {
                        continue;
                    }
                    pending_found = true;
                    if (it->second.wait_for(std::chrono::seconds(0))
                        == std::future_status::ready) {
                        consume_drain_future(std::move(it->second));
                        pending_futures_.erase(it);
                        release_in_flight_if_current(generation);
                        return true;
                    }
                    break;  // 当前代号至多一个句柄槽
                }
            }
            if (!pending_found) {
                // spawn 在 CAS 之后才入槽：短暂窗口内可能还没有 future，
                // 退让重试（同既有 yield 语义）。
                std::this_thread::yield();
                return std::chrono::steady_clock::now() < deadline;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;  // 预算耗尽：如实上报（证据不伪造）。
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    [[nodiscard]] std::uint64_t next_generation() noexcept {
        return generation_counter_.fetch_add(1) + 1;  // 0 保留为“无在飞”。
    }

    // 成员声明顺序即初始化顺序（GCC -Werror=reorder）：inbox_ 先于 handler_。
    executor::Executor& executor_;
    ManagerPumpOptions options_;
    executor::comm::MpscChannel<Work> inbox_;
    Handler handler_;
    // 单飞标志与代号合一：0 = 无在飞，非 0 = 在飞排空的代号。CAS 仲裁唯一
    // 排空任务；排队软超时/提交即拒的任务由消费其 future 的一方复位（自愈）。
    std::atomic<std::uint64_t> in_flight_generation_{0};
    std::atomic<std::uint64_t> generation_counter_{0};
    // 排空任务 future 句柄槽：入队者（任意上下文）生产、flush 消费。
    std::mutex futures_mutex_;
    std::deque<std::pair<std::uint64_t, std::future<void>>> pending_futures_;
    ManagerPumpStats stats_;
};

}  // namespace aki::app
