// 传输归档 IO worker（设计 §7.1③/DEC-011；M4-04）。
//
// aki::transfer::TransferIo 承载面的实现：专用 blocking worker
//（名 aki.transfer-io，与 DatabaseWorker 同款 IBlockingIoWorker + 有界
// MpscChannel 作业通道形态，EXEC-04）上逐块执行发送侧 Aki 侧文件 IO——
// hash 相位（发送前流式 SHA-256，§7.1④）→ copy 相位（归档拷贝 source →
// files/tmp/<transfer_id>.part，FileStore::write_part 分块驱动）。
//
// 形态（DEC-011 ①）：
//   - 作业为 offset 基础的无状态分块（源文件句柄与 hash 上下文跨分块保持
//     在 worker 会话状态内——仅 worker 单线程访问）；
//   - 每会话单飞由调用方（TM 泵）保证：start/advance 一次一分块，完成事件
//     经 event_sink 回投后由泵续接；
//   - 通道有界满即拒绝（RULE-09）；等待为 try_receive + 短睡眠轮询
//    （pinned v0.5.0-7 receive_for 上游缺陷的既定备选，M2-05 注记），
//     停止响应上界 = 轮询间隔；
//   - 事件投递在 worker 线程、保证不抛出（异常在作业边界捕获转为 failed
//     事件）；调用方在此上下文只做有界投递（EXEC-02 纪律的对称面）；
//   - 关闭：request_stop 协作退出（在飞作业完成后存量清理，回调结算），
//     join 归 owner EXEC-01 步骤 2/3（§11.1③ 复用 DatabaseWorker 关闭纪律）。
//
// 结构（同 DatabaseWorkerControl/Runnable 的共享 Impl 形态，EXEC-07）：
//   - TransferIoControl：宿主持有的控制面（实现 TransferIo 承载面，
//     任意上下文线程安全）；
//   - TransferIoRunnable：实现 IBlockingIoWorker，归 executor facade
//    （注册后由其销毁），经共享 Impl 与控制面协作；会话状态（源句柄/
//     hash 上下文）为 runnable 私有，仅 run 线程访问。
//
// 层向：persistence 依赖 domain（既有方向，承载面为 aki::transfer::TransferIo）；
// FileStore/Sha256 为本层既有组件。RULE-10：本头是注册侧接线头（executor
// 类型允许存在于该层，同 database_worker_adapter.hpp 处理）。
#pragma once

#include "persistence/storage/file_store.hpp"
#include "persistence/storage/sha256.hpp"
#include "transfer/storage/transfer_io.hpp"

#include <executor/blocking_io.hpp>
#include <executor/comm/channel.hpp>
#include <executor/comm/types.hpp>
#include <executor/stop_token.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace aki::persistence {

// 构造选项（命名空间作用域，同仓库既有处理）。
struct TransferIoWorkerOptions {
    std::size_t channel_capacity = 64;           // RULE-09：作业通道预算
    std::size_t chunk_bytes = 256U * 1024U;      // 分块上限（对齐 heyaki 量级）
    std::chrono::milliseconds wait_timeout{10};  // run() 通道等待上界
};

class TransferIoRunnable;

// 宿主侧控制面（TransferIo 承载面；任意上下文线程安全）。
class TransferIoControl final : public aki::transfer::TransferIo {
public:
    struct Job {
        enum class Kind { open, advance, cancel, release };
        Kind kind;
        aki::transfer::TransferId transfer;
        std::filesystem::path source;
        std::string display_name;
        std::uint64_t total_bytes = 0;
    };

    // 控制面共享状态（宿主与 runnable 两端共享；EXEC-07）。
    struct Impl {
        Impl(std::shared_ptr<const FileStore> worker_store,
            TransferIoWorkerOptions worker_options)
            : store(std::move(worker_store)),
              options(worker_options),
              channel(executor::comm::ChannelOptions{
                  .capacity = worker_options.channel_capacity,
                  .enable_stats = true,
                  .name = "aki.transfer-io.jobs"}) {}

        std::shared_ptr<const FileStore> store;
        TransferIoWorkerOptions options;
        executor::comm::MpscChannel<Job> channel;
        std::function<void(const aki::transfer::TransferIoEvent&)> sink;
        std::mutex sink_mutex;
        std::atomic<bool> stop_requested{false};
        // 「提交成功但事件未回调完成」的作业数（idle 语义：消费方析构前的
        // 等待条件，DEC-011 ③）。
        std::atomic<std::uint64_t> in_flight{0};
        std::atomic<std::uint64_t> rejected{0};
    };

    explicit TransferIoControl(std::shared_ptr<const FileStore> store,
        TransferIoWorkerOptions options = {})
        : impl_(std::make_shared<Impl>(std::move(store), options)) {}

    TransferIoControl(const TransferIoControl&) = delete;
    TransferIoControl& operator=(const TransferIoControl&) = delete;

    // 注册侧接线（组合根；runnable 构造用）。
    [[nodiscard]] std::shared_ptr<Impl> impl() const noexcept {
        return impl_;
    }

    // ---- TransferIo 承载面 ----

    [[nodiscard]] bool start(const aki::transfer::TransferId& transfer,
        std::filesystem::path source, std::string display_name,
        std::uint64_t total_bytes) override {
        if (transfer.value.empty() || source.empty()
            || !FileStore::valid_transfer_id(transfer.value)) {
            return false;  // 有界校验（FileStore 路径入口校验：路径穿越在入口被拒）
        }
        return submit(Job{Job::Kind::open, transfer, std::move(source),
            std::move(display_name), total_bytes});
    }

    [[nodiscard]] bool advance(
        const aki::transfer::TransferId& transfer) override {
        return submit(Job{Job::Kind::advance, transfer, {}, {}, 0});
    }

    [[nodiscard]] bool cancel(
        const aki::transfer::TransferId& transfer) override {
        return submit(Job{Job::Kind::cancel, transfer, {}, {}, 0});
    }

    [[nodiscard]] bool release(
        const aki::transfer::TransferId& transfer) override {
        return submit(Job{Job::Kind::release, transfer, {}, {}, 0});
    }

    // 装配序约束：worker 启动前设置（组合根装配序）；运行期读写经 mutex。
    void set_event_sink(
        std::function<void(const aki::transfer::TransferIoEvent&)> sink)
        override {
        std::lock_guard<std::mutex> guard(impl_->sink_mutex);
        impl_->sink = std::move(sink);
    }

    void request_stop() noexcept override {
        impl_->stop_requested.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool idle() const noexcept override {
        return impl_->in_flight.load(std::memory_order_acquire) == 0;
    }

    [[nodiscard]] std::uint64_t rejected_submissions() const noexcept override {
        return impl_->rejected.load(std::memory_order_relaxed);
    }

private:
    [[nodiscard]] bool submit(Job&& job) {
        if (impl_->stop_requested.load(std::memory_order_acquire)) {
            impl_->rejected.fetch_add(1, std::memory_order_relaxed);
            return false;  // 已停止：admission 拒绝可见
        }
        // in_flight 在提交侧递增、事件回调返回后递减（runnable 侧）：
        // idle() == true 即「无未回调作业」。
        impl_->in_flight.fetch_add(1, std::memory_order_release);
        if (!impl_->channel.try_send(std::move(job))) {
            impl_->in_flight.fetch_sub(1, std::memory_order_release);
            impl_->rejected.fetch_add(1, std::memory_order_relaxed);
            return false;  // 通道满：拒绝可见（RULE-09）
        }
        return true;
    }

    std::shared_ptr<Impl> impl_;
};

// IBlockingIoWorker 适配器：单一 worker 串行消费作业通道 + 会话状态私有
//（EXEC-04；对象所有权归 executor facade，宿主仅经 TransferIoControl 交互）。
class TransferIoRunnable final : public executor::IBlockingIoWorker {
public:
    explicit TransferIoRunnable(std::shared_ptr<TransferIoControl::Impl> impl)
        : impl_(std::move(impl)) {}

    TransferIoRunnable(const TransferIoRunnable&) = delete;
    TransferIoRunnable& operator=(const TransferIoRunnable&) = delete;

    // 消费循环（DatabaseWorker 同款轮询环）：作业间检查停止请求/StopToken；
    // 退出路径清理存量会话（.part 幂等删除）并以 cancelled 事件结算未执行
    // 作业（回调不悬挂，RULE-09）。
    void run(executor::StopToken stop_token) override {
        for (;;) {
            if (impl_->stop_requested.load(std::memory_order_acquire)) {
                drain_and_cancel();
                return;
            }
            if (stop_token.stop_requested()) {
                drain_and_cancel();
                return;
            }
            TransferIoControl::Job job;
            if (impl_->channel.try_receive(job)) {
                execute(job);
                continue;
            }
            std::this_thread::sleep_for(impl_->options.wait_timeout);
        }
    }

    // 平凡 noexcept（通道等待为有界自旋，≤wait_timeout 内自解，结构性满足
    // §8.2 步骤 3 可解除阻塞契约）。
    void wakeup() noexcept override {}

private:
    // 会话状态（仅 run 线程访问；句柄与 hash 上下文跨分块保持）。
    struct Session {
        std::filesystem::path source;
        std::string display_name;
        std::uint64_t total = 0;
        bool copying = false;  // false = hash 相位，true = copy 相位
        std::uint64_t offset = 0;
        std::ifstream input;
        Sha256 hash;
    };

    void execute(const TransferIoControl::Job& job) {
        switch (job.kind) {
            case TransferIoControl::Job::Kind::open:
                open_session(job);
                break;
            case TransferIoControl::Job::Kind::advance:
                advance_session(job.transfer);
                break;
            case TransferIoControl::Job::Kind::cancel:
                cancel_session(job.transfer);
                break;
            case TransferIoControl::Job::Kind::release:
                release_session(job.transfer);
                break;
        }
    }

    void open_session(const TransferIoControl::Job& job) {
        auto [it, inserted] = sessions_.try_emplace(job.transfer.value);
        if (!inserted) {
            emit({job.transfer,
                     aki::transfer::TransferIoEvent::Phase::failed,
                     0, 0, {}, "transfer io: session already open"},
                /*accounted=*/true);
            return;
        }
        Session& session = it->second;
        session.source = job.source;
        session.display_name = job.display_name;
        session.total = job.total_bytes;
        try {
            session.input.open(session.source, std::ios::binary);
            if (!session.input.is_open()) {
                throw std::runtime_error(
                    "transfer io: cannot open source '" + session.source.string()
                    + "'");
            }
            do_chunk(job.transfer, session);
        } catch (const std::exception& error) {
            fail(job.transfer, error.what());
        }
    }

    void advance_session(const aki::transfer::TransferId& transfer) {
        auto it = sessions_.find(transfer.value);
        if (it == sessions_.end()) {
            emit({transfer, aki::transfer::TransferIoEvent::Phase::failed, 0,
                     0, {}, "transfer io: unknown session (late advance)"},
                /*accounted=*/true);
            return;
        }
        try {
            do_chunk(transfer, it->second);
        } catch (const std::exception& error) {
            fail(transfer, error.what());
        }
    }

    void cancel_session(const aki::transfer::TransferId& transfer) {
        sessions_.erase(transfer.value);  // 析构关闭源句柄
        impl_->store->discard_part(transfer.value);  // 幂等删除（noexcept）
        emit({transfer, aki::transfer::TransferIoEvent::Phase::cancelled, 0, 0,
                 {}, {}},
            /*accounted=*/true);
    }

    // 终态 Completed 路径：清理会话状态、保留 .part（M2-06 作业组消费）。
    void release_session(const aki::transfer::TransferId& transfer) {
        sessions_.erase(transfer.value);
        emit({transfer, aki::transfer::TransferIoEvent::Phase::released, 0, 0,
                 {}, {}},
            /*accounted=*/true);
    }

    // 单分块推进（hash → copy 相位切换在块边界；空文件首块即直达终态）。
    void do_chunk(const aki::transfer::TransferId& transfer, Session& session) {
        if (!session.copying) {
            if (session.offset < session.total) {
                const auto want = static_cast<std::size_t>(
                    std::min<std::uint64_t>(impl_->options.chunk_bytes,
                        session.total - session.offset));
                std::vector<char> buffer(want);
                read_at(session, session.offset, buffer);
                session.hash.update(std::as_bytes(std::span{buffer}));
                session.offset += buffer.size();
                if (session.offset < session.total) {
                    emit({transfer,
                             aki::transfer::TransferIoEvent::Phase::hash_progress,
                             session.offset, session.total, {}, {}},
                        /*accounted=*/true);
                    return;
                }
            }
            // hash 相位完成（含空文件首块直达）。
            const std::string hex = session.hash.final_hex();
            session.copying = true;
            session.offset = 0;
            emit({transfer, aki::transfer::TransferIoEvent::Phase::hash_done,
                     0, session.total, hex, {}},
                /*accounted=*/true);
            return;
        }
        if (session.offset < session.total) {
            const auto want = static_cast<std::size_t>(
                std::min<std::uint64_t>(impl_->options.chunk_bytes,
                    session.total - session.offset));
            std::vector<char> buffer(want);
            read_at(session, session.offset, buffer);
            // 首块覆盖（truncate）、后续追加——顺序写由每会话单飞保持。
            impl_->store->write_part(transfer.value,
                std::as_bytes(std::span{buffer}), session.offset > 0);
            session.offset += buffer.size();
            emit({transfer,
                     session.offset >= session.total
                         ? aki::transfer::TransferIoEvent::Phase::copy_done
                         : aki::transfer::TransferIoEvent::Phase::copy_progress,
                     session.offset, session.total, {}, {}},
                /*accounted=*/true);
            return;
        }
        // 空文件：copy 相位无分块——显物化空 .part（M2-06 终态作业组对
        // 缺失 .part 按契约明确失败，空文件必须有可 SHA-256 的实体）。
        if (session.total == 0) {
            impl_->store->write_part(transfer.value, {}, false);
        }
        emit({transfer, aki::transfer::TransferIoEvent::Phase::copy_done,
                 session.total, session.total, {}, {}},
            /*accounted=*/true);
    }

    void read_at(Session& session, std::uint64_t offset,
        std::span<char> buffer) {
        session.input.clear();
        session.input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!session.input.good()) {
            throw std::runtime_error(
                "transfer io: seek failed in '" + session.source.string() + "'");
        }
        session.input.read(buffer.data(),
            static_cast<std::streamsize>(buffer.size()));
        if (session.input.gcount()
            != static_cast<std::streamsize>(buffer.size())) {
            throw std::runtime_error(
                "transfer io: short read in '" + session.source.string()
                + "' (source changed or truncated?)");
        }
    }

    void fail(const aki::transfer::TransferId& transfer,
        const std::string& error) {
        sessions_.erase(transfer.value);
        emit({transfer, aki::transfer::TransferIoEvent::Phase::failed, 0, 0,
                 {}, error},
            /*accounted=*/true);
    }

    // 停止/退出路径：存量会话清理（.part 幂等删除；合成 cancelled 通知，
    // accounted=false——这些会话没有未结算作业），通道内未执行作业按取消
    // 结算（事件回调即结算，不悬挂）。
    void drain_and_cancel() {
        std::vector<std::string> keys;
        keys.reserve(sessions_.size());
        for (const auto& [key, session] : sessions_) {
            keys.push_back(key);
        }
        for (const auto& key : keys) {
            impl_->store->discard_part(key);
        }
        sessions_.clear();
        for (const auto& key : keys) {
            emit({aki::transfer::TransferId{key},
                     aki::transfer::TransferIoEvent::Phase::cancelled, 0, 0,
                     {}, {}},
                /*accounted=*/false);
        }
        TransferIoControl::Job job;
        while (impl_->channel.try_receive(job)) {
            emit({job.transfer,
                     aki::transfer::TransferIoEvent::Phase::cancelled, 0, 0,
                     {}, {}},
                /*accounted=*/true);
        }
        impl_->channel.close();  // 此后提交明确拒绝
    }

    // 事件投递（worker 线程；不抛出——sink 契约）。accounted = true 时在
    // 回调返回后递减 in_flight（作业结算）。
    void emit(const aki::transfer::TransferIoEvent& event, bool accounted) {
        {
            std::lock_guard<std::mutex> guard(impl_->sink_mutex);
            if (impl_->sink) {
                impl_->sink(event);  // sink 契约：有界、不抛出
            }
        }
        if (accounted) {
            impl_->in_flight.fetch_sub(1, std::memory_order_release);
        }
    }

    std::shared_ptr<TransferIoControl::Impl> impl_;
    std::map<std::string, Session> sessions_;  // 仅 run 线程访问
};

}  // namespace aki::persistence
