// M2-05：DatabaseWorker 单测（blocking worker 首次启用；DEC-004；设计第
// 8.2/8.3/11.1 节 ①③）。
//
// 覆盖（验收 ①②，DOD-02 六项沿 DatabaseWorker 路径 = M2 退出-2）：
//   - 正常完成：入队 → 串行执行 → 仓储生效（文件库重启断言）→ promise 结算；
//   - 任务异常：SqliteError/runtime_error 经 promise 结算 + failed 计数，
//     worker 存活、后续作业继续（未捕获会以 WorkerException 终止 worker——
//     硬实现纪律的显式断言）；
//   - 提交拒绝：未注册 / 作业不完整 / 通道满 / 排空关闭后，均明确拒绝
//     （RULE-09 计数可见）；
//   - 执行中取消：StopToken 在作业间检查——在飞作业完成不打断，未执行存量
//     以 JobCancelledError 经 future 结算（不悬挂、失败可见，RULE-09；
//     EXEC-01 步骤 2 语义）；
//   - 超时：drain 有界预算耗尽如实记录（drain_budget_exhausted），不伪造完成；
//   - shutdown：钩子 request_drain → 排空（wakeup/等待延迟上界实测）→
//     EXEC-01 步骤 2/3 → fully_stopped，已 admit 作业零丢失（含进程存活
//     超过 drain_budget 后才请求排空的场景——预算锚定于 drain 请求时刻）。
// 另覆盖：同实体作业 FIFO 串行保序（验收 ②，以 FK 依赖作业序证明）；
// RULE-07：并发只经 executor（blocking worker + owner）承载。
//
// 本 TU 链接 aki_app（ExecutorOwner）与 aki_persistence，并包含注册侧接线头
// database_worker_adapter.hpp（executor 类型在注册侧接线层，RULE-10 守卫的
// 公开头不含之——公开面守卫由 test_persistence_public_surface 延续）。
#include "app/lifecycle/executor_owner.hpp"
#include "persistence/database/database_worker.hpp"
#include "persistence/database/database_worker_adapter.hpp"
#include "persistence/migration/migration.hpp"
#include "persistence/migration/schema_v1.hpp"
#include "persistence/repository/repositories.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using aki::app::ExecutorOwner;
using aki::app::ExecutorShutdownReport;
using aki::conversation::Conversation;
using aki::conversation::ConversationId;
using aki::conversation::ConversationState;
using aki::device::DeviceId;
using aki::device::DeviceIdentity;
using aki::device::TrustState;
using aki::persistence::Database;
using aki::persistence::DatabaseWorkerControl;
using aki::persistence::DatabaseWorkerOptions;
using aki::persistence::DatabaseWorkerRunnable;
using aki::persistence::DbJob;
using aki::persistence::Repositories;
using aki::transfer::TransferId;
using aki::transfer::TransferState;

int g_counter = 0;

std::string temp_db_path(const std::string& tag) {
    auto path = std::filesystem::temp_directory_path()
        / ("aki-dbworker-test-" + tag + "-"
            + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())
            + "-" + std::to_string(++g_counter) + ".db3");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path.string();
}

bool wait_until(const std::function<bool()>& predicate,
    std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

DeviceIdentity make_device(const std::string& id) {
    DeviceIdentity identity;
    identity.id = DeviceId{id};
    identity.display_name = "device-" + id;
    identity.trust_state = TrustState::Trusted;
    return identity;
}

// 每用例独立 owner + 文件库 + 迁移（主线程，启动纪律：注册前完成）+ 注册。
struct WorkerFixture {
    explicit WorkerFixture(const std::string& tag,
        DatabaseWorkerOptions worker_options = {})
        : path(temp_db_path(tag)) {
        auto database = Database::open(path);
        REQUIRE(aki::persistence::Migrator(
                    aki::persistence::schema_v1_steps())
                    .bring_up_to_date(database)
            == 1);
        auto repositories = std::make_unique<Repositories>(
            std::move(database), worker_options.repository_cache_capacity);
        control = std::make_shared<DatabaseWorkerControl>(worker_options);
        runnable = std::make_unique<DatabaseWorkerRunnable>(
            std::move(repositories), control);
    }

    // 注册（EXEC-07 唯一入口）。thread_name 缺失即干净失败（不静默降级）。
    [[nodiscard]] bool register_worker() {
        executor::BlockingWorkerSpec spec;
        spec.name = "aki.db-worker";
        spec.config.thread_name = "aki-db-worker";
        spec.worker = std::move(runnable);
        const bool started = owner.initialize()
            && owner.start_blocking_worker(std::move(spec));
        if (started) {
            control->mark_registered();
        }
        return started;
    }

    // EXEC-01 步骤 1 钩子：request_drain + 有界等待排空完成（设计第 11.1 节
    // ③ 排空位于钩子内、先于步骤 2/3）。
    [[nodiscard]] ExecutorShutdownReport shutdown_with_drain() {
        return owner.shutdown([&] {
            control->request_drain();
            REQUIRE(wait_until([&] { return control->drain_completed(); }, 3s));
        });
    }

    std::string path;
    ExecutorOwner owner;
    std::shared_ptr<DatabaseWorkerControl> control;
    std::unique_ptr<DatabaseWorkerRunnable> runnable;
};

// 作业构造：work + 完成通道（future 由调用方持有）。
struct JobWithFuture {
    aki::persistence::DbJob job;
    std::future<void> future;
};

JobWithFuture make_job(
    std::function<void(Repositories&)> work) {
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    return JobWithFuture{aki::persistence::DbJob{std::move(work),
                             std::move(done)},
        std::move(future)};
}

}  // namespace

// ---- DOD-02 正常完成 + 串行保序（验收 ②）----

TEST_CASE("Jobs execute serially in admission order and land in the database",
    "[unit][database_worker][dod02]") {
    WorkerFixture fx("serial");
    REQUIRE(fx.register_worker());

    // 作业 2 的会话外键依赖作业 1 的设备行：顺序错误即 FK 失败（保序证明）。
    std::vector<int> order;
    auto j1 = make_job([&](Repositories& repos) {
        repos.devices.upsert(make_device("local-1"));
        repos.devices.upsert(make_device("alpha-01"));
        order.push_back(1);
    });
    auto j2 = make_job([&](Repositories& repos) {
        Conversation conversation;
        conversation.id = ConversationId{"conv-alpha-01"};
        conversation.local_device = DeviceId{"local-1"};
        conversation.remote_device = DeviceId{"alpha-01"};
        conversation.state = ConversationState::Active;
        repos.conversations.upsert(conversation);
        order.push_back(2);
    });
    auto j3 = make_job([&](Repositories&) { order.push_back(3); });

    REQUIRE(fx.control->enqueue(std::move(j1.job)));
    REQUIRE(fx.control->enqueue(std::move(j2.job)));
    REQUIRE(fx.control->enqueue(std::move(j3.job)));
    REQUIRE(fx.control->rejected_count() == 0);

    j1.future.get();  // 逐个消费 future（RULE-09：结果可见）
    j2.future.get();
    j3.future.get();
    REQUIRE(order == std::vector<int>{1, 2, 3});      // FIFO 串行保序
    REQUIRE(fx.control->completed_count() == 3);
    REQUIRE(fx.control->failed_count() == 0);

    const auto report = fx.shutdown_with_drain();
    REQUIRE(report.fully_stopped());
    REQUIRE(report.blocking_workers_requested == 1);  // EXEC-07：句柄归 owner

    // 仓储生效（文件库重启断言，SCOPE-09 形态）。
    Database reopened = Database::open(fx.path);
    aki::persistence::DeviceRepository devices(reopened);
    const auto device = devices.find(DeviceId{"alpha-01"});
    REQUIRE(device.has_value());
    REQUIRE(device->trust_state == TrustState::Trusted);
    aki::persistence::ConversationRepository conversations(reopened);
    const auto conversation =
        conversations.find(ConversationId{"conv-alpha-01"});
    REQUIRE(conversation.has_value());
    REQUIRE(conversation->state == ConversationState::Active);
}

// ---- DOD-02 任务异常 ----

TEST_CASE("A failing job settles its future and the worker survives",
    "[unit][database_worker][dod02]") {
    WorkerFixture fx("exception");
    REQUIRE(fx.register_worker());

    auto failing = make_job([&](Repositories& repos) {
        repos.transfers.complete(
            TransferId{"t-missing"}, TransferState::Completed);
    });
    REQUIRE(fx.control->enqueue(std::move(failing.job)));
    REQUIRE_THROWS_AS(failing.future.get(), std::runtime_error);

    // worker 存活：后续作业继续执行（未捕获异常会终止 worker——显式断言）。
    auto following = make_job(
        [&](Repositories& repos) { repos.devices.upsert(make_device("d")); });
    REQUIRE(fx.control->enqueue(std::move(following.job)));
    following.future.get();

    REQUIRE(fx.control->failed_count() == 1);
    REQUIRE(fx.control->completed_count() == 1);

    const auto report = fx.shutdown_with_drain();
    REQUIRE(report.fully_stopped());
}

// ---- DOD-02 提交拒绝 ----

TEST_CASE("Submission rejection: unregistered, invalid, full and closed",
    "[unit][database_worker][dod02]") {
    SECTION("unregistered control rejects enqueue (startup discipline)") {
        DatabaseWorkerControl control;
        auto job = make_job([](Repositories&) {});
        REQUIRE_FALSE(control.enqueue(std::move(job.job)));
        REQUIRE(control.rejected_count() == 1);
    }

    SECTION("incomplete job is rejected") {
        WorkerFixture fx("invalid");
        REQUIRE(fx.register_worker());
        auto done = std::make_shared<std::promise<void>>();
        (void)done->get_future();
        REQUIRE_FALSE(fx.control->enqueue(
            aki::persistence::DbJob{nullptr, std::move(done)}));
        REQUIRE(fx.control->rejected_count() == 1);
        (void)fx.shutdown_with_drain();
    }

    SECTION("full channel rejects with backpressure counters") {
        DatabaseWorkerOptions options;
        options.channel_capacity = 2;
        WorkerFixture fx("full", options);
        REQUIRE(fx.register_worker());

        // 占用 worker：门闩作业阻塞 run 循环，通道得以填满。
        auto gate = std::make_shared<std::promise<void>>();
        std::future<void> gate_future = gate->get_future();
        std::atomic<bool> blocker_entered{false};
        auto blocker = make_job([&](Repositories&) {
            blocker_entered.store(true);
            gate_future.wait();
        });
        REQUIRE(fx.control->enqueue(std::move(blocker.job)));
        CHECK(wait_until([&] { return blocker_entered.load(); }, 2s));

        auto j2 = make_job([](Repositories&) {});
        auto j3 = make_job([](Repositories&) {});
        auto j4 = make_job([](Repositories&) {});
        REQUIRE(fx.control->enqueue(std::move(j2.job)));  // 通道 1/2
        REQUIRE(fx.control->enqueue(std::move(j3.job)));  // 通道 2/2
        REQUIRE(fx.control->channel_depth() == 2);
        REQUIRE_FALSE(fx.control->enqueue(std::move(j4.job)));  // 满 → 拒绝
        REQUIRE(fx.control->rejected_count() == 1);

        gate->set_value();  // 放行
        j2.future.get();
        j3.future.get();

        const auto report = fx.shutdown_with_drain();
        REQUIRE(report.fully_stopped());
        REQUIRE(fx.control->completed_count() == 3);
    }

    SECTION("enqueue after drain close is rejected and visible") {
        WorkerFixture fx("closed");
        REQUIRE(fx.register_worker());
        auto job = make_job([](Repositories&) {});
        REQUIRE(fx.control->enqueue(std::move(job.job)));
        job.future.get();

        fx.control->request_drain();
        REQUIRE(wait_until([&] { return fx.control->drain_completed(); }, 3s));
        REQUIRE(fx.control->is_closed());

        auto late = make_job([](Repositories&) {});
        REQUIRE_FALSE(fx.control->enqueue(std::move(late.job)));  // 关闭后拒绝
        REQUIRE(fx.control->channel_closed_send_count() >= 1);    // EXEC-06

        const auto report = fx.owner.shutdown();
        REQUIRE(report.fully_stopped());
    }
}

// ---- DOD-02 执行中取消（作业间 StopToken；在飞作业不打断）----

TEST_CASE("Stop is honored between jobs; the in-flight job is not interrupted",
    "[unit][database_worker][dod02]") {
    WorkerFixture fx("stop-between");
    REQUIRE(fx.register_worker());

    std::atomic<bool> marker_a{false};
    std::atomic<bool> marker_b{false};
    auto gate = std::make_shared<std::promise<void>>();
    std::future<void> gate_future = gate->get_future();
    std::atomic<bool> a_entered{false};
    auto a = make_job([&](Repositories&) {
        a_entered.store(true);               // 在飞：阻塞直至钩子放行
        gate_future.wait();
        marker_a.store(true);
    });
    auto b = make_job([&](Repositories&) { marker_b.store(true); });
    REQUIRE(fx.control->enqueue(std::move(a.job)));
    REQUIRE(wait_until([&] { return a_entered.load(); }, 2s));  // A 已在飞
    REQUIRE(fx.control->enqueue(std::move(b.job)));  // B 排队（不被执行预期）

    // 不排空、直接关闭：钩子先请求协作退出（在飞 A 放行后，作业间退出，
    // B 不再出队）→ 放行 A → EXEC-01 步骤 2 request_stop → 步骤 3 join。
    const auto report = fx.owner.shutdown([&] {
        fx.control->request_exit();
        gate->set_value();
    });
    REQUIRE(fx.control->exit_requested());
    REQUIRE(marker_a.load());            // 在飞作业完成（不打断）
    REQUIRE_FALSE(marker_b.load());      // 存量作业不执行（作业间 token 检查）
    // 放弃 ≠ 吞掉：B 的 future 以取消异常结算（worker join 前完成，RULE-09）。
    REQUIRE_THROWS_AS(b.future.get(),
        aki::persistence::JobCancelledError);
    REQUIRE(fx.control->failed_count() == 1);
    REQUIRE(fx.control->is_closed());    // exit 后通道关闭：admission 不撒谎
    REQUIRE_FALSE(fx.control->enqueue(make_job([](Repositories&) {}).job));
    REQUIRE(report.executor_shutdown_completed);
}

// ---- DOD-02 超时（drain 有界预算耗尽如实记录）----

TEST_CASE("Drain budget exhaustion is recorded and not faked as clean",
    "[unit][database_worker][dod02]") {
    DatabaseWorkerOptions options;
    options.drain_budget = 30ms;  // 有意小于单作业耗时
    WorkerFixture fx("drain-budget", options);
    REQUIRE(fx.register_worker());

    std::atomic<int> executed{0};
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 3; ++i) {
        auto job = make_job([&](Repositories&) {
            std::this_thread::sleep_for(80ms);
            executed.fetch_add(1);
        });
        futures.push_back(std::move(job.future));
        REQUIRE(fx.control->enqueue(std::move(job.job)));
    }

    const auto report = fx.owner.shutdown([&] {
        fx.control->request_drain();
        REQUIRE(wait_until([&] { return fx.control->drain_completed(); }, 3s));
    });

    REQUIRE(fx.control->drain_budget_exhausted());  // 预算耗尽如实记录
    REQUIRE(executed.load() < 3);                    // 未伪造排空完成
    REQUIRE(fx.control->is_closed());

    // 放弃 ≠ 吞掉：未执行存量逐个以取消异常结算，与执行数对账（零悬挂）。
    int cancelled = 0;
    for (auto& future : futures) {
        try {
            future.get();
        } catch (const aki::persistence::JobCancelledError&) {
            ++cancelled;
        }
    }
    REQUIRE(executed.load() + cancelled == 3);
    REQUIRE(fx.control->failed_count() == cancelled);

    REQUIRE(report.executor_shutdown_completed);
}

// ---- DOD-02 shutdown（排空全序列 + 延迟上界实测）----

TEST_CASE("Shutdown drains admitted jobs with bounded latency and full evidence",
    "[unit][database_worker][dod02]") {
    DatabaseWorkerOptions options;
    options.drain_budget = 2s;
    WorkerFixture fx("shutdown", options);
    REQUIRE(fx.register_worker());

    constexpr int kJobs = 5;
    std::vector<std::future<void>> futures;
    for (int i = 0; i < kJobs; ++i) {
        auto job = make_job(
            [&](Repositories& repos) { repos.devices.upsert(make_device(
                                           "d-" + std::to_string(i))); });
        futures.push_back(std::move(job.future));
        REQUIRE(fx.control->enqueue(std::move(job.job)));
    }

    const auto started_at = std::chrono::steady_clock::now();
    const auto report = fx.owner.shutdown([&] {
        fx.control->request_drain();
        REQUIRE(wait_until([&] { return fx.control->drain_completed(); }, 3s));
    });
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);

    // wakeup 可解除阻塞实测：排空完成延迟远小于预算 + 一个等待周期上界。
    REQUIRE(elapsed < options.drain_budget + 1s);

    for (auto& future : futures) {
        future.get();  // 已 admit 作业零丢失
    }
    REQUIRE(fx.control->completed_count() == kJobs);
    REQUIRE(fx.control->drain_budget_exhausted() == false);
    REQUIRE(report.fully_stopped());  // Completed + Stopped + wait_timeout==0
}

// ---- 预算锚定：进程存活超过 drain_budget 后请求排空，预算仍完整 ----
// （回归：deadline 曾锚定于 run() 启动——存活超预算后，排空只处理首个作业
// 就耗尽，已 admit 存量被放弃，违反第 11.1 节 ③ 零丢失契约。）

TEST_CASE("Drain budget anchors at the drain request, not worker start",
    "[unit][database_worker][dod02]") {
    DatabaseWorkerOptions options;
    options.drain_budget = 150ms;
    WorkerFixture fx("drain-anchor", options);
    REQUIRE(fx.register_worker());

    // worker 存活时间超过 drain_budget（旧锚定下预算此时必然已耗尽）。
    std::this_thread::sleep_for(2 * options.drain_budget + 100ms);

    constexpr int kJobs = 3;
    std::vector<std::future<void>> futures;
    for (int i = 0; i < kJobs; ++i) {
        auto job = make_job(
            [&](Repositories& repos) { repos.devices.upsert(make_device(
                                           "d-" + std::to_string(i))); });
        futures.push_back(std::move(job.future));
        REQUIRE(fx.control->enqueue(std::move(job.job)));
    }

    const auto report = fx.shutdown_with_drain();

    for (auto& future : futures) {
        future.get();  // 零丢失：全部正常完成（无取消异常）
    }
    REQUIRE(fx.control->completed_count() == kJobs);
    REQUIRE(fx.control->drain_budget_exhausted() == false);
    REQUIRE(report.fully_stopped());
}

// ---- 未注册即拒绝已在上面覆盖；此处补注册失败（thread_name 缺失）的干净失败 ----

TEST_CASE("Registration failure is a clean failure without silent degrade",
    "[unit][database_worker]") {
    WorkerFixture fx("reg-fail");
    executor::BlockingWorkerSpec spec;
    spec.name = "aki.db-worker";
    // spec.config.thread_name 故意缺失（库校验必填项）。
    spec.worker = std::move(fx.runnable);
    REQUIRE(fx.owner.initialize());
    REQUIRE_FALSE(fx.owner.start_blocking_worker(std::move(spec)));
    auto job = make_job([](Repositories&) {});
    REQUIRE_FALSE(fx.control->enqueue(std::move(job.job)));  // 未注册即拒绝
}
