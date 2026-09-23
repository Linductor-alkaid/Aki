// DatabaseWorker 公开控制面（DEC-004；设计第 11.1 节 ①③；M2-05）。
//
// RULE-10 边界：本公开头文件不含 sqlite3 类型，也不含 executor 类型
// （实现细节经 pimpl 隐藏在 database_worker.cpp；IBlockingIoWorker 适配器在
// database_worker_adapter.hpp 注册侧接线头，executor 类型按设计第 8.2 节
// 允许存在于该层，不属于本守卫公开面）。公开 API 只暴露 M1 领域类型与
// std 标准类型。
//
// 结构（EXEC-04/07，设计第 11.1 节 ③）：
//   - DatabaseWorkerControl：宿主侧控制面（任意上下文线程安全）——作业入队
//     （有界 MpscChannel，满/关闭明确拒绝，RULE-09）、drain 请求与状态、
//     完成/失败/拒绝计数（EXEC-06）。对象由宿主持有（shared_ptr 语义由
//     调用方管理），生命周期覆盖注册→运行→关闭全过程。
//   - DatabaseWorkerRunnable（database_worker_adapter.hpp）：实现
//     executor::IBlockingIoWorker，单一 Database 连接独占 + 串行消费作业
//     （消费 M2-04 仓储层），经宿主 `ExecutorOwner::start_blocking_worker`
//     注册（EXEC-07：WorkerHandle 归 owner）。
//
// 关闭顺序（设计第 11.1 节 ③，EXEC-01 步骤 1 钩子内）：宿主钩子在
// `AppStateOwner.close()` 之后调用 `request_drain()` 并有界等待
// `drain_completed()`；run() 循环排空（有界预算，耗尽如实记录）后关闭通道；
// 其后 EXEC-01 步骤 2 request_stop → 步骤 3 stop()（join）。排空完成后
// 新作业入队明确拒绝（RULE-09）。
//
// 取消粒度：run() 在作业间检查 StopToken（EXEC-01 步骤 2 置位后退出），
// 执行中的语句/作业不被打断（设计第 11.1 节 ③）。通道等待为有界超时
// （Options::wait_timeout），结构性可被 wakeup 解除阻塞（§8.2 步骤 3）。
//
// 启动纪律（设计第 11.1 节 ②）：启动恢复在主线程同步执行、不经本 worker；
// 注册完成前（`mark_registered()` 未调用）入队明确拒绝。
#pragma once

#include "persistence/database/database.hpp"
#include "persistence/repository/repositories.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace aki::persistence {

class DatabaseWorkerRunnable;

// 构造选项置于命名空间作用域（GCC 对嵌套 Options 默认实参的限制，同
// DatabaseOptions 处理）。
struct DatabaseWorkerOptions {
    std::size_t channel_capacity = 256;                  // RULE-09：作业通道预算
    std::chrono::milliseconds wait_timeout{100};         // run() 通道等待上界（可解除阻塞）
    std::chrono::milliseconds drain_budget{2000};        // 排空有界预算（耗尽如实记录）
    std::size_t repository_cache_capacity = 16;          // 仓储语句缓存容量
};

// 仓储组：单一连接独占（DEC-004），四仓储共享同一 Database。
struct Repositories {
    Database database;
    DeviceRepository devices;
    ConversationRepository conversations;
    MessageRepository messages;
    TransferRepository transfers;

    explicit Repositories(Database worker_database,
        std::size_t cache_capacity = 16)
        : database(std::move(worker_database)),
          devices(database, cache_capacity),
          conversations(database, cache_capacity),
          messages(database, cache_capacity),
          transfers(database, cache_capacity) {}

    // 不可拷贝亦不可移动：四仓储持有指向 database 成员的回引，重定位即悬垂
    // （设计第 11.1 节 ③ 单一连接独占的寿命契约；宿主以 unique_ptr 锚定）。
    Repositories(const Repositories&) = delete;
    Repositories& operator=(const Repositories&) = delete;
    Repositories(Repositories&&) = delete;
    Repositories& operator=(Repositories&&) = delete;
};

// 单个 DB 作业：在 worker 串行上下文内对仓储层执行的操作。
// done 必须非空：成功 set_value、异常 set_exception（调用方经 future 消费，
// RULE-09 失败可见）。作业顺序 = 接受顺序（单消费者串行，设计第 11.1 节 ①）。
struct DbJob {
    std::function<void(Repositories&)> work;
    std::shared_ptr<std::promise<void>> done;
};

// 作业在执行前被放弃（worker 协作退出 / 排空预算耗尽 / 无排空停止）时经
// future 结算的取消异常：关闭中的提交必须得到明确结果，不得悬挂至
// broken_promise 或静默吞掉（AGENTS 规则 10 / RULE-09）。
class JobCancelledError : public std::runtime_error {
public:
    explicit JobCancelledError(const char* context)
        : std::runtime_error(std::string(
                                 "database worker: job cancelled before"
                                 " execution (")
            + context + ")") {}
};


class DatabaseWorkerControl {
public:
    explicit DatabaseWorkerControl(
        DatabaseWorkerOptions options = {});
    ~DatabaseWorkerControl();

    DatabaseWorkerControl(const DatabaseWorkerControl&) = delete;
    DatabaseWorkerControl& operator=(const DatabaseWorkerControl&) = delete;

    // 注册侧接线（ExecutorOwner::start_blocking_worker 成功后由宿主调用）；
    // 未注册前入队明确拒绝（设计第 11.1 节 ② 启动纪律）。
    void mark_registered() noexcept;

    // 作业入队（任意上下文）。false = 拒绝（未注册 / 作业不完整 / 通道满 /
    // 已关闭），拒绝计数可见（RULE-09 / EXEC-06）。
    [[nodiscard]] bool enqueue(DbJob job);

    // EXEC-01 步骤 1 钩子内（AppStateOwner.close() 之后）请求排空；run()
    // 循环在 Options::drain_budget 预算内消费至通道空，随后关闭通道。
    void request_drain() noexcept;

    // 协作退出信号（与 drain 互斥使用）：run() 循环在作业间检查后立即退出，
    // 已入队但未执行的作业以 JobCancelledError 经 future 结算（不执行、
    // 不结算成功，RULE-09 可见），通道随后关闭（exit 后入队明确拒绝）。
    // 用于无排空需求的关闭路径与 DOD-02 执行中取消语义的确定性验证。
    void request_exit() noexcept;

    [[nodiscard]] bool drain_requested() const noexcept;
    [[nodiscard]] bool exit_requested() const noexcept;
    [[nodiscard]] bool drain_completed() const noexcept;
    [[nodiscard]] bool drain_budget_exhausted() const noexcept;  // 耗尽如实记录
    [[nodiscard]] bool is_closed() const noexcept;               // 通道已关闭

    // EXEC-06 观测（std 类型，不泄漏 executor 类型）。
    [[nodiscard]] std::uint64_t completed_count() const noexcept;
    [[nodiscard]] std::uint64_t failed_count() const noexcept;
    [[nodiscard]] std::uint64_t rejected_count() const noexcept;
    [[nodiscard]] std::uint64_t channel_dropped_count() const noexcept;
    [[nodiscard]] std::uint64_t channel_closed_send_count() const noexcept;
    [[nodiscard]] std::uint64_t channel_depth() const noexcept;

    struct Impl;

private:
    friend class DatabaseWorkerRunnable;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aki::persistence
