// Transfer Manager 骨架（设计第 8.3 节，DEC-008；M1-05，EXEC-05/EXEC-07）。
//
// 只写 transfers Store：started/progress/completed 事件 → UpsertTransfer /
// UpdateTransferProgress / CompleteTransfer（终态幂等，迟到事件不复活，RULE-08）。
// 传输四接口是本域出站操作（→ Adapter）。应用发起的传输由会话任务承载：
// submit_cancellable + StopToken（executor 注入首参数），TaskHandle + future 按
// 业务稳定 TransferId 由本 Manager 显式持有；取消一律经
// executor().request_task_cancel(handle) 发起，会话任务轮询 stop_requested()
// 协作退出（不抢占、不引入 TimerHandle，M1 无周期负载）。
//
// M1 会话循环是协作取消骨架：轮询停止请求直到被取消（真实分块传输循环由 M4
// 替换）；会话只有在取消请求后才会退出，宿主关闭钩子必须先 request_cancel_all
// 并 flush 消费在途 future（设计第 8.3 节关闭顺序）。取消断言经
// get_cancellation_status()/ExecutorSnapshot.cancellation（EXEC-06）。
//
// 会话表/回收队列只在排空上下文访问；active_session_count() 原子仅供跨上下文
// 诊断。生命周期（EXEC-07）：本对象必须先于其会话任务终结。
#pragma once

#include "app/application/manager_runtime.hpp"
#include "app/state/app_events.hpp"
#include "app/state/app_state_owner.hpp"
#include "app/state/app_state_updates.hpp"
#include "device/device/device_types.hpp"
#include "heyaki/adapter/heyaki_adapter.hpp"
#include "transfer/transfer/transfer_types.hpp"

#include <executor/executor.hpp>
#include <executor/task_cancellation.hpp>
#include <executor/types.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <variant>

namespace aki::app {

struct TransferStartedWork {
    aki::transfer::Transfer transfer;
};

struct TransferProgressWork {
    aki::transfer::TransferId transfer;
    std::uint64_t transferred = 0;
    std::uint64_t total = 0;
};

struct TransferCompletedWork {
    aki::transfer::TransferId transfer;
    aki::transfer::TransferState final_state = aki::transfer::TransferState::Completed;
};

struct StartTransferWork {
    aki::device::DeviceId to;
    aki::transfer::TransferId transfer_id;
    aki::transfer::FileMetadata file;
};

struct PauseTransferWork {
    aki::transfer::TransferId transfer_id;
};

struct ResumeTransferWork {
    aki::transfer::TransferId transfer_id;
};

struct CancelTransferWork {
    aki::transfer::TransferId transfer_id;
};

// 关闭钩子用：请求取消全部在飞会话（EXEC-01 步骤 2 前置，句柄由 Manager 发起）。
struct CancelAllSessionsWork {};

// flush 哨兵：触发排空上下文内的会话回收（有界等待），完成后通知调用方。
struct ReapSessionsWork {
    std::shared_ptr<std::promise<void>> done;
};

using TransferManagerWork = std::variant<TransferStartedWork,
    TransferProgressWork,
    TransferCompletedWork,
    StartTransferWork,
    PauseTransferWork,
    ResumeTransferWork,
    CancelTransferWork,
    CancelAllSessionsWork,
    ReapSessionsWork>;

// M1 会话只有取消退出一种终相；Completed 由 M4 的真实分块循环引入。
enum class TransferSessionOutcome { CancelledByRequest };

// 构造选项置于命名空间作用域（同 AppStateOwnerOptions 处理，GCC 纪律）。
struct TransferManagerOptions {
    ManagerPumpOptions pump{};
    aki::device::DeviceId sender;  // 应用发起传输的 sender（本机设备）。
    // 会话轮询间隔：协作取消的解除阻塞粒度（must be > 0）。
    std::chrono::milliseconds session_poll_interval{50};
    // flush 时在排空上下文内回收会话的有界等待预算。
    std::chrono::milliseconds session_reap_wait{200};
};

class TransferManager {
public:
    using Options = TransferManagerOptions;

    TransferManager(executor::Executor& executor, AppStateOwner& state_owner,
        aki::heyaki::HeyakiAdapter& adapter, TransferManagerOptions options = {})
        : options_(std::move(options)),
          executor_(executor),
          state_owner_(state_owner),
          adapter_(adapter),
          pump_(executor, options_.pump,
              [this](TransferManagerWork& work) { return handle(work); }) {}

    TransferManager(const TransferManager&) = delete;
    TransferManager& operator=(const TransferManager&) = delete;

    // ---- Sink 路由入口 ----

    [[nodiscard]] bool enqueue_transfer_started(aki::transfer::Transfer transfer) {
        return pump_.enqueue(TransferStartedWork{std::move(transfer)});
    }

    [[nodiscard]] bool enqueue_transfer_progress(aki::transfer::TransferId transfer,
        std::uint64_t transferred, std::uint64_t total) {
        return pump_.enqueue(
            TransferProgressWork{std::move(transfer), transferred, total});
    }

    [[nodiscard]] bool enqueue_transfer_completed(aki::transfer::TransferId transfer,
        aki::transfer::TransferState final_state) {
        return pump_.enqueue(
            TransferCompletedWork{std::move(transfer), final_state});
    }

    // ---- 本域出站操作（传输四接口，设计第 7/8.1 节）----

    // 应用发起传输：本地记录 Queued → Adapter admission；成功则派生可取消会话
    // 任务（TaskHandle 按 TransferId 归本 Manager 持有，EXEC-07）。同 id 在飞
    // 会话已存在时拒绝（返回 false）：不替换旧会话记录（其句柄与 future 归属
    // 不变）。
    [[nodiscard]] bool start_transfer(aki::device::DeviceId to,
        aki::transfer::TransferId transfer_id, aki::transfer::FileMetadata file) {
        return pump_.enqueue(
            StartTransferWork{std::move(to), std::move(transfer_id), std::move(file)});
    }

    [[nodiscard]] bool pause_transfer(aki::transfer::TransferId transfer_id) {
        return pump_.enqueue(PauseTransferWork{std::move(transfer_id)});
    }

    [[nodiscard]] bool resume_transfer(aki::transfer::TransferId transfer_id) {
        return pump_.enqueue(ResumeTransferWork{std::move(transfer_id)});
    }

    // 协作取消：Adapter 侧 + Executor 侧（request_task_cancel）双通道；会话
    // future 移入回收队列，由排空上下文消费（终态结算不丢，AGENTS 规则 3）。
    [[nodiscard]] bool cancel_transfer(aki::transfer::TransferId transfer_id) {
        return pump_.enqueue(CancelTransferWork{std::move(transfer_id)});
    }

    [[nodiscard]] bool request_cancel_all() {
        return pump_.enqueue(CancelAllSessionsWork{});
    }

    // 有界等待：存量工作排空 + 在飞会话取消并回收（future 已消费）。宿主关闭
    // 钩子使用；预算耗尽返回 false（证据不伪造）。
    [[nodiscard]] bool flush(std::chrono::milliseconds budget) {
        auto done = std::make_shared<std::promise<void>>();
        auto finished = done->get_future();
        if (!pump_.enqueue(ReapSessionsWork{std::move(done)})) {
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + budget;
        if (finished.wait_until(deadline) != std::future_status::ready) {
            return false;
        }
        finished.get();  // 消费哨兵 future（纪律：保留并消费）。
        return pump_.flush(std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()));
    }

    // ---- 观测（EXEC-06；跨上下文诊断）----

    [[nodiscard]] int active_session_count() const noexcept {
        return active_sessions_.load();
    }

    [[nodiscard]] std::uint64_t cancelled_session_count() const noexcept {
        return cancelled_sessions_.load();
    }

    [[nodiscard]] ManagerPumpStats::Snapshot stats() const noexcept {
        return pump_.stats();
    }

private:
    struct SessionRecord {
        executor::TaskHandle handle;
        std::future<TransferSessionOutcome> future;
    };

    bool handle(TransferManagerWork& work) {
        reap_settled_sessions();  // 排空上下文内的非阻塞回收。
        return std::visit([this](auto& item) { return handle(item); }, work);
    }

    bool handle(TransferStartedWork& work) {
        if (work.transfer.id.empty()) {
            return false;
        }
        const bool posted = post_event(TransferStartedEvent{work.transfer});
        const bool applied = state_owner_.submit_update(UpsertTransfer{work.transfer});
        return posted && applied;
    }

    bool handle(TransferProgressWork& work) {
        if (work.transfer.empty()) {
            return false;
        }
        const bool posted =
            post_event(TransferProgressEvent{work.transfer, work.transferred, work.total});
        const bool applied = state_owner_.submit_update(
            UpdateTransferProgress{work.transfer, work.transferred, work.total});
        return posted && applied;
    }

    bool handle(TransferCompletedWork& work) {
        if (work.transfer.empty() || !aki::transfer::is_terminal(work.final_state)) {
            return false;  // final_state 仅取终态（设计第 10.1 节）。
        }
        const bool posted =
            post_event(TransferCompletedEvent{work.transfer, work.final_state});
        const bool applied = state_owner_.submit_update(
            CompleteTransfer{work.transfer, work.final_state});
        return posted && applied;
    }

    bool handle(StartTransferWork& work) {
        if (work.to.empty() || work.transfer_id.empty() || work.file.name.empty()) {
            return false;
        }
        if (sessions_.find(work.transfer_id.value) != sessions_.end()) {
            // 重复 TransferId：拒绝 admission（经 handler_rejections 可见，
            // RULE-09）。不替换旧会话记录——替换会丢弃未消费的 future（AGENTS
            // 规则 3）并留下不可取消的常驻会话任务（active_sessions_ 永不归零，
            // 关闭不再干净，EXEC-01 步骤 4）。已取消会话已移出本表（归回收
            // 队列），同 id 重启不受此守卫限制。
            return false;
        }
        aki::transfer::Transfer transfer;
        transfer.id = work.transfer_id;
        transfer.sender = options_.sender;
        transfer.receiver = work.to;
        transfer.file = work.file;
        transfer.state = aki::transfer::TransferState::Queued;
        if (!adapter_.start_file_transfer(work.to, work.transfer_id, work.file)) {
            transfer.state = aki::transfer::TransferState::Failed;
            return state_owner_.submit_update(UpsertTransfer{std::move(transfer)});
        }
        if (!state_owner_.submit_update(UpsertTransfer{std::move(transfer)})) {
            return false;  // 本地记录被拒：不派生会话（admission 失败可见）。
        }
        auto submission = executor_.submit_cancellable(
            [this](executor::StopToken stop_token) {
                return session_loop(std::move(stop_token));
            });
        if (!submission.handle.valid()) {
            return false;  // 提交即拒：会话未派生（拒绝可见，RULE-09）。
        }
        sessions_.insert_or_assign(work.transfer_id.value,
            SessionRecord{submission.handle, std::move(submission.future)});
        return true;
    }

    bool handle(PauseTransferWork& work) {
        return adapter_.pause_transfer(work.transfer_id);
    }

    bool handle(ResumeTransferWork& work) {
        return adapter_.resume_transfer(work.transfer_id);
    }

    bool handle(CancelTransferWork& work) {
        adapter_.cancel_transfer(work.transfer_id);  // SPI：幂等停止。
        const auto it = sessions_.find(work.transfer_id.value);
        if (it == sessions_.end()) {
            return true;  // 无在飞会话（未启动或已回收）：幂等。
        }
        request_session_cancel(it->second);
        reap_queue_.push_back(std::move(it->second));
        sessions_.erase(it);
        return true;
    }

    bool handle(CancelAllSessionsWork&) {
        for (auto& [id, record] : sessions_) {
            request_session_cancel(record);
            reap_queue_.push_back(std::move(record));
        }
        sessions_.clear();
        return true;
    }

    bool handle(ReapSessionsWork& work) {
        reap_sessions_blocking(options_.session_reap_wait);
        if (work.done) {
            work.done->set_value();
        }
        return true;
    }

    void request_session_cancel(const SessionRecord& record) {
        const auto response = executor_.request_task_cancel(record.handle);
        // accepted() 含幂等重复请求；AlreadyCompleted/NotFound 是过期句柄的
        // 预期结果，终态结算统一由回收队列的 future 消费完成（不静默、不
        // 重复请求）。
        (void)response;
    }

    // 会话任务（M1 协作取消骨架，M4 替换为真实分块传输循环）：executor 注入
    // StopToken 首参数；轮询停止请求自行退出——request_task_cancel 不抢占、
    // 不会打断无 wakeup 的阻塞调用。
    TransferSessionOutcome session_loop(executor::StopToken stop_token) {
        const SessionActiveGuard guard{active_sessions_};
        while (!stop_token.stop_requested()) {
            std::this_thread::sleep_for(options_.session_poll_interval);
        }
        return TransferSessionOutcome::CancelledByRequest;
    }

    struct SessionActiveGuard {
        std::atomic<int>& count;
        explicit SessionActiveGuard(std::atomic<int>& target) : count(target) {
            count.fetch_add(1);
        }
        ~SessionActiveGuard() { count.fetch_sub(1); }
        SessionActiveGuard(const SessionActiveGuard&) = delete;
        SessionActiveGuard& operator=(const SessionActiveGuard&) = delete;
    };

    // 非阻塞回收：已就绪的会话 future 消费结算（终态不丢，AGENTS 规则 3）。
    void reap_settled_sessions() {
        while (!reap_queue_.empty()
            && reap_queue_.front().future.wait_for(std::chrono::seconds(0))
                == std::future_status::ready) {
            consume_session_future(std::move(reap_queue_.front().future));
            reap_queue_.pop_front();
        }
    }

    // 有界回收（flush 哨兵路径）：等待在飞会话退出并消费 future。
    void reap_sessions_blocking(std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (!reap_queue_.empty()) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::milliseconds(0)) {
                return;  // 预算耗尽：存量保留在队列，后续 drain/flush 继续回收。
            }
            auto& front = reap_queue_.front();
            if (front.future.wait_for(remaining) != std::future_status::ready) {
                return;
            }
            consume_session_future(std::move(front.future));
            reap_queue_.pop_front();
        }
    }

    void consume_session_future(std::future<TransferSessionOutcome> future) {
        try {
            const auto outcome = future.get();
            if (outcome == TransferSessionOutcome::CancelledByRequest) {
                cancelled_sessions_.fetch_add(1);
            }
        } catch (const executor::TaskCancelled&) {
            // 排队期取消：会话从未运行即被结算（TaskCancelled(Explicit)），
            // 属请求成功的正常终相，计入取消计数。
            cancelled_sessions_.fetch_add(1);
        }
    }

    template <typename Payload>
    bool post_event(Payload payload) {
        AppEvent event;
        event.payload = std::move(payload);
        return state_owner_.post_event(std::move(event));
    }

    Options options_;
    executor::Executor& executor_;
    AppStateOwner& state_owner_;
    aki::heyaki::HeyakiAdapter& adapter_;
    std::map<std::string, SessionRecord> sessions_;  // 仅排空上下文访问。
    std::deque<SessionRecord> reap_queue_;           // 仅排空上下文访问。
    std::atomic<int> active_sessions_{0};
    std::atomic<std::uint64_t> cancelled_sessions_{0};
    ManagerPump<TransferManagerWork> pump_;
};

}  // namespace aki::app
