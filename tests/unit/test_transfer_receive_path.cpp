// M4-05：接收侧与控制面单测（设计 §7.1①②⑤/DEC-012；网络无关，无 [skip]
// 受控退出——始终完整执行）。
//
// 覆盖：
//   - 接收链路（DEC-012①②）：入站建行 → 进度状态推进（Negotiating →
//     Transferring）→ 暂停（第 11 方法）→ 恢复 → committed → M2-06 终态
//     作业组自接收根供源合并（流式 SHA-256 + 就位 files/<id>/<净化名> +
//     stored_* 回写 + 接收原件同作业删除；多段 logical_name）→ 重启一致；
//   - 供源回退次序（.part 优先 → 接收根 → final 恢复 → 明确失败）；
//   - Failed/Cancelled：.part 幂等删除作业（接收侧无 .part 时 no-op）；
//   - 终态幂等与迟到不复活（RULE-08）+ 运行期零传导（DEC-010②：入站传输
//     状态不推进任何消息面）；
//   - 控制面：pause/resume/cancel 命令（Fake 记录 + 未知 id 拒绝 + 取消
//     幂等）——DOD-02 六项沿控制/接收路径（并发原语复用 TM 泵与 DB worker，
//     既有 DOD-02 覆盖沿用；此处覆盖路径行为）。
#include "app/application/transfer_manager.hpp"
#include "app/lifecycle/executor_owner.hpp"
#include "app/state/app_state_owner.hpp"
#include "heyaki/adapter/fake_heyaki_adapter.hpp"
#include "persistence/database/database.hpp"
#include "persistence/database/database_worker.hpp"
#include "persistence/database/database_worker_adapter.hpp"
#include "persistence/migration/migration.hpp"
#include "persistence/migration/schema_v1.hpp"
#include "persistence/repository/repositories.hpp"
#include "persistence/repository/update_jobs.hpp"
#include "persistence/storage/file_jobs.hpp"
#include "persistence/storage/file_store.hpp"
#include "persistence/storage/sha256.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;

using aki::app::AppStateOwner;
using aki::app::ExecutorOwner;
using aki::app::TransferManager;
using aki::app::TransferManagerOptions;
using aki::conversation::MessageId;
using aki::device::DeviceId;
using aki::heyaki::FakeHeyakiAdapter;
using aki::persistence::Database;
using aki::persistence::FileStore;
using aki::persistence::Migrator;
using aki::persistence::schema_v1_steps;
using aki::persistence::sha256_hex;
using aki::transfer::FileMetadata;
using aki::transfer::Transfer;
using aki::transfer::TransferId;
using aki::transfer::TransferState;

int g_counter = 0;

std::string temp_root(const std::string& tag) {
    auto path = std::filesystem::temp_directory_path()
        / ("aki-recv-path-" + tag + "-"
            + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())
            + "-" + std::to_string(++g_counter));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
    return path.string();
}

bool wait_until(const std::function<bool()>& predicate,
    std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> chars{std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>()};
    const auto* data = reinterpret_cast<const std::byte*>(chars.data());
    return {data, data + chars.size()};
}

std::filesystem::path write_source(const std::filesystem::path& path,
    std::uint64_t size, char pattern) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    std::string chunk(1024, pattern);
    std::uint64_t written = 0;
    while (written < size) {
        const auto want = static_cast<std::size_t>(
            std::min<std::uint64_t>(1024, size - written));
        out.write(chunk.data(), static_cast<std::streamsize>(want));
        written += want;
    }
    out.close();
    return path;
}

// 接收路径组合（网络无关，DB 全量）：迁移 + 仓储 + DatabaseWorker +
// 接受后处理器（传输子集，含接收根供源）+ TM + Fake（注入面驱动 wire 事件）。
// receive_dir 模拟 heyaki 接收根（committed 时文件已在其中——测试直接预置）。
struct ReceivePathStack {
    explicit ReceivePathStack(const std::string& root)
        : root(root),
          store(std::make_shared<FileStore>(root)),
          receive_dir(root + "/receive/inbox"),
          database(Database::open(
              (std::filesystem::path{root} / "aki.db3").string())) {
        std::error_code ec;
        std::filesystem::create_directories(receive_dir, ec);
        const Migrator migrator{schema_v1_steps()};
        if (migrator.bring_up_to_date(database) != 1) {
            throw std::runtime_error("receive stack: migration failed");
        }
        db = std::make_shared<aki::persistence::DatabaseWorkerControl>(
            std::make_unique<aki::persistence::Repositories>(
                std::move(database)),
            aki::persistence::DatabaseWorkerOptions{});
        // database 已移交仓储（move-only 单句柄）——后续经 db->repositories()。
        state_owner = std::make_unique<AppStateOwner>(
            aki::app::AppStateOwnerOptions{}, aki::app::AppState{},
            [this](const aki::app::AppStateUpdate& update) {
                std::visit(
                    [&](const auto& concrete) {
                        using Update = std::decay_t<decltype(concrete)>;
                        if constexpr (std::is_same_v<Update,
                                         aki::app::UpsertTransfer>) {
                            auto job =
                                aki::persistence::make_transfer_upsert_job(
                                    concrete.transfer);
                            db_futures.push_back(job.done->get_future());
                            (void)db->enqueue(std::move(job));
                        } else if constexpr (std::is_same_v<Update,
                                                         aki::app::
                                                             UpdateTransferProgress>) {
                            auto job =
                                aki::persistence::make_transfer_progress_job(
                                    concrete.transfer, concrete.transferred,
                                    concrete.total);
                            db_futures.push_back(job.done->get_future());
                            (void)db->enqueue(std::move(job));
                        } else if constexpr (std::is_same_v<Update,
                                                         aki::app::
                                                             CompleteTransfer>) {
                            std::vector<aki::persistence::DbJob> jobs;
                            if (concrete.final_state
                                == TransferState::Completed) {
                                jobs.push_back(
                                    aki::persistence::
                                        make_transfer_complete_job(store,
                                            concrete.transfer.value,
                                            receive_dir));
                            } else {
                                jobs.push_back(
                                    aki::persistence::
                                        make_transfer_terminal_job(
                                            concrete.transfer,
                                            concrete.final_state));
                                jobs.push_back(
                                    aki::persistence::
                                        make_transfer_discard_job(
                                            store,
                                            concrete.transfer.value));
                            }
                            for (auto& job : jobs) {
                                db_futures.push_back(job.done->get_future());
                                (void)db->enqueue(std::move(job));
                            }
                        }
                    },
                    update);
            });
        TransferManagerOptions transfer_options;
        transfer_options.pump.name = "aki.tm";
        transfer_options.sender = DeviceId{"local-1"};
        transfers = std::make_unique<TransferManager>(
            host.executor(), *state_owner, adapter, transfer_options);
        sink.transfers = transfers.get();
        adapter.set_sink(&sink);
        auto db_runnable =
            std::make_unique<aki::persistence::DatabaseWorkerRunnable>(db);
        executor::BlockingWorkerSpec db_spec;
        db_spec.name = "aki.db-worker";
        db_spec.config.thread_name = "aki-db-worker";
        db_spec.worker = std::move(db_runnable);
        if (!host.start_blocking_worker(std::move(db_spec))) {
            throw std::runtime_error("receive stack: db worker failed");
        }
        db->mark_registered();
    }

    ReceivePathStack(const ReceivePathStack&) = delete;
    ReceivePathStack& operator=(const ReceivePathStack&) = delete;

    ~ReceivePathStack() {
        if (!host.is_shutdown()) {
            (void)transfers->request_cancel_all();
            (void)transfers->flush(2s);
            (void)host.shutdown();
        }
    }

    void quiesce() {
        REQUIRE(transfers->flush(2s));
        for (;;) {
            const auto watermark = [](const AppStateOwner& owner) {
                return owner.stats().updates_applied
                    + owner.stats().updates_rejected
                    + owner.stats().events_forwarded
                    + owner.stats().events_dropped;
            };
            const std::uint64_t before = watermark(*state_owner);
            state_owner->drain();
            if (watermark(*state_owner) == before) {
                return;
            }
        }
    }

    void drain_db() {
        db->request_drain();
        REQUIRE(wait_until([&] { return db->drain_completed(); }, 5s));
        for (auto& future : db_futures) {
            future.get();  // 全部作业成功（RULE-09 失败可见）
        }
    }

    std::string root;
    std::shared_ptr<FileStore> store;
    std::string receive_dir;
    Database database;
    std::shared_ptr<aki::persistence::DatabaseWorkerControl> db;
    ExecutorOwner host;
    std::unique_ptr<AppStateOwner> state_owner;
    FakeHeyakiAdapter adapter;
    std::unique_ptr<TransferManager> transfers;
    std::vector<std::future<void>> db_futures;

    struct TransferOnlySink final : aki::heyaki::HeyakiAdapterSink {
        TransferManager* transfers = nullptr;

        explicit TransferOnlySink(TransferManager* target = nullptr)
            : transfers(target) {}

        bool on_device_discovered(aki::device::DiscoveredDevice) override {
            return true;
        }
        bool on_device_connected(DeviceId,
            aki::device::ConnectionPath) override {
            return true;
        }
        bool on_device_disconnected(DeviceId) override { return true; }
        bool on_message_received(aki::conversation::Message) override {
            return true;
        }
        bool on_message_delivered(aki::conversation::ConversationId,
            MessageId) override {
            return true;
        }
        bool on_message_send_failed(aki::conversation::ConversationId,
            MessageId) override {
            return true;
        }
        bool on_transfer_started(aki::transfer::Transfer transfer) override {
            return transfers->enqueue_transfer_started(std::move(transfer));
        }
        bool on_transfer_progress(aki::transfer::TransferId transfer,
            std::uint64_t transferred, std::uint64_t total) override {
            return transfers->enqueue_transfer_progress(
                std::move(transfer), transferred, total);
        }
        bool on_transfer_completed(aki::transfer::TransferId transfer,
            aki::transfer::TransferState final_state) override {
            return transfers->enqueue_transfer_completed(
                std::move(transfer), final_state);
        }
        bool on_transfer_paused(
            aki::transfer::TransferId transfer) override {
            return transfers->enqueue_transfer_paused(std::move(transfer));
        }
        bool on_connection_path_changed(DeviceId,
            aki::device::ConnectionPath, aki::device::ConnectionPath)
            override {
            return true;
        }
        bool on_pairing_completed(DeviceId, bool, std::string_view)
            override {
            return true;
        }
    } sink;
};

}  // namespace

// ---- 接收链路：建行 → 推进 → 暂停/恢复 → committed 合并 → 重启一致 ----

TEST_CASE("Inbound transfer merges from the receive root and survives restart",
    "[unit][receive_path]") {
    const std::string root = temp_root("merge");
    const TransferId id{"t-rx"};
    const std::string logical_name = "docs/model.bin";  // 多段 logical_name
    {
        ReceivePathStack stack{root};
        // heyaki 接收根落盘形态（committed 时已完整——测试预置）。
        const auto receive_file =
            std::filesystem::path{stack.receive_dir} / "docs" / "model.bin";
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path{stack.receive_dir} / "docs", ec);
        write_source(receive_file, 40, 'r');
        const auto expected_bytes = read_bytes(receive_file);
        const auto expected_hash = sha256_hex(expected_bytes);

        // 入站链路（wire 事件经 Fake 注入面，路由同 RouterSink）：
        // 首个状态事件建行（receiver=local）→ 进度推进 Transferring →
        // 暂停 → 恢复 → committed。file.name 用**剥根段**相对名（与真实
        // Adapter 映射一致：wire manifest logical_name = join(root, name)
        // 含根前缀，Adapter 建接收行时剥根段——test_heyaki_node_adapter 的
        // 建行/剥根段断言与本用例的合并断言配对覆盖全链路）。
        Transfer inbound;
        inbound.id = id;
        inbound.sender = DeviceId{"beta"};
        inbound.receiver = DeviceId{"local-1"};
        inbound.file = FileMetadata{logical_name, 40, std::string{}, std::string{}};
        inbound.total = 40;
        inbound.state = TransferState::Negotiating;
        REQUIRE(stack.adapter.inject_transfer_started(inbound));
        stack.quiesce();
        REQUIRE(stack.adapter.inject_transfer_progress(id, 16, 40));
        stack.quiesce();
        {
            executor::comm::Snapshot<aki::app::AppState> snapshot;
            REQUIRE(stack.state_owner->try_load_snapshot(snapshot));
            REQUIRE(snapshot.value.transfers.transfers.front().state
                == TransferState::Transferring);
        }
        REQUIRE(stack.adapter.inject_transfer_paused(id));
        stack.quiesce();
        {
            executor::comm::Snapshot<aki::app::AppState> snapshot;
            REQUIRE(stack.state_owner->try_load_snapshot(snapshot));
            REQUIRE(snapshot.value.transfers.transfers.front().state
                == TransferState::Paused);
        }
        REQUIRE(stack.adapter.inject_transfer_progress(id, 40, 40));
        stack.quiesce();  // 恢复：Paused → Transferring（§7.1⑤）
        {
            executor::comm::Snapshot<aki::app::AppState> snapshot;
            REQUIRE(stack.state_owner->try_load_snapshot(snapshot));
            REQUIRE(snapshot.value.transfers.transfers.front().state
                == TransferState::Transferring);
        }
        REQUIRE(
            stack.adapter.inject_transfer_completed(id, TransferState::Completed));
        REQUIRE(wait_until([&] {
            (void)stack.transfers->flush(100ms);
            stack.state_owner->drain();
            executor::comm::Snapshot<aki::app::AppState> snapshot;
            return stack.state_owner->try_load_snapshot(snapshot)
                && snapshot.value.transfers.transfers.front().state
                    == TransferState::Completed;
        }, 5s));
        stack.drain_db();

        // 合并断言（DEC-012①）：files/<id>/<净化名> 就位 + stored_* 回写 +
        // 接收原件同作业删除 + .part 从未存在。
        const auto final_path = std::filesystem::path{root} / "files"
            / id.value / "model.bin";
        REQUIRE(std::filesystem::exists(final_path));
        REQUIRE(read_bytes(final_path) == expected_bytes);
        REQUIRE_FALSE(std::filesystem::exists(receive_file));
        REQUIRE_FALSE(stack.store->part_exists(id.value));
        {
            aki::persistence::Statement statement = stack.db->repositories()
                                                        .database.prepare(
            "SELECT stored_relative_path, stored_sha256, stored_size_bytes,"
            " state FROM transfer WHERE transfer_id = ?1;");
            statement.bind(1, id.value);
            REQUIRE(statement.step());
            REQUIRE(statement.column_text(0)
                == "files/" + id.value + "/model.bin");
            REQUIRE(statement.column_text(1) == expected_hash);
            REQUIRE(static_cast<std::uint64_t>(statement.column_int64(2))
                == 40);
            REQUIRE(static_cast<int>(statement.column_int64(3))
                == static_cast<int>(TransferState::Completed));
        }

        // 关闭（EXEC-01 序）。
        const auto report = stack.host.shutdown([&] {
            (void)stack.transfers->request_cancel_all();
            CHECK(stack.transfers->flush(2s));
            stack.state_owner->close();
            stack.db->request_drain();
        });
        REQUIRE(report.fully_stopped());
        REQUIRE(report.blocking_workers_stopped == 1);
    }

    // ---- 重启：行 + 文件本体 + 回写列一致（验收）----
    auto reopened = Database::open(
        (std::filesystem::path{root} / "aki.db3").string());
    const Migrator reopen_migrator{schema_v1_steps()};
    REQUIRE(reopen_migrator.bring_up_to_date(reopened) == 0);
    {
        aki::persistence::Statement statement = reopened.prepare(
            "SELECT stored_relative_path, stored_sha256, stored_size_bytes"
            " FROM transfer WHERE transfer_id = ?1;");
        statement.bind(1, id.value);
        REQUIRE(statement.step());
        const std::string relative = statement.column_text(0);
        const std::string stored_hash = statement.column_text(1);
        const auto stored_size =
            static_cast<std::uint64_t>(statement.column_int64(2));
        REQUIRE(relative == "files/" + id.value + "/model.bin");
        REQUIRE(stored_size == 40);
        const auto final_path = std::filesystem::path{root} / "files"
            / id.value / "model.bin";
        REQUIRE(std::filesystem::exists(final_path));
        REQUIRE(sha256_hex(read_bytes(final_path)) == stored_hash);
    }
}

// ---- 供源回退次序与失败可见（DEC-012①：.part 优先 → 接收根 → 明确失败）----

TEST_CASE("Complete source resolution falls back and fails visibly",
    "[unit][receive_path]") {
    const std::string root = temp_root("source");
    ReceivePathStack stack{root};
    const TransferId id{"t-src"};

    // 行建立（入站形态）。
    Transfer inbound;
    inbound.id = id;
    inbound.sender = DeviceId{"beta"};
    inbound.receiver = DeviceId{"local-1"};
    inbound.file = FileMetadata{"only.bin", 8, std::string{}, std::string{}};
    inbound.total = 8;
    inbound.state = TransferState::Transferring;
    inbound.transferred = 8;
    REQUIRE(stack.adapter.inject_transfer_started(inbound));

    // 全部供源缺失（无 .part、接收根无文件）→ 作业明确失败（RULE-09）。
    REQUIRE(stack.adapter.inject_transfer_completed(id, TransferState::Completed));
    REQUIRE(wait_until([&] {
        (void)stack.transfers->flush(100ms);
        stack.state_owner->drain();
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        return stack.state_owner->try_load_snapshot(snapshot)
            && snapshot.value.transfers.transfers.front().state
                == TransferState::Completed;
    }, 5s));
    stack.db->request_drain();
    REQUIRE(wait_until([&] { return stack.db->drain_completed(); }, 5s));
    bool failed_visibly = false;
    for (auto& future : stack.db_futures) {
        try {
            future.get();
        } catch (const std::exception&) {
            failed_visibly = true;  // 「无供源」明确失败（不伪造成功）
        }
    }
    REQUIRE(failed_visibly);
    // 行已 Completed 但 stored_* 为空（存储缺失可见，DEC-011 已知边角形态）。
    {
        aki::persistence::Statement statement =
            stack.db->repositories().database.prepare(
                "SELECT stored_sha256 FROM transfer WHERE transfer_id = ?1;");
        statement.bind(1, id.value);
        REQUIRE(statement.step());
        REQUIRE(statement.column_text(0).empty());
    }

    const auto report = stack.host.shutdown([&] {
        (void)stack.transfers->request_cancel_all();
        CHECK(stack.transfers->flush(2s));
        stack.state_owner->close();
        stack.db->request_drain();
    });
    REQUIRE(report.fully_stopped());
}

// ---- Failed/Cancelled：.part 幂等删除（接收侧无 .part 时 no-op）+
//      终态幂等（RULE-08）+ 运行期零传导 ----

TEST_CASE("Inbound failure paths discard idempotently and stay terminal",
    "[unit][receive_path][dod02]") {
    const std::string root = temp_root("fail");
    ReceivePathStack stack{root};
    const TransferId id{"t-fx"};

    Transfer inbound;
    inbound.id = id;
    inbound.sender = DeviceId{"beta"};
    inbound.receiver = DeviceId{"local-1"};
    inbound.file = FileMetadata{"cancel.bin", 16, std::string{}, std::string{}};
    inbound.total = 16;
    inbound.state = TransferState::Negotiating;
    REQUIRE(stack.adapter.inject_transfer_started(inbound));
    stack.quiesce();
    REQUIRE(stack.adapter.inject_transfer_progress(id, 8, 16));
    stack.quiesce();  // Transferring

    // 取消终态：行 Cancelled + discard 作业 no-op（接收侧无 .part）。
    REQUIRE(
        stack.adapter.inject_transfer_completed(id, TransferState::Cancelled));
    stack.quiesce();
    stack.drain_db();
    {
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        REQUIRE(stack.state_owner->try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.transfers.transfers.front().state
            == TransferState::Cancelled);
    }
    REQUIRE_FALSE(stack.store->part_exists(id.value));

    // 迟到进度/终态不复活（RULE-08）：LateProgress 拒绝 + Completed 拒绝。
    const auto rejected_before = stack.state_owner->stats().updates_rejected;
    REQUIRE(stack.adapter.inject_transfer_progress(id, 16, 16));
    REQUIRE(
        stack.adapter.inject_transfer_completed(id, TransferState::Completed));
    stack.quiesce();
    {
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        REQUIRE(stack.state_owner->try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.transfers.transfers.front().state
            == TransferState::Cancelled);
        REQUIRE(stack.state_owner->stats().updates_rejected
            == rejected_before + 2);
    }
    // 迟到暂停（行缓存已随终态清理）：最小行 upsert 交 owner 拒绝可见。
    REQUIRE(stack.adapter.inject_transfer_paused(id));
    stack.quiesce();
    REQUIRE(stack.state_owner->stats().updates_rejected
        == rejected_before + 3);

    const auto report = stack.host.shutdown();
    REQUIRE(report.fully_stopped());
}

// ---- 控制面：pause/resume/cancel 命令（Fake 记录；未知 id 拒绝；幂等）----
//（DOD-02：控制命令沿 TM 泵串行——泵级六项（正常/异常/拒绝/取消/超时/
// shutdown）由 test_app_managers 既有覆盖；此处覆盖命令路径行为。）

TEST_CASE("Transfer control commands route to the adapter with idempotent "
    "cancel",
    "[unit][receive_path][dod02]") {
    const std::string root = temp_root("control");
    ReceivePathStack stack{root};
    const TransferId id{"t-ctl"};

    Transfer inbound;
    inbound.id = id;
    inbound.sender = DeviceId{"beta"};
    inbound.receiver = DeviceId{"local-1"};
    inbound.file = FileMetadata{"ctl.bin", 4, std::string{}, std::string{}};
    inbound.total = 4;
    inbound.state = TransferState::Transferring;
    REQUIRE(stack.adapter.inject_transfer_started(inbound));
    stack.quiesce();

    // pause/resume/cancel 命令（无 io 会话——接收形态；命令面记录）。
    REQUIRE(stack.transfers->pause_transfer(id));
    REQUIRE(stack.transfers->resume_transfer(id));
    REQUIRE(stack.transfers->cancel_transfer(id));
    REQUIRE(stack.transfers->flush(2s));
    int pause_cmds = 0;
    int resume_cmds = 0;
    int cancel_cmds = 0;
    for (const auto& command : stack.adapter.transfer_commands()) {
        if (command.transfer_id != id) {
            continue;
        }
        using Kind = FakeHeyakiAdapter::TransferCommand::Kind;
        if (command.kind == Kind::Pause) {
            ++pause_cmds;
        } else if (command.kind == Kind::Resume) {
            ++resume_cmds;
        } else if (command.kind == Kind::Cancel) {
            ++cancel_cmds;
        }
    }
    REQUIRE(pause_cmds == 1);
    REQUIRE(resume_cmds == 1);
    REQUIRE(cancel_cmds == 1);

    // 未知 id 的控制命令：enqueue admission 受理（异步命令面），Fake 侧
    // 会话未知 → adapter 调用拒绝（无命令记录——admission false 经命令面
    // 可见，RULE-09）。
    REQUIRE(stack.transfers->pause_transfer(TransferId{"t-unknown"}));
    REQUIRE(stack.transfers->flush(2s));
    bool unknown_cmd_seen = false;
    for (const auto& command : stack.adapter.transfer_commands()) {
        if (command.transfer_id == TransferId{"t-unknown"}) {
            unknown_cmd_seen = true;
        }
    }
    REQUIRE_FALSE(unknown_cmd_seen);

    // 取消幂等：重复取消不再产生新命令（Fake 侧会话已注销）。
    REQUIRE(stack.transfers->cancel_transfer(id));
    REQUIRE(stack.transfers->flush(2s));
    size_t cancel_after = 0;
    for (const auto& command : stack.adapter.transfer_commands()) {
        if (command.transfer_id == id
            && command.kind
                == FakeHeyakiAdapter::TransferCommand::Kind::Cancel) {
            ++cancel_after;
        }
    }
    REQUIRE(cancel_after == 2);  // 两次命令入列（SPI 幂等），行为侧唯一语义

    const auto report = stack.host.shutdown();
    REQUIRE(report.fully_stopped());
}
