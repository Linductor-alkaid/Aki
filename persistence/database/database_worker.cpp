// DatabaseWorker 控制面实现（M2-05）。executor/sqlite 细节经 pimpl 与
// database_worker_adapter.hpp（注册侧接线头）隐藏在本层。
#include "persistence/database/database_worker.hpp"

#include "persistence/database/database_worker_adapter.hpp"

#include <stdexcept>
#include <utility>

namespace aki::persistence {

DatabaseWorkerControl::DatabaseWorkerControl(DatabaseWorkerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

DatabaseWorkerControl::~DatabaseWorkerControl() = default;

void DatabaseWorkerControl::mark_registered() noexcept {
    impl_->registered.store(true);
}

bool DatabaseWorkerControl::enqueue(DbJob job) {
    // 非法作业（缺执行体/缺完成通道）与未注册：明确拒绝（RULE-09）。
    if (!job.work || !job.done) {
        impl_->rejected.fetch_add(1);
        return false;
    }
    if (!impl_->registered.load()) {
        impl_->rejected.fetch_add(1);
        return false;
    }
    // 通道满 / 已关闭：try_send 明确拒绝（CommStats Dropped / ClosedSend 可见）。
    if (!impl_->channel.try_send(std::move(job))) {
        impl_->rejected.fetch_add(1);
        return false;
    }
    return true;
}

void DatabaseWorkerControl::request_exit() noexcept {
    impl_->exit_requested.store(true);
}

bool DatabaseWorkerControl::exit_requested() const noexcept {
    return impl_->exit_requested.load();
}

void DatabaseWorkerControl::request_drain() noexcept {
    impl_->drain_requested.store(true);
}

bool DatabaseWorkerControl::drain_requested() const noexcept {
    return impl_->drain_requested.load();
}

bool DatabaseWorkerControl::drain_completed() const noexcept {
    return impl_->drain_completed.load();
}

bool DatabaseWorkerControl::drain_budget_exhausted() const noexcept {
    return impl_->drain_budget_exhausted.load();
}

bool DatabaseWorkerControl::is_closed() const noexcept {
    return impl_->channel.is_closed();
}

std::uint64_t DatabaseWorkerControl::completed_count() const noexcept {
    return impl_->completed.load();
}

std::uint64_t DatabaseWorkerControl::failed_count() const noexcept {
    return impl_->failed.load();
}

std::uint64_t DatabaseWorkerControl::rejected_count() const noexcept {
    return impl_->rejected.load();
}

std::uint64_t DatabaseWorkerControl::channel_dropped_count() const noexcept {
    return impl_->channel.stats().dropped_count;
}

std::uint64_t DatabaseWorkerControl::channel_closed_send_count()
    const noexcept {
    return impl_->channel.stats().closed_send_count;
}

std::uint64_t DatabaseWorkerControl::channel_depth() const noexcept {
    return impl_->channel.size_approx();
}

}  // namespace aki::persistence
