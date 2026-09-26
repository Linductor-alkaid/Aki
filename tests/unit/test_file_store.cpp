// M2-06：文件本体存储布局与生命周期单测（DEC-004；设计第 11.1 节 ④/②）。
//
// 覆盖（验收 ①②③⑤）：
//   - 作业组全路径（经 DatabaseWorker 串行执行，验收 ⑤）：Completed 成功
//     （SHA-256 + 原子改名 + 回写位断言）、幂等重跑跳过、.part 缺失明确
//     失败、Failed/Cancelled 删除且幂等；
//   - 启动清扫：残留清除 / 活动保留 / 终态行残留清除 / 非法名清除；
//   - 路径安全：TransferId 字符集拒绝、穿越载荷（../ 绝对路径 分隔符）
//     不落盘（净化后磁盘名无穿越）；
//   - SHA-256 已知向量（FIPS 180-4）、布局创建、平台解析公开面 std::string。
//
// 本 TU 只链接 aki_persistence + aki_app（无 sqlite include 路径传播）——
// RULE-10 公开边界延续（验收 ④）。
#include "app/lifecycle/executor_owner.hpp"
#include "persistence/database/database_worker.hpp"
#include "persistence/database/database_worker_adapter.hpp"
#include "persistence/migration/migration.hpp"
#include "persistence/migration/schema_v1.hpp"
#include "persistence/repository/repositories.hpp"
#include "persistence/storage/data_root.hpp"
#include "persistence/storage/file_jobs.hpp"
#include "persistence/storage/file_store.hpp"
#include "persistence/storage/sha256.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using aki::app::ExecutorOwner;
using aki::app::ExecutorShutdownReport;
using aki::persistence::Database;
using aki::persistence::DatabaseWorkerControl;
using aki::persistence::DatabaseWorkerOptions;
using aki::persistence::DatabaseWorkerRunnable;
using aki::persistence::DbJob;
using aki::persistence::FileStore;
using aki::persistence::Migrator;
using aki::persistence::Repositories;
using aki::persistence::Sha256;
using aki::transfer::FileMetadata;
using aki::transfer::Transfer;
using aki::transfer::TransferId;
using aki::transfer::TransferState;

int g_counter = 0;

std::string temp_root(const std::string& tag) {
    auto path = std::filesystem::temp_directory_path()
        / ("aki-filestore-test-" + tag + "-"
            + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())
            + "-" + std::to_string(++g_counter));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
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

Transfer make_transfer_for(const std::string& id, const std::string& name) {
    Transfer transfer;
    transfer.id = TransferId{id};
    transfer.sender = aki::device::DeviceId{"local-1"};
    transfer.receiver = aki::device::DeviceId{"alpha-01"};
    transfer.file = aki::transfer::FileMetadata{name, 0, "application/octet-stream", ""};
    return transfer;
}

std::vector<std::byte> bytes_of(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()),
        reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>());
}

struct WorkerFixture {
    explicit WorkerFixture(std::unique_ptr<Repositories> repositories)
        : control(std::make_shared<DatabaseWorkerControl>(
              std::move(repositories))),
          repos(control->repositories()) {}

    explicit WorkerFixture(std::shared_ptr<DatabaseWorkerControl> shared)
        : control(std::move(shared)), repos(control->repositories()) {
        runnable = std::make_unique<DatabaseWorkerRunnable>(control);
        executor::BlockingWorkerSpec spec;
        spec.name = "aki.db-worker";
        spec.config.thread_name = "aki-db-worker";
        spec.worker = std::move(runnable);
        REQUIRE(owner.initialize());
        REQUIRE(owner.start_blocking_worker(std::move(spec)));
        control->mark_registered();
    }

    // EXEC-01 步骤 1 钩子：request_drain + 有界等待排空完成。
    [[nodiscard]] ExecutorShutdownReport shutdown_with_drain() {
        return owner.shutdown([&] {
            control->request_drain();
            REQUIRE(wait_until([&] { return control->drain_completed(); }, 3s));
        });
    }

    std::string path = temp_root("worker-db");
    ExecutorOwner owner;
    std::shared_ptr<DatabaseWorkerControl> control;
    std::unique_ptr<DatabaseWorkerRunnable> runnable;
    Repositories& repos;
};

}  // namespace

// ---- SHA-256 已知向量（FIPS 180-4）----

TEST_CASE("SHA-256 matches known vectors", "[unit][file_store]") {
    CHECK(aki::persistence::sha256_hex(bytes_of(""))
        == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(aki::persistence::sha256_hex(bytes_of("abc"))
        == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // 流式与一次性一致（跨块边界）。
    const std::string long_input(1000, 'x');
    aki::persistence::Sha256 streaming;
    streaming.update(bytes_of(long_input.substr(0, 1)));
    streaming.update(bytes_of(long_input.substr(1, 700)));
    streaming.update(bytes_of(long_input.substr(701)));
    CHECK(streaming.final_hex()
        == aki::persistence::sha256_hex(bytes_of(long_input)));
}

// ---- 流式分块：部分缓冲未被本次数据填满时不得丢弃已缓冲字节（回归）----

TEST_CASE("SHA-256 streaming keeps partial buffer across small updates",
    "[unit][file_store]") {
    const std::string input(30, 'A');
    const std::string expected =
        "37b9403cf88cc2639d0a118d757a43a0ff6d4871823707ab6a8bb56bc68e8e79";
    CHECK(aki::persistence::sha256_hex(bytes_of(input)) == expected);

    // 任意小于块的分块序列与一次性一致（Python hashlib 交叉验证）。
    // 旧实现在「部分缓冲未被本次数据填满」时把 buffer_size_ 清零丢字节；
    // 10+20 / 10+10+10 分块结构性触发该路径。
    for (const auto& splits : {std::vector<std::size_t>{10, 20},
             std::vector<std::size_t>{10, 10, 10}}) {
        aki::persistence::Sha256 streaming;
        std::size_t offset = 0;
        for (const std::size_t chunk : splits) {
            streaming.update(bytes_of(input.substr(offset, chunk)));
            offset += chunk;
        }
        CHECK(offset == input.size());
        CHECK(streaming.final_hex() == expected);
    }
}

// ---- TransferId 约束与净化（验收 ③）----

TEST_CASE("TransferId charset and disk-name sanitization reject traversal",
    "[unit][file_store][path-safety]") {
    REQUIRE(FileStore::valid_transfer_id("T-1_ok"));
    REQUIRE_FALSE(FileStore::valid_transfer_id(""));
    REQUIRE_FALSE(FileStore::valid_transfer_id(std::string(65, 'a')));
    REQUIRE_FALSE(FileStore::valid_transfer_id("../evil"));
    REQUIRE_FALSE(FileStore::valid_transfer_id("a/b"));
    REQUIRE_FALSE(FileStore::valid_transfer_id("a b"));
    REQUIRE_FALSE(FileStore::valid_transfer_id("a.b"));  // 点不在字符集

    // 穿越载荷净化：末段分量 + 非法字符替换 + 点名回退。
    CHECK(FileStore::sanitize_disk_name("../../etc/passwd") == "passwd");
    CHECK(FileStore::sanitize_disk_name("..\\..\\boot.ini") == "boot.ini");
    CHECK(FileStore::sanitize_disk_name("a/b/c report.txt") == "c_report.txt");
    CHECK(FileStore::sanitize_disk_name("..") == "file");
    CHECK(FileStore::sanitize_disk_name("") == "file");
    CHECK(FileStore::sanitize_disk_name(std::string(200, 'a')).size() == 128);
}

// ---- 布局与 .part 写入 ----

TEST_CASE("FileStore creates the DEC-004 layout and stores parts",
    "[unit][file_store]") {
    const auto root = temp_root("layout");
    FileStore store(root);
    REQUIRE(std::filesystem::exists(root + "/files"));
    REQUIRE(std::filesystem::exists(root + "/files/tmp"));
    CHECK(store.data_root() == root);

    const std::vector<std::byte> chunk1 = bytes_of("hello ");
    const std::vector<std::byte> chunk2 = bytes_of("world");
    store.write_part("t-1", chunk1);                        // 覆盖
    store.write_part("t-1", chunk2, /*append=*/true);       // 追加（分块流式）
    REQUIRE(store.part_exists("t-1"));
    CHECK(read_file(store.part_path("t-1")) == "hello world");

    CHECK_THROWS_AS(store.write_part("../evil", chunk1),
        std::invalid_argument);                             // 入口拒绝
    CHECK(std::filesystem::exists(root + "/files/tmp"));    // 穿越载荷未落盘
    CHECK_FALSE(std::filesystem::exists(root + "/files/evil"));
}

// ---- 验收 ①：经 DatabaseWorker 的 Completed 作业组全路径 ----

TEST_CASE("Completed job group renames with SHA-256 and writes back",
    "[unit][file_store][database_worker]") {
    const auto root = temp_root("complete");
    FileStore store(root);
    Database db = Database::open(":memory:");
    REQUIRE(aki::persistence::Migrator(aki::persistence::schema_v1_steps())
                .bring_up_to_date(db)
        == 1);
    auto control = std::make_shared<DatabaseWorkerControl>(
        std::make_unique<Repositories>(std::move(db)));
    auto& transfers = control->repositories().transfers;

    Transfer transfer = make_transfer_for("t-1", "模型 权重.bin");
    transfer.state = TransferState::Transferring;
    transfers.upsert(transfer);

    const std::string payload = "persistent file body for t-1";
    store.write_part("t-1", bytes_of(payload));

    WorkerFixture fx(std::move(control));
    auto complete_job = aki::persistence::make_transfer_complete_job(
        std::make_shared<FileStore>(root), "t-1");
    auto complete_future = complete_job.done->get_future();
    REQUIRE(fx.control->enqueue(std::move(complete_job)));
    complete_future.get();  // worker 完成后再断言（消除计数竞态）

    // 经 worker 串行执行的作业：future 结算即完成（EXEC-04/06）。
    REQUIRE(fx.control->completed_count() == 1);

    const auto row = transfers.find(TransferId{"t-1"});
    REQUIRE(row.has_value());
    REQUIRE(row->state == TransferState::Completed);
    REQUIRE(row->file.name == "模型 权重.bin");   // 原始名仅存 DB

    // 回写位（stored_*）不入领域 Transfer，经 SQL 断言（M2-04 complete 落列）。
    // 注意：db 已在 Repositories 组装时移出（空壳），必须经 fx.control 持有的实例。
    aki::persistence::Statement stored =
        fx.control->repositories().database.prepare(
            "SELECT stored_relative_path, stored_sha256, stored_size_bytes"
            " FROM transfer WHERE transfer_id = 't-1';");
    REQUIRE(stored.step());
    const std::string expected_sha =
        aki::persistence::sha256_hex(bytes_of(payload));
    REQUIRE(stored.column_text(0)
        == "files/t-1/_____________.bin");  // 非法字节逐字节替换为 '_'
    REQUIRE(stored.column_text(1) == expected_sha);
    REQUIRE(static_cast<std::uint64_t>(stored.column_int64(2))
        == payload.size());

    CHECK(std::filesystem::exists(root + "/files/t-1"));
    CHECK_FALSE(store.part_exists("t-1"));          // .part 已清理
    CHECK(read_file(root + "/files/t-1/_____________.bin") == payload);

    // 幂等重跑：行已 Completed 且目标文件存在 → 整组跳过计成功。
    store.complete_transfer(transfers, "t-1");
    CHECK(fx.control->completed_count() == 1);      // worker 侧无新执行

    const auto report = fx.shutdown_with_drain();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("Completed job group fails explicitly when the part is missing",
    "[unit][file_store][database_worker]") {
    const auto root = temp_root("missing");
    FileStore store(root);
    Database db = Database::open(":memory:");
    REQUIRE(aki::persistence::Migrator(aki::persistence::schema_v1_steps())
                .bring_up_to_date(db)
        == 1);
    auto control = std::make_shared<DatabaseWorkerControl>(
        std::make_unique<Repositories>(std::move(db)));
    auto& transfers = control->repositories().transfers;
    Transfer transfer = make_transfer_for("t-miss", "x.bin");
    transfer.state = TransferState::Transferring;
    transfers.upsert(transfer);

    WorkerFixture fx(std::move(control));
    auto job = aki::persistence::make_transfer_complete_job(
        std::make_shared<FileStore>(root), "t-miss");
    auto job_future = job.done->get_future();
    REQUIRE(fx.control->enqueue(std::move(job)));
    REQUIRE_THROWS_AS(job_future.get(), std::runtime_error);

    REQUIRE(fx.control->failed_count() == 1);        // RULE-09：失败可见
    const auto row = transfers.find(TransferId{"t-miss"});
    REQUIRE(row->state == TransferState::Transferring);  // 不伪造完成

    // worker 存活：后续正常作业继续。
    auto recovery = aki::persistence::make_transfer_discard_job(
        std::make_shared<FileStore>(root), "t-miss");
    auto recovery_future = recovery.done->get_future();
    REQUIRE(fx.control->enqueue(std::move(recovery)));
    REQUIRE_NOTHROW(recovery_future.get());

    const auto report = fx.shutdown_with_drain();
    REQUIRE(report.fully_stopped());

    // 关闭后：repos 由 control 持有（生命周期覆盖 fixture 全程）。
    // 注：对 Database 的 SQL 访问已在 shutdown 前完成；关闭后不再做 DB 调用。
}

TEST_CASE("Failed and Cancelled terminal states discard the part idempotently",
    "[unit][file_store][database_worker]") {
    const auto root = temp_root("discard");
    FileStore store(root);
    store.write_part("t-cancel", bytes_of("partial"));

    Database db = Database::open(":memory:");
    REQUIRE(aki::persistence::Migrator(aki::persistence::schema_v1_steps())
                .bring_up_to_date(db)
        == 1);
    auto control = std::make_shared<DatabaseWorkerControl>(
        std::make_unique<Repositories>(std::move(db)));
    auto& transfers = control->repositories().transfers;
    Transfer transfer = make_transfer_for("t-cancel", "x.bin");
    transfer.state = TransferState::Transferring;
    transfers.upsert(transfer);

    WorkerFixture fx(std::move(control));
    REQUIRE(fx.control->enqueue(aki::persistence::make_transfer_discard_job(
        std::make_shared<FileStore>(root), "t-cancel")));
    (void)fx.control->enqueue(aki::persistence::make_transfer_discard_job(
        std::make_shared<FileStore>(root), "t-cancel"));  // 幂等重跑
    (void)fx.shutdown_with_drain();

    REQUIRE_FALSE(store.part_exists("t-cancel"));
    const auto row = transfers.find(TransferId{"t-cancel"});
    REQUIRE(row.has_value());
}

// ---- 验收 ②：启动清扫 ----

TEST_CASE("Startup sweep removes orphans and keeps active transfers",
    "[unit][file_store][sweep]") {
    const auto root = temp_root("sweep");
    FileStore store(root);
    store.write_part("t-active", bytes_of("keep"));
    store.write_part("t-done", bytes_of("drop"));
    store.write_part("t-unknown", bytes_of("drop"));
    {
        // 非法名残留：id 校验拒绝 → 视为垃圾清理。
        std::ofstream garbage(store.data_root() + "/files/tmp/bad name.part",
            std::ios::binary);
        garbage << "junk";
    }

    Database db = Database::open(":memory:");
    REQUIRE(aki::persistence::Migrator(aki::persistence::schema_v1_steps())
                .bring_up_to_date(db)
        == 1);
    aki::persistence::TransferRepository transfers(db);
    Transfer active = make_transfer_for("t-active", "a.bin");
    active.state = TransferState::Transferring;   // 活动 → .part 保留
    transfers.upsert(active);
    Transfer done = make_transfer_for("t-done", "b.bin");
    done.state = TransferState::Completed;        // 终态 → .part 清理
    transfers.upsert(done);
    // t-unknown 无行 → 清理。

    const std::size_t removed =
        store.sweep_tmp_orphans(transfers.load_all());
    CHECK(removed == 3);
    CHECK(store.part_exists("t-active"));          // 活动保留
    CHECK_FALSE(store.part_exists("t-done"));
    CHECK_FALSE(store.part_exists("t-unknown"));
    CHECK_FALSE(std::filesystem::exists(
        root + "/files/tmp/bad name.part"));
}

// ---- 验收 ④：数据根解析公开面 ----

TEST_CASE("Data root resolution exposes a std::string path",
    "[unit][file_store]") {
    const std::string root = aki::persistence::resolve_data_root();
    CHECK_FALSE(root.empty());
    CHECK(root == aki::persistence::resolve_data_root());  // 稳定
}
