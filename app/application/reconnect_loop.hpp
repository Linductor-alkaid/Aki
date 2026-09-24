// 重连协调器（DEC-006 映射 6 恢复路径；SCOPE-11；M3-07；EXEC-05 长任务
// 取消与超时闭合，DEC-008 任务承载/宿主关闭纪律）。
//
// 职责：对端断开（peer_sessions diff 管道的 disconnected 事件，M3-06）后，
// 以 submit_cancellable 长任务周期重试重连动作，直至会话恢复（authenticated）
// 或有界预算耗尽（EXEC-05 超时语义：预算耗尽如实计数，不悬挂）；StopToken
// 使重连循环对 Executor 生命周期视图可见，受控关闭钩子（EXEC-01 步骤 1）
// 先取消并消费在途 future 再进入 owner 步骤 2/3（DEC-008）。
//
// 契约：
//   - try_reconnect/is_recovered 为注入的 aki/std 回调（组合根/adapter 提供，
//     如 NodeSession::connect_lan / session_authenticated——RULE-10：heyaki
//     类型不出本层公开面）；回调在 executor 任务上下文执行（EXEC-02 语义由
//     回调实现方保持有界）。
//   - 每 peer 至多一个在途循环（重复 start 同 peer 返回 false，单飞纪律；
//     检查/提交/登记在同一临界区——同 peer 并发 start 亦只获准一次）。
//   - 回调异常经 future 结算（RULE-09 可见，不静默）；统计仅记账不掩盖。
//   - stop_all()（宿主关闭钩子）：request_task_cancel 全部在途循环 + 有界
//     等待并消费全部 future（DEC-008：先取消并消费再交还 owner）。
//
// RULE-10：本组件在 app/application，仅消费 aki/std 面（NodeSession 的
// aki/std 方法经注入回调间接使用），零 heyaki 类型。
#pragma once

#include "device/device/device_types.hpp"

#include <executor/executor.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace aki::app {

// 构造选项置于命名空间作用域（同仓库既有处理，GCC 纪律）。
struct ReconnectCoordinatorOptions {
    std::chrono::milliseconds retry_interval{500};   // 重试间隔（可中断切片等待）
    std::chrono::milliseconds recovery_budget{30000};  // 单循环有界预算（EXEC-05）
    std::chrono::milliseconds stop_wait_budget{3000};  // stop_all 单 future 消费预算
};

// 观测统计（EXEC-06；跨上下文原子读）。
struct ReconnectCoordinatorStats {
    std::uint64_t started = 0;          // 循环启动数
    std::uint64_t recovered = 0;        // 会话恢复数（正常完成）
    std::uint64_t budget_exhausted = 0; // 预算耗尽数（超时语义，如实记录）
    std::uint64_t cancelled = 0;        // 停止请求退出数（执行中取消）
    std::uint64_t callback_failures = 0;// 回调异常数（future 结算可见）
};

class ReconnectCoordinator {
public:
    // try_reconnect：单次重连尝试（如 connect_lan/restart_session）；
    // is_recovered：会话恢复判定（如 session_authenticated）。
    using Attempt = std::function<bool()>;
    using RecoveryCheck = std::function<bool()>;

    struct PerPeerHooks {
        Attempt try_reconnect;
        RecoveryCheck is_recovered;
    };

    explicit ReconnectCoordinator(executor::Executor& executor,
        ReconnectCoordinatorOptions options = {})
        : executor_(executor), options_(options) {}

    ReconnectCoordinator(const ReconnectCoordinator&) = delete;
    ReconnectCoordinator& operator=(const ReconnectCoordinator&) = delete;

    // 启动对端重连循环（长任务，submit_cancellable + StopToken，EXEC-05）。
    // false = 该 peer 已有在途循环（单飞）或提交被拒（RULE-09 可见）。
    //
    // 线程纪律：单飞检查、submit 与登记在同一临界区。若先释放锁再提交，同
    // peer 并发两次 start 可双双通过检查并各自 submit——第二个 emplace 因
    // key 已存在静默失败（返回值被忽略仍 return true），其 handle/future 被
    // 丢弃：产生 stop_all 永不取消/消费的失控循环（违反 AGENTS 规则 3 与
    // 本文件单飞契约），并可能在 shutdown 后继续调用注入回调。临界区内不
    // 等待：submit_cancellable 是非阻塞入队 admission（提交即拒经 handle
    // 无效可见），循环任务体 run_loop/finish 只触碰原子量不获取 mutex_，
    // stop_all 持锁仅做移出、在锁外取消/等待——无死锁环。
    [[nodiscard]] bool start(const aki::device::DeviceId& peer,
        const PerPeerHooks& hooks) {
        const std::string key = peer.value;
        std::lock_guard<std::mutex> guard(mutex_);
        auto existing = loops_.find(key);
        if (existing != loops_.end()) {
            if (existing->second.future.wait_for(
                    std::chrono::seconds(0))
                != std::future_status::ready) {
                return false;  // 在途单飞（同 peer 不并发循环）
            }
            // 上一轮已终结：就地消费 future（RULE-09 对账）后允许新一轮。
            try {
                existing->second.future.get();
            } catch (...) {
                // 异常已在 callback_failures/executor failure 体系可见。
            }
            loops_.erase(existing);
        }
        auto submission = executor_.submit_cancellable(
            [this, key, hooks](executor::StopToken stop_token) {
                run_loop(key, hooks.try_reconnect, hooks.is_recovered,
                    std::move(stop_token));
            });
        if (!submission.handle.valid()) {
            return false;  // 提交即拒（RULE-09）。
        }
        loops_.emplace(key,
            LoopRecord{submission.handle, std::move(submission.future)});
        started_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool running(const aki::device::DeviceId& peer) const {
        std::lock_guard<std::mutex> guard(mutex_);
        return loops_.count(peer.value) != 0U;
    }

    [[nodiscard]] std::size_t running_count() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return loops_.size();
    }

    // 宿主关闭钩子（EXEC-01 步骤 1）：取消全部在途循环 + 有界消费 future。
    // 返回消费的 future 数（零悬挂对账）。
    [[nodiscard]] std::size_t stop_all() {
        std::map<std::string, LoopRecord> drained;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            drained = std::move(loops_);
            loops_.clear();
        }
        for (auto& [key, record] : drained) {
            (void)key;
            executor_.request_task_cancel(record.handle);
        }
        std::size_t consumed = 0;
        for (auto& [key, record] : drained) {
            (void)key;
            if (record.future.wait_for(options_.stop_wait_budget)
                == std::future_status::ready) {
                try {
                    record.future.get();
                } catch (...) {
                    // 循环内回调异常/取消结算：消费即对账（failure 计数已在
                    // executor failure 体系可见）。
                }
                ++consumed;
            }
        }
        return consumed;
    }

    [[nodiscard]] ReconnectCoordinatorStats stats() const noexcept {
        ReconnectCoordinatorStats snapshot;
        snapshot.started = started_.load(std::memory_order_relaxed);
        snapshot.recovered = recovered_.load(std::memory_order_relaxed);
        snapshot.budget_exhausted =
            budget_exhausted_.load(std::memory_order_relaxed);
        snapshot.cancelled = cancelled_.load(std::memory_order_relaxed);
        snapshot.callback_failures =
            callback_failures_.load(std::memory_order_relaxed);
        return snapshot;
    }

private:
    struct LoopRecord {
        executor::TaskHandle handle;
        std::future<void> future;
    };

    void run_loop(const std::string& key, const Attempt& try_reconnect,
        const RecoveryCheck& is_recovered, executor::StopToken stop_token) {
        const auto deadline =
            std::chrono::steady_clock::now() + options_.recovery_budget;
        while (!stop_token.stop_requested()) {
            try {
                (void)try_reconnect();  // 失败（false）继续重试
            } catch (...) {
                callback_failures_.fetch_add(1, std::memory_order_relaxed);
                // 不吞异常退出：异常经 future 结算由消费方感知；但循环内
                // 异常不终止重连职责——继续下一轮（EXEC-05 长任务韧性）。
            }
            if (is_recovered()) {
                recovered_.fetch_add(1, std::memory_order_relaxed);
                finish(key);
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                budget_exhausted_.fetch_add(1, std::memory_order_relaxed);
                finish(key);
                return;  // EXEC-05 超时语义：预算耗尽如实退出，不悬挂
            }
            // 可中断切片等待（stop 到达最多一个切片延迟）。
            const auto until =
                std::chrono::steady_clock::now() + options_.retry_interval;
            while (!stop_token.stop_requested()
                && std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
        }
        cancelled_.fetch_add(1, std::memory_order_relaxed);
        finish(key);
    }

    void finish(const std::string&) {
        // 记录保留至 stop_all/start 就地消费（future 消费纪律，AGENTS 规则 3）。
    }

    executor::Executor& executor_;
    ReconnectCoordinatorOptions options_;
    mutable std::mutex mutex_;
    std::map<std::string, LoopRecord> loops_;
    std::atomic<std::uint64_t> started_{0};
    std::atomic<std::uint64_t> recovered_{0};
    std::atomic<std::uint64_t> budget_exhausted_{0};
    std::atomic<std::uint64_t> cancelled_{0};
    std::atomic<std::uint64_t> callback_failures_{0};
};

}  // namespace aki::app
