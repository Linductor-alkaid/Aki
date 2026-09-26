// Transfer Manager（设计第 8.3/7.1 节，DEC-008；M1-05 → M4-04 重构，
// DEC-011）。
//
// 只写 transfers Store：started/progress/completed 事件 → UpsertTransfer /
// UpdateTransferProgress / CompleteTransfer（终态幂等，迟到事件不复活，RULE-08）。
// 传输四接口是本域出站操作（→ Adapter）。
//
// 会话承载（M4-04 定案，DEC-011/§7.1①③）：**事件驱动会话状态机，无池上
// 会话长任务**——wire 侧状态由 heyaki 文件事件（经 RouterSink 入泵）驱动；
// Aki 侧归档（发送前流式 SHA-256 → source 拷贝 files/tmp/<id>.part）经注入的
// TransferIo 承载面（组合根以 aki.transfer-io blocking worker 实现）分块执行：
// 每会话单飞（一次一个在飞分块作业，完成事件回到泵后续接下一块），进度经
// 每会话最新进度槽 + 单飞 dirty 工作项聚合（每次排空至多一个
// UpdateTransferProgress，§7.1③），终态闸门——`CompleteTransfer(Completed)`
// 仅在归档完成后放行（M2-06 终态作业组对不完整 .part 明确失败）；
// `Failed`/`Cancelled` 终态不等待归档（在飞块结束后由 FIFO cancel 作业幂等
// 清理）。取消经「会话控制位（closing/paused，块间检查）+ io cancel 作业 +
// worker StopToken」表达，不经 request_task_cancel（无池任务句柄）。
//
// hash-first（§7.1④）：发送前 SHA-256 分块流先行；`stored_sha256` 随消息载荷
// 携带的调用方经 start_transfer 的 on_hash_ready 延续在 hash 完成后（泵上下文）
// 被触发；归档失败/无 io 承载时以空 hash 触发（调用方决定不发消息）。
//
// 线程契约与生命周期（EXEC-02/04/07）：会话表/进度槽只在排空上下文访问；
// IO 事件经 worker 线程的有界投递面回到本泵收件箱（收件箱满时短退避重试，
// 仍失败计数可见）；**本对象必须先于其 IO 事件回调终结**——flush() 先等
// IO 在飞归零、再做最终泵排空（两序皆满足才返回 true，有界预算），析构
// 兜底 request_stop + 有界等待；组合根关闭序为「flush（含 IO 归零）→
// owner EXEC-01 步骤 2/3 回收 worker」（§11.1③）。
#pragma once

#include "app/application/manager_runtime.hpp"
#include "app/state/app_events.hpp"
#include "app/state/app_state_owner.hpp"
#include "app/state/app_state_updates.hpp"
#include "device/device/device_types.hpp"
#include "heyaki/adapter/heyaki_adapter.hpp"
#include "transfer/storage/transfer_io.hpp"
#include "transfer/transfer/transfer_types.hpp"

#include <executor/executor.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
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
    // 发送侧本地路径（§7.1⑤）：随出站 SPI 传 Adapter（wire 侧 heyaki 自读），
    // 同时是 Aki 归档拷贝的源。
    std::filesystem::path source_path;
    // hash-first 延续（§7.1④/DEC-011）：hash 完成后于泵上下文以 64 字符
    // hex 触发；归档失败/无 io 承载时以空串触发（调用方决定不发消息）。
    std::function<void(std::string)> on_hash_ready;
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

// 传输暂停（M4-05，§8.1 第 11 方法/DEC-012④）：对端驱动/本地暂停确认。
struct TransferPausedWork {
    aki::transfer::TransferId transfer;
};

// IO 完成事件（worker 线程 → 本泵收件箱的有界投递面）。
struct IoEventWork {
    aki::transfer::TransferIoEvent event;
};

// 进度聚合的单飞 dirty 工作项（每会话至多一个在飞：每次排空至多一个
// UpdateTransferProgress，§7.1③）。
struct ProgressFlushWork {
    aki::transfer::TransferId transfer;
};

// 关闭钩子用：终态化全部会话（取消归档链路 + 丢弃在飞续接）。
struct CancelAllSessionsWork {};

// flush 哨兵：确认泵静止且 IO 在飞归零后通知调用方。
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
    TransferPausedWork,
    IoEventWork,
    ProgressFlushWork,
    CancelAllSessionsWork,
    ReapSessionsWork>;

// 构造选项置于命名空间作用域（同 AppStateOwnerOptions 处理，GCC 纪律）。
struct TransferManagerOptions {
    ManagerPumpOptions pump{};
    aki::device::DeviceId sender;  // 应用发起传输的 sender（本机设备）。
    // 归档 IO 承载面（组合根注入；null = 无归档链路——会话仅 wire 侧事件
    // 驱动，测试/无存储配置形态）。生命周期覆盖本对象全程。
    aki::transfer::TransferIo* io = nullptr;
    // IO 事件投递满时的有界重试预算（worker 线程退避；泵持续排空下饱和
    // 不可达，仍失败计数可见——RULE-09）。
    std::chrono::milliseconds io_delivery_budget{2000};
};

class TransferManager {
public:
    using Options = TransferManagerOptions;

    TransferManager(executor::Executor& executor, AppStateOwner& state_owner,
        aki::heyaki::HeyakiAdapter& adapter, TransferManagerOptions options = {})
        : options_(std::move(options)),
          state_owner_(state_owner),
          adapter_(adapter),
          pump_(executor, options_.pump,
              [this](TransferManagerWork& work) { return handle(work); }) {
        if (options_.io != nullptr) {
            // IO 事件投递面（worker 线程上下文 → 本泵收件箱；装配序：worker
            // 启动前注册，组合根保证）。
            options_.io->set_event_sink(
                [this](const aki::transfer::TransferIoEvent& event) {
                    deliver_io_event(event);
                });
        }
    }

    TransferManager(const TransferManager&) = delete;
    TransferManager& operator=(const TransferManager&) = delete;

    // 兜底闭合（组合根关闭序的第一责任人仍是宿主，§11.1③）：IO 在飞未
    // 归零时请求 worker 协作停止并等待回调结算——本对象析构后不得再有
    // IO 事件回调触碰本对象（DEC-011 ③）。
    ~TransferManager() {
        if (options_.io != nullptr && !options_.io->idle()) {
            options_.io->request_stop();
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!options_.io->idle()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    break;  // 预算耗尽：如实放弃等待（违反关闭序的组合根缺陷）
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

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

    // 传输暂停（M4-05）：UpsertTransfer(Paused) 经已知行缓存承载；发送会话
    // 同时抑制归档续接（对端驱动暂停，§7.1①）。
    [[nodiscard]] bool enqueue_transfer_paused(
        aki::transfer::TransferId transfer) {
        return pump_.enqueue(TransferPausedWork{std::move(transfer)});
    }

    // ---- 本域出站操作（传输四接口，设计第 7/8.1 节）----

    // 应用发起传输：本地记录 Queued → Adapter admission（wire 侧 push_file）；
    // 归档链路（hash → copy）经 IO 承载面分块驱动（M4-04）。同 id 会话已
    // 存在时拒绝（不替换旧会话记录）。source_path（§7.1⑤）经出站 SPI 传
    // Adapter 且为归档源。on_hash_ready 见 StartTransferWork（hash-first）。
    [[nodiscard]] bool start_transfer(aki::device::DeviceId to,
        aki::transfer::TransferId transfer_id, aki::transfer::FileMetadata file,
        std::filesystem::path source_path = {},
        std::function<void(std::string)> on_hash_ready = {}) {
        return pump_.enqueue(StartTransferWork{std::move(to),
            std::move(transfer_id), std::move(file), std::move(source_path),
            std::move(on_hash_ready)});
    }

    [[nodiscard]] bool pause_transfer(aki::transfer::TransferId transfer_id) {
        return pump_.enqueue(PauseTransferWork{std::move(transfer_id)});
    }

    [[nodiscard]] bool resume_transfer(aki::transfer::TransferId transfer_id) {
        return pump_.enqueue(ResumeTransferWork{std::move(transfer_id)});
    }

    // 协作取消：Adapter 侧 + 会话收尾（续接抑制 + 归档 FIFO cancel 作业：
    // 在飞块完成后幂等清理 .part，§7.1①）。
    [[nodiscard]] bool cancel_transfer(aki::transfer::TransferId transfer_id) {
        return pump_.enqueue(CancelTransferWork{std::move(transfer_id)});
    }

    [[nodiscard]] bool request_cancel_all() {
        return pump_.enqueue(CancelAllSessionsWork{});
    }

    // 有界等待：泵静止 + IO 在飞归零（本对象终结前置条件）。宿主关闭钩子
    // 使用；预算耗尽返回 false（证据不伪造）。
    [[nodiscard]] bool flush(std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        for (;;) {
            // 先判 IO 归零：worker 的 in_flight 在 sink 投递（即 IoEventWork
            // 已入箱）之后才递减——idle 为真 ⇒ 既有 IO 事件已全部入箱且无
            // 在飞作业。若先泵静止后判 idle，两检查之间落入的 IO 事件会以
            // 未处理状态滞留箱内却返回 true（TOCTOU）。
            if (options_.io != nullptr && !options_.io->idle()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const auto remaining =
                deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::milliseconds(0)
                || !pump_.flush(std::chrono::duration_cast<
                      std::chrono::milliseconds>(remaining))) {
                return false;
            }
            // 泵排空期间处理 IoEventWork 可派生新 IO 作业（continue_archive）
            // ——失去 idle 时重走循环；idle 复判为真时无在飞作业亦无待投递
            // 事件可再落入两检查之间（新作业只可能由泵 handler 派生，而泵
            // 已静止），此时返回 true 才满足「泵静止且无未回调 IO 作业」。
            if (options_.io == nullptr || options_.io->idle()) {
                return true;
            }
        }
    }

    // ---- 观测（EXEC-06；跨上下文诊断）----

    [[nodiscard]] int active_session_count() const noexcept {
        return session_count_.load();
    }

    [[nodiscard]] std::uint64_t cancelled_session_count() const noexcept {
        return cancelled_sessions_.load();
    }

    // IO 承载面 admission 拒绝（通道满/已停止——RULE-09 可观测）。
    [[nodiscard]] std::uint64_t io_rejected_submissions() const noexcept {
        return options_.io != nullptr ? options_.io->rejected_submissions() : 0;
    }

    // 迟到 IO 事件（会话已终结清理后到达——幂等忽略，计数可见）。
    [[nodiscard]] std::uint64_t late_io_events() const noexcept {
        return late_io_events_.load();
    }

    // IO 事件投递重试预算耗尽（收件箱持续满——会话停摆信号，RULE-09）。
    [[nodiscard]] std::uint64_t io_events_dropped() const noexcept {
        return io_events_dropped_.load();
    }

    // 进度 flush 单飞项入箱拒绝（TM 收件箱满——RULE-09 可观测，不吞掉）。
    // 拒绝时不置 dirty（单飞不变式保持），最新槽随下一进度事件重试投递。
    [[nodiscard]] std::uint64_t progress_flush_rejections() const noexcept {
        return progress_flush_rejections_.load();
    }

    [[nodiscard]] ManagerPumpStats::Snapshot stats() const noexcept {
        return pump_.stats();
    }

private:
    // 发送会话（仅排空上下文访问；事件驱动状态机，DEC-011①）。
    struct SendSession {
        bool has_io = false;          // 归档链路启用（io 承载面注入且 start 成功）
        bool io_in_flight = false;    // 每会话单飞：一个在飞分块作业
        bool closing = false;         // 取消/终态后抑制续接
        bool paused = false;          // 暂停：续接抑制（resume 放行）
        bool archive_complete = false;
        bool archive_failed = false;
        std::function<void(std::string)> on_hash_ready;  // hash-first 延续
        std::optional<TransferCompletedWork> held_completed;  // 终态闸门
        std::uint64_t progress_latest = 0;  // 最新进度槽（wire/archive 共用）
        std::uint64_t progress_total = 0;
        bool progress_dirty = false;        // 单飞 dirty 标记
    };

    bool handle(TransferManagerWork& work) {
        return std::visit([this](auto& item) { return handle(item); }, work);
    }

    bool handle(TransferStartedWork& work) {
        if (work.transfer.id.empty()) {
            return false;
        }
        // 已知行（发送行/已建入站行）：同态重复幂等去重（wire probing/offered
        // 重复事件不重放主路径事件）；状态推进（如 Queued → Negotiating）以
        // 已知行数据合并承载（不覆盖既有 metadata，DEC-012④）。
        auto known = known_rows_.find(work.transfer.id.value);
        if (known != known_rows_.end()) {
            if (known->second.state == work.transfer.state) {
                return true;  // 同态重复：幂等去重（无新事件/更新）
            }
            if (aki::transfer::can_transition(known->second.state,
                    work.transfer.state)) {
                aki::transfer::Transfer advanced = known->second;
                advanced.state = work.transfer.state;
                advanced.transferred = work.transfer.transferred;
                advanced.total = work.transfer.total;
                const bool posted =
                    post_event(TransferStartedEvent{advanced});
                const bool applied =
                    state_owner_.submit_update(UpsertTransfer{advanced});
                if (applied) {
                    known->second = advanced;
                }
                return posted && applied;
            }
            // 非法边（如终态后迟到 started）：交由 owner 状态机拒绝可见。
        } else {
            known_rows_.emplace(work.transfer.id.value, work.transfer);
        }
        const bool posted = post_event(TransferStartedEvent{work.transfer});
        const bool applied = state_owner_.submit_update(UpsertTransfer{work.transfer});
        return posted && applied;
    }

    bool handle(TransferProgressWork& work) {
        if (work.transfer.empty()) {
            return false;
        }
        auto it = sessions_.find(work.transfer.value);
        if (it == sessions_.end()) {
            // 非会话路径（入站 wire/inject 直达，M4-05）：首个进度事件把行
            // 自 Negotiating/Paused 推进到 Transferring（§7.1⑤——整行 upsert
            // 承载状态+进度，已知行数据合并）；已在 Transferring 时为部分列
            // 更新。未知行不建行（UpdateTransferProgress 不建行，DEC-012②）。
            auto known = known_rows_.find(work.transfer.value);
            if (known != known_rows_.end()
                && (known->second.state
                        == aki::transfer::TransferState::Negotiating
                    || known->second.state
                        == aki::transfer::TransferState::Paused)) {
                aki::transfer::Transfer advanced = known->second;
                advanced.state = aki::transfer::TransferState::Transferring;
                advanced.transferred = work.transferred;
                advanced.total = work.total;
                const bool posted = post_event(
                    TransferProgressEvent{work.transfer, work.transferred,
                        work.total});
                const bool applied =
                    state_owner_.submit_update(UpsertTransfer{advanced});
                if (applied) {
                    known->second = advanced;
                }
                return posted && applied;
            }
            if (known != known_rows_.end()) {
                known->second.transferred = work.transferred;
                known->second.total = work.total;
            }
            const bool posted = post_event(
                TransferProgressEvent{work.transfer, work.transferred, work.total});
            const bool applied = state_owner_.submit_update(
                UpdateTransferProgress{work.transfer, work.transferred,
                    work.total});
            return posted && applied;
        }
        // 会话路径：最新槽 + 单飞 dirty（每次排空至多一个进度更新，§7.1③）。
        it->second.progress_latest = work.transferred;
        it->second.progress_total = work.total;
        mark_progress_dirty(work.transfer, it->second);
        return true;
    }

    bool handle(TransferCompletedWork& work) {
        if (work.transfer.empty() || !aki::transfer::is_terminal(work.final_state)) {
            return false;  // final_state 仅取终态（设计第 10.1 节）。
        }
        auto it = sessions_.find(work.transfer.value);
        if (it != sessions_.end()) {
            SendSession& session = it->second;
            if (work.final_state == aki::transfer::TransferState::Completed
                && session.has_io && !session.archive_complete
                && !session.archive_failed) {
                // 终态闸门（§7.1③）：wire committed 先于归档完成——持有事件，
                // 归档 copy_done 后放行（M2-06 作业组对不完整 .part 明确失败）。
                session.held_completed = work;
                return true;
            }
            if (work.final_state == aki::transfer::TransferState::Completed) {
                // 归档已完整（或无归档链路）：清理 worker 侧状态但保留
                // .part——M2-06 终态作业组随后消费（提前丢弃会使作业组
                // 以「.part 缺失」明确失败）。
                if (session.has_io) {
                    (void)options_.io->release(work.transfer);
                }
            } else {
                finish_session_io(session, work.transfer);
            }
            sessions_.erase(it);
            session_count_.store(static_cast<int>(sessions_.size()));
        }
        return deliver_terminal(work);
    }

    bool handle(StartTransferWork& work) {
        if (work.to.empty() || work.transfer_id.empty() || work.file.name.empty()) {
            return false;
        }
        if (sessions_.find(work.transfer_id.value) != sessions_.end()) {
            // 重复 TransferId：拒绝 admission（经 handler_rejections 可见，
            // RULE-09）。不替换旧会话记录。
            return false;
        }
        aki::transfer::Transfer transfer;
        transfer.id = work.transfer_id;
        transfer.sender = options_.sender;
        transfer.receiver = work.to;
        transfer.file = work.file;
        transfer.total = work.file.size_bytes;
        transfer.state = aki::transfer::TransferState::Queued;
        if (!adapter_.start_file_transfer(work.to, work.transfer_id,
                work.file, work.source_path)) {
            transfer.state = aki::transfer::TransferState::Failed;
            return state_owner_.submit_update(UpsertTransfer{std::move(transfer)});
        }
        if (!state_owner_.submit_update(UpsertTransfer{transfer})) {
            return false;  // 本地记录被拒：不建会话（admission 失败可见）。
        }
        known_rows_[work.transfer_id.value] = transfer;  // 发送行建档（M4-05）
        SendSession session;
        session.has_io = options_.io != nullptr;
        if (options_.io != nullptr) {
            if (options_.io->start(work.transfer_id, work.source_path,
                    work.file.name, work.file.size_bytes)) {
                session.io_in_flight = true;  // 首个 hash 分块在飞
            } else {
                // 归档 admission 拒绝（通道满/停止）：计数可见（io_rejected）；
                // wire 侧照常（正交，§6.1②）——hash-first 延续以空 hash 触发。
                session.archive_failed = true;
                session.has_io = false;
            }
        }
        const bool direct_empty_hash = !session.has_io && work.on_hash_ready != nullptr;
        if (work.on_hash_ready != nullptr && session.has_io) {
            session.on_hash_ready = std::move(work.on_hash_ready);
        }
        sessions_.emplace(work.transfer_id.value, std::move(session));
        session_count_.store(static_cast<int>(sessions_.size()));
        if (direct_empty_hash) {
            // 无归档承载/准入失败：空 hash 直通（调用方决定不发消息）。
            // 注：回调只入队其他 Manager/本泵，不再触碰本会话记录。
            auto on_ready = work.on_hash_ready;
            on_ready("");
        }
        return true;
    }

    bool handle(PauseTransferWork& work) {
        auto it = sessions_.find(work.transfer_id.value);
        if (it != sessions_.end()) {
            it->second.paused = true;  // 归档续接抑制（在飞块自然完成）
        }
        return adapter_.pause_transfer(work.transfer_id);
    }

    bool handle(ResumeTransferWork& work) {
        auto it = sessions_.find(work.transfer_id.value);
        if (it != sessions_.end()) {
            SendSession& session = it->second;
            session.paused = false;
            if (session.has_io && !session.io_in_flight
                && !session.archive_complete && !session.archive_failed
                && !session.closing) {
                continue_archive(work.transfer_id, session);
            }
        }
        return adapter_.resume_transfer(work.transfer_id);
    }

    bool handle(CancelTransferWork& work) {
        adapter_.cancel_transfer(work.transfer_id);  // SPI：幂等停止。
        auto it = sessions_.find(work.transfer_id.value);
        if (it == sessions_.end()) {
            return true;  // 无会话（未启动或已终结）：幂等。
        }
        finish_session_io(it->second, work.transfer_id);
        sessions_.erase(it);
        session_count_.store(static_cast<int>(sessions_.size()));
        cancelled_sessions_.fetch_add(1);
        return true;
    }

    bool handle(TransferPausedWork& work) {
        if (work.transfer.empty()) {
            return false;
        }
        // 发送会话：抑制归档续接（对端驱动暂停，§7.1①；本地暂停命令在
        // handle(PauseTransferWork) 已置位，此处幂等）。
        auto session = sessions_.find(work.transfer.value);
        if (session != sessions_.end()) {
            session->second.paused = true;
        }
        // 整行 upsert（Paused）：已知行缓存承载（§7.1②——状态推进经
        // UpsertTransfer；不新增 AppEvent 主路径类型）。
        auto known = known_rows_.find(work.transfer.value);
        if (known == known_rows_.end()) {
            // 未知行的暂停（迟到/无行）：状态机将拒绝可见——构造最小行交由
            // owner 拒绝（RULE-09），不静默。
            aki::transfer::Transfer row;
            row.id = work.transfer;
            row.state = aki::transfer::TransferState::Paused;
            return state_owner_.submit_update(UpsertTransfer{row});
        }
        if (known->second.state == aki::transfer::TransferState::Paused) {
            return true;  // 同态重复：幂等
        }
        if (!aki::transfer::can_transition(known->second.state,
                aki::transfer::TransferState::Paused)) {
            // 非法边（终态后迟到暂停）：交由 owner 状态机拒绝可见。
            aki::transfer::Transfer rejected = known->second;
            rejected.state = aki::transfer::TransferState::Paused;
            return state_owner_.submit_update(UpsertTransfer{rejected});
        }
        aki::transfer::Transfer paused = known->second;
        paused.state = aki::transfer::TransferState::Paused;
        const bool applied = state_owner_.submit_update(UpsertTransfer{paused});
        if (applied) {
            known->second = paused;
        }
        return applied;
    }

    bool handle(IoEventWork& work) {
        const aki::transfer::TransferIoEvent& event = work.event;
        auto it = sessions_.find(event.transfer.value);
        if (it == sessions_.end()) {
            if (event.phase
                == aki::transfer::TransferIoEvent::Phase::released) {
                return true;  // Completed 终态后的正常清理回报（不计迟到）
            }
            late_io_events_.fetch_add(1);  // 迟到事件（会话已清理）：幂等忽略
            return true;
        }
        SendSession& session = it->second;
        session.io_in_flight = false;
        switch (event.phase) {
            case aki::transfer::TransferIoEvent::Phase::hash_progress:
                continue_archive(event.transfer, session);
                return true;
            case aki::transfer::TransferIoEvent::Phase::hash_done:
                if (session.on_hash_ready != nullptr) {
                    // hash-first 延续（泵上下文，§7.1④）：stored_sha256 已知，
                    // 调用方在此发送消息（图片流 v2）。
                    auto on_ready = std::move(session.on_hash_ready);
                    session.on_hash_ready = nullptr;
                    on_ready(event.hash_hex);
                }
                continue_archive(event.transfer, session);
                return true;
            case aki::transfer::TransferIoEvent::Phase::copy_progress:
                session.progress_latest = event.bytes_done;
                session.progress_total = event.bytes_total;
                mark_progress_dirty(event.transfer, session);
                continue_archive(event.transfer, session);
                return true;
            case aki::transfer::TransferIoEvent::Phase::copy_done:
                session.archive_complete = true;
                session.progress_latest = event.bytes_done;
                session.progress_total = event.bytes_total;
                mark_progress_dirty(event.transfer, session);
                if (session.held_completed.has_value()) {
                    // 终态闸门放行（§7.1③）：归档完成 + wire committed——
                    // release（保留 .part 给终态作业组）。
                    TransferCompletedWork held = std::move(*session.held_completed);
                    if (session.has_io) {
                        (void)options_.io->release(event.transfer);
                    }
                    sessions_.erase(it);
                    session_count_.store(static_cast<int>(sessions_.size()));
                    return deliver_terminal(held);
                }
                return true;
            case aki::transfer::TransferIoEvent::Phase::failed:
                // 归档失败：wire 侧不受影响（正交）；held 终态释放为已知
                // 边角（M2-06 作业组对不完整 .part 明确失败可见，DEC-011）。
                session.archive_failed = true;
                if (session.on_hash_ready != nullptr) {
                    auto on_ready = std::move(session.on_hash_ready);
                    session.on_hash_ready = nullptr;
                    on_ready("");  // 空 hash：调用方决定不发消息
                }
                if (session.held_completed.has_value()) {
                    TransferCompletedWork held = std::move(*session.held_completed);
                    sessions_.erase(it);
                    session_count_.store(static_cast<int>(sessions_.size()));
                    return deliver_terminal(held);
                }
                return true;
            case aki::transfer::TransferIoEvent::Phase::cancelled:
                sessions_.erase(it);
                session_count_.store(static_cast<int>(sessions_.size()));
                return true;
            case aki::transfer::TransferIoEvent::Phase::released:
                // Completed 终态的 worker 侧清理回报（正常路径，不计迟到）。
                sessions_.erase(it);
                session_count_.store(static_cast<int>(sessions_.size()));
                return true;
        }
        return false;
    }

    bool handle(ProgressFlushWork& work) {
        auto it = sessions_.find(work.transfer.value);
        if (it == sessions_.end()) {
            return true;  // 会话已终结：进度槽随会话丢弃
        }
        SendSession& session = it->second;
        if (!session.progress_dirty) {
            return true;  // 已被先前 flush 消费
        }
        session.progress_dirty = false;
        const bool posted = post_event(TransferProgressEvent{work.transfer,
            session.progress_latest, session.progress_total});
        const bool applied = state_owner_.submit_update(
            UpdateTransferProgress{work.transfer, session.progress_latest,
                session.progress_total});
        return posted && applied;
    }

    bool handle(CancelAllSessionsWork&) {
        for (auto& [key, session] : sessions_) {
            session.closing = true;
            if (session.has_io) {
                // FIFO cancel 作业：在飞块完成后幂等清理（§7.1①）。
                (void)options_.io->cancel(aki::transfer::TransferId{key});
            }
        }
        cancelled_sessions_.fetch_add(sessions_.size());
        sessions_.clear();
        session_count_.store(0);
        return true;
    }

    bool handle(ReapSessionsWork& work) {
        // 泵静止 + IO 归零由 flush() 的外层循环保证（无池任务 future 可回收）。
        if (work.done) {
            work.done->set_value();
        }
        return true;
    }

    // ---- 会话推进（泵上下文）----

    void continue_archive(const aki::transfer::TransferId& id,
        SendSession& session) {
        if (session.closing || session.paused || session.archive_complete
            || session.archive_failed) {
            return;  // 续接抑制
        }
        if (options_.io->advance(id)) {
            session.io_in_flight = true;
        } else {
            // advance 拒绝（通道满/停止）：归档停摆——io_rejected 计数可见，
            // 会话标记 archive_failed（held 终态按已知边角释放）。
            session.archive_failed = true;
        }
    }

    // 会话收尾：续接抑制 + FIFO cancel 作业（在飞块完成后幂等清理 .part）。
    void finish_session_io(SendSession& session,
        const aki::transfer::TransferId& id) {
        session.closing = true;
        if (session.has_io) {
            (void)options_.io->cancel(id);
        }
    }

    // 终态投递（事件 + CompleteTransfer；held 放行与直达共用出口）。
    bool deliver_terminal(const TransferCompletedWork& work) {
        known_rows_.erase(work.transfer.value);  // 终态：行缓存清理（M4-05）
        const bool posted =
            post_event(TransferCompletedEvent{work.transfer, work.final_state});
        const bool applied = state_owner_.submit_update(
            CompleteTransfer{work.transfer, work.final_state});
        return posted && applied;
    }

    void mark_progress_dirty(const aki::transfer::TransferId& id,
        SendSession& session) {
        if (session.progress_dirty) {
            return;  // 单飞 dirty 项在箱：本批后续事件只更新最新槽
        }
        if (!pump_.enqueue(ProgressFlushWork{id})) {
            // 收件箱满：不置 dirty（单飞不变式——dirty ⇒ 箱内有 flush 项，
            // 否则后续事件全部提前返回、该会话进度永久停摆），拒绝计数可见
            //（RULE-09/AGENTS 规则 10，不吞掉）；最新槽保持，随下一次进度
            // 事件重试投递。
            progress_flush_rejections_.fetch_add(1);
            return;
        }
        session.progress_dirty = true;
    }

    // IO 事件投递面（worker 线程上下文；有界 + 不抛出——TransferIo 契约）。
    // 收件箱满时短退避重试（泵持续排空，饱和不可达）；预算耗尽计数可见。
    void deliver_io_event(const aki::transfer::TransferIoEvent& event) {
        const auto deadline =
            std::chrono::steady_clock::now() + options_.io_delivery_budget;
        for (;;) {
            if (pump_.enqueue(IoEventWork{event})) {
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                io_events_dropped_.fetch_add(1);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    template <typename Payload>
    bool post_event(Payload payload) {
        AppEvent event;
        event.payload = std::move(payload);
        return state_owner_.post_event(std::move(event));
    }

    Options options_;
    AppStateOwner& state_owner_;
    aki::heyaki::HeyakiAdapter& adapter_;
    std::map<std::string, SendSession> sessions_;  // 仅排空上下文访问。
    // 已知传输行缓存（仅排空上下文；M4-05）：入站 started 建档与发送行建档
    // 的行数据来源——paused 的整行 upsert 与 progress 的状态推进
    //（Negotiating/Paused → Transferring）经它构造；终态清理。
    std::map<std::string, aki::transfer::Transfer> known_rows_;
    std::atomic<int> session_count_{0};            // 跨上下文诊断镜像。
    std::atomic<std::uint64_t> cancelled_sessions_{0};
    std::atomic<std::uint64_t> late_io_events_{0};
    std::atomic<std::uint64_t> io_events_dropped_{0};
    std::atomic<std::uint64_t> progress_flush_rejections_{0};
    ManagerPump<TransferManagerWork> pump_;
};

}  // namespace aki::app
