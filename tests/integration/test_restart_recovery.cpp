// M2-07：重启恢复集成测试（设计第 11.1 节 ②③；DEC-004 验证方式；SCOPE-09 /
// M2 退出-1）。
//
// 覆盖：
//   - 写入（设备、信任状态、会话、消息含送达终态、传输历史）→ 受控关闭
//     （fully_stopped + DatabaseWorker 钩子末尾排空）→ 重新 open 后逐域断言
//     一致（验收 ①；含 stored_* 回写位、文件本体落盘、迁移幂等、播种对接）；
//   - DB 文件损坏时 open 干净失败（SqliteError，SQLITE_NOTADB；不静默，
//     验收 ②），且失败不残留半初始化状态（同组件对有效根仍可用）；
//   - 启动清扫按加载到的活动 Transfer 行清理 files/tmp/ 残留（活动保留 /
//     终态与无行清除）；
//   - 宿主路径 shutdown 排空不丢作业：批量入队不逐个等待 → 钩子排空 →
//     admitted 全部完成、future 全部结算（M2-05 DOD-02 shutdown 项的宿主
//     组合复验）。
//
// 组合纪律：每用例独立 ExecutorOwner（顺序生命周期，AGENTS 规则 7/8，与
// M2-05 用例集相同形态）；恢复为主线程同步执行、不经 blocking worker（§11.1 ②），
// worker 在播种完成后注册（EXEC-02 启动段纪律）。
//
// 本 TU 链接 aki_app（ExecutorOwner）与 aki_persistence，并包含注册侧接线头
// database_worker_adapter.hpp（executor 类型在接线层，RULE-10）。
#include "app/lifecycle/executor_owner.hpp"
#include "app/state/app_state.hpp"
#include "app/state/app_state_owner.hpp"
#include "persistence/database/database.hpp"
#include "persistence/database/database_worker.hpp"
#include "persistence/database/database_worker_adapter.hpp"
#include "persistence/recovery/startup_recovery.hpp"
#include "persistence/repository/update_jobs.hpp"
#include "persistence/storage/file_jobs.hpp"
#include "persistence/storage/file_store.hpp"
#include "persistence/storage/sha256.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

using aki::app::AppState;
using aki::app::AppStateOwner;
using aki::app::AppStateOwnerOptions;
using aki::app::ExecutorOwner;
using aki::conversation::Conversation;
using aki::conversation::ConversationId;
using aki::conversation::ConversationState;
using aki::conversation::DeliveryState;
using aki::conversation::Message;
using aki::conversation::MessageId;
using aki::conversation::MessageType;
using aki::conversation::TextPayload;
using aki::device::DeviceClass;
using aki::device::DeviceId;
using aki::device::DeviceIdentity;
using aki::device::PresenceState;
using aki::device::TrustState;
using aki::persistence::Database;
using aki::persistence::DatabaseWorkerControl;
using aki::persistence::DatabaseWorkerRunnable;
using aki::persistence::DbJob;
using aki::persistence::RecoveryResult;
using aki::persistence::perform_startup_recovery;
using aki::transfer::Transfer;
using aki::transfer::TransferId;
using aki::transfer::TransferState;

int g_counter = 0;

std::string temp_root(const std::string& tag) {
    auto path = std::filesystem::temp_directory_path()
        / ("aki-recovery-test-" + tag + "-"
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

std::span<const std::byte> bytes_of(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

DeviceIdentity make_device(const std::string& id, TrustState trust) {
    DeviceIdentity identity;
    identity.id = DeviceId{id};
    identity.display_name = "device-" + id;
    identity.device_class = DeviceClass::Desktop;
    identity.os_name = "test-os";
    identity.trust_state = trust;
    return identity;
}

Transfer make_transfer(const std::string& id, const std::string& name,
    std::uint64_t total) {
    Transfer transfer;
    transfer.id = TransferId{id};
    transfer.sender = DeviceId{"local-1"};
    transfer.receiver = DeviceId{"alpha-01"};
    transfer.file = aki::transfer::FileMetadata{name, total, "text/plain"};
    return transfer;
}

// 恢复数据 → AppState（宿主的播种对接形态，§11.1 ②）。
AppState seed_from(const aki::persistence::RecoveredData& data) {
    AppState state;
    state.devices.devices = data.devices;
    state.conversations.conversations = data.conversations;
    state.messages.messages = data.messages;
    state.transfers.transfers = data.transfers;
    return state;
}

}  // namespace

// ---- 验收 ①：写入 → 受控关闭 → 重新 open 逐域一致（SCOPE-09）----

TEST_CASE("Restart recovery restores every domain after a drained shutdown",
    "[integration][restart_recovery]") {
    const std::string root = temp_root("restart");

    ExecutorOwner owner;
    REQUIRE(owner.initialize());

    // 启动恢复（主线程同步，§11.1 ②）：空库首开 → 迁移 1 步 + 全域空。
    RecoveryResult first = perform_startup_recovery(root);
    REQUIRE(first.diagnostics.migrations_applied == 1);
    REQUIRE(first.diagnostics.tmp_orphans_removed == 0);
    REQUIRE(first.state.devices.empty());
    REQUIRE(first.state.conversations.empty());
    REQUIRE(first.state.messages.empty());
    REQUIRE(first.state.transfers.empty());

    // 播种对接：加载结果经 AppStateOwner 构造入参成为初始快照（立即可读）。
    AppStateOwner state_owner{AppStateOwnerOptions{}, seed_from(first.state)};
    executor::comm::Snapshot<AppState> seeded;
    REQUIRE(state_owner.try_load_snapshot(seeded));
    REQUIRE(seeded.value.messages.messages.empty());

    // 恢复完成后注册 DatabaseWorker（§11.1 ②③；单一连接整体移交）。
    auto control = std::make_shared<DatabaseWorkerControl>(
        std::move(first.repositories));
    executor::BlockingWorkerSpec spec;
    spec.name = "aki.db-worker";
    spec.config.thread_name = "aki-db-worker";
    spec.worker = std::make_unique<DatabaseWorkerRunnable>(control);
    REQUIRE(owner.start_blocking_worker(std::move(spec)));
    control->mark_registered();

    // ---- 写入（§11.1 ① 映射；FIFO = 接受顺序）----
    std::vector<std::future<void>> futures;
    const auto track = [&](DbJob job) {
        futures.push_back(job.done->get_future());
        REQUIRE(control->enqueue(std::move(job)));
    };

    // 设备 + 信任状态（整行 upsert 推进 Unknown -> Pending -> Trusted）。
    track(aki::persistence::make_device_upsert_job(
        make_device("local-1", TrustState::Unknown)));
    DeviceIdentity alpha = make_device("alpha-01", TrustState::Unknown);
    track(aki::persistence::make_device_upsert_job(alpha));
    alpha.trust_state = TrustState::Pending;
    track(aki::persistence::make_device_upsert_job(alpha));
    alpha.trust_state = TrustState::Trusted;
    track(aki::persistence::make_device_upsert_job(alpha));

    // 会话。
    Conversation conversation;
    conversation.id = ConversationId{"conv-alpha-01"};
    conversation.local_device = DeviceId{"local-1"};
    conversation.remote_device = DeviceId{"alpha-01"};
    conversation.state = ConversationState::Active;
    track(aki::persistence::make_conversation_upsert_job(conversation));

    // 消息含送达终态（m-1 Sent -> Delivered；m-2 收到即 Delivered）。
    Message m1;
    m1.id = MessageId{"m-1"};
    m1.sender = DeviceId{"local-1"};
    m1.receiver = DeviceId{"alpha-01"};
    m1.timestamp = std::chrono::system_clock::now();
    m1.type = MessageType::Text;
    m1.payload = TextPayload{"hello alpha"};
    m1.state = DeliveryState::Sent;
    track(aki::persistence::make_message_upsert_job(
        m1, ConversationId{"conv-alpha-01"}));
    track(aki::persistence::make_message_delivery_job(
        MessageId{"m-1"}, DeliveryState::Delivered));
    Message m2;
    m2.id = MessageId{"m-2"};
    m2.sender = DeviceId{"alpha-01"};
    m2.receiver = DeviceId{"local-1"};
    m2.timestamp = std::chrono::system_clock::now();
    m2.type = MessageType::Text;
    m2.payload = TextPayload{"hello local"};
    m2.state = DeliveryState::Delivered;
    track(aki::persistence::make_message_upsert_job(
        m2, ConversationId{"conv-alpha-01"}));

    // 传输历史：t-1 字节源驱动 Completed（文件作业组）+ t-2 Cancelled。
    const std::string payload = "restart recovery payload";
    first.store->write_part("t-1", bytes_of(payload));
    Transfer t1 = make_transfer("t-1", "notes.txt", payload.size());
    t1.state = TransferState::Transferring;
    t1.transferred = 6;
    t1.total = payload.size();
    track(aki::persistence::make_transfer_upsert_job(t1));
    track(aki::persistence::make_transfer_progress_job(
        TransferId{"t-1"}, payload.size(), payload.size()));
    track(aki::persistence::make_transfer_complete_job(first.store, "t-1"));

    first.store->write_part("t-2", bytes_of("partial"));
    Transfer t2 = make_transfer("t-2", "aborted.bin", 4096);
    t2.state = TransferState::Queued;
    track(aki::persistence::make_transfer_upsert_job(t2));
    track(aki::persistence::make_transfer_terminal_job(
        TransferId{"t-2"}, TransferState::Cancelled));
    track(aki::persistence::make_transfer_discard_job(first.store, "t-2"));

    // ---- 受控关闭（§11.1 ③：排空位于钩子末尾，close() 之后）----
    const auto report = owner.shutdown([&] {
        state_owner.close();
        control->request_drain();
        REQUIRE(wait_until([&] { return control->drain_completed(); }, 3s));
    });
    REQUIRE(report.fully_stopped());
    REQUIRE(report.blocking_workers_stopped == 1);
    REQUIRE(control->drain_completed());
    REQUIRE_FALSE(control->drain_budget_exhausted());

    for (auto& future : futures) {
        REQUIRE_NOTHROW(future.get());  // 零丢失：全部正常结算
    }
    REQUIRE(control->completed_count() == futures.size());
    REQUIRE(control->failed_count() == 0);
    REQUIRE(control->rejected_count() == 0);

    // ---- 重新 open 后逐域断言一致（SCOPE-09）----
    RecoveryResult reopened = perform_startup_recovery(root);
    REQUIRE(reopened.diagnostics.migrations_applied == 0);  // 迁移幂等
    REQUIRE(reopened.diagnostics.tmp_orphans_removed == 0); // 干净关闭无残留

    // 设备（presence 易失：恢复后 Offline，§11.1 ①）。
    REQUIRE(reopened.state.devices.size() == 2);
    REQUIRE(reopened.state.devices[0].id == DeviceId{"local-1"});
    REQUIRE(reopened.state.devices[0].trust_state == TrustState::Unknown);
    REQUIRE(reopened.state.devices[0].presence == PresenceState::Offline);
    REQUIRE(reopened.state.devices[1].id == DeviceId{"alpha-01"});
    REQUIRE(reopened.state.devices[1].trust_state == TrustState::Trusted);
    REQUIRE(reopened.state.devices[1].display_name == "device-alpha-01");
    REQUIRE(reopened.state.devices[1].device_class == DeviceClass::Desktop);
    REQUIRE(reopened.state.devices[1].os_name == "test-os");

    // 会话。
    REQUIRE(reopened.state.conversations.size() == 1);
    REQUIRE(reopened.state.conversations[0].id
        == ConversationId{"conv-alpha-01"});
    REQUIRE(reopened.state.conversations[0].state == ConversationState::Active);
    REQUIRE(reopened.state.conversations[0].local_device == DeviceId{"local-1"});
    REQUIRE(reopened.state.conversations[0].remote_device
        == DeviceId{"alpha-01"});

    // 消息含送达终态（时间戳与 payload 无损往返）。
    REQUIRE(reopened.state.messages.size() == 2);
    REQUIRE(reopened.state.messages[0].id == MessageId{"m-1"});
    REQUIRE(reopened.state.messages[0].state == DeliveryState::Delivered);
    REQUIRE(reopened.state.messages[0].timestamp == m1.timestamp);
    REQUIRE(std::get_if<TextPayload>(&reopened.state.messages[0].payload)
        != nullptr);
    REQUIRE(std::get<TextPayload>(reopened.state.messages[0].payload).text
        == "hello alpha");
    REQUIRE(reopened.state.messages[1].id == MessageId{"m-2"});
    REQUIRE(reopened.state.messages[1].state == DeliveryState::Delivered);
    REQUIRE(reopened.state.messages[1].timestamp == m2.timestamp);

    // 传输历史（t-1 Completed + 进度收敛；t-2 Cancelled）。
    REQUIRE(reopened.state.transfers.size() == 2);
    REQUIRE(reopened.state.transfers[0].id == TransferId{"t-1"});
    REQUIRE(reopened.state.transfers[0].state == TransferState::Completed);
    REQUIRE(reopened.state.transfers[0].transferred == payload.size());
    REQUIRE(reopened.state.transfers[0].total == payload.size());
    REQUIRE(reopened.state.transfers[0].file.name == "notes.txt");
    REQUIRE(reopened.state.transfers[1].id == TransferId{"t-2"});
    REQUIRE(reopened.state.transfers[1].state == TransferState::Cancelled);

    // Completed 文件本体与 stored_* 回写位（M2-06 契约）。
    REQUIRE(std::filesystem::exists(root + "/files/t-1/notes.txt"));
    REQUIRE_FALSE(std::filesystem::exists(root + "/files/tmp/t-1.part"));
    REQUIRE_FALSE(std::filesystem::exists(root + "/files/tmp/t-2.part"));
    aki::persistence::Statement stored =
        reopened.repositories->database.prepare(
            "SELECT stored_relative_path, stored_sha256, stored_size_bytes"
            " FROM transfer WHERE transfer_id = 't-1';");
    REQUIRE(stored.step());
    REQUIRE(stored.column_text(0) == "files/t-1/notes.txt");
    REQUIRE(stored.column_text(1) == aki::persistence::sha256_hex(bytes_of(payload)));
    REQUIRE(static_cast<std::uint64_t>(stored.column_int64(2))
        == payload.size());

    // 播种对接：恢复结果构造 AppStateOwner，初始快照立即可读（§11.1 ②）。
    AppStateOwner reopened_owner{AppStateOwnerOptions{},
        seed_from(reopened.state)};
    executor::comm::Snapshot<AppState> snapshot;
    REQUIRE(reopened_owner.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.devices.devices.size() == 2);
    REQUIRE(snapshot.value.messages.messages.size() == 2);
    REQUIRE(snapshot.value.transfers.transfers.size() == 2);
    REQUIRE(snapshot.value.transfers.transfers[0].state
        == TransferState::Completed);
}

// ---- 验收 ②：DB 文件损坏时 open 干净失败、不静默 ----

TEST_CASE("Corrupted database file fails recovery open cleanly",
    "[integration][restart_recovery]") {
    const std::string root = temp_root("corrupt");
    std::filesystem::create_directories(root + "/db");
    {
        std::ofstream out(root + "/db/aki.db3", std::ios::binary);
        out << "this file is not a sqlite database (M2-07 corrupt-db fixture)";
    }

    try {
        (void)perform_startup_recovery(root);
        FAIL("expected SqliteError for corrupted database file");
    } catch (const aki::persistence::SqliteError& error) {
        // 干净失败：主错误码 + errmsg 语义可见（SQLITE_NOTADB = 26）。
        CHECK(error.code() == 26);
        CHECK_FALSE(std::string(error.what()).empty());
    }

    // 失败不残留半初始化状态：同组件对有效根仍正常工作。
    const std::string other = temp_root("corrupt-clean");
    RecoveryResult ok = perform_startup_recovery(other);
    CHECK(ok.diagnostics.migrations_applied == 1);
    CHECK(ok.state.devices.empty());
}

// ---- 启动清扫（§11.1 ②：按加载到的活动 Transfer 行清扫 files/tmp/）----

TEST_CASE("Recovery sweep keeps active parts and removes orphans",
    "[integration][restart_recovery]") {
    const std::string root = temp_root("sweep");

    // 第一段：写入活动 + 终态两行传输后关闭连接（行存在、.part 尚未写入）。
    {
        RecoveryResult first = perform_startup_recovery(root);
        Transfer active = make_transfer("t-active", "a.bin", 10);
        active.state = TransferState::Transferring;
        first.repositories->transfers.upsert(active);
        Transfer done = make_transfer("t-done", "b.bin", 10);
        done.state = TransferState::Completed;
        first.repositories->transfers.upsert(done);
    }  // Repositories 析构关闭连接。

    // 崩溃残留：活动行、终态行、无行三种 .part。
    aki::persistence::FileStore planter(root);
    planter.write_part("t-active", bytes_of("active"));
    planter.write_part("t-done", bytes_of("done"));
    planter.write_part("t-unknown", bytes_of("unknown"));

    RecoveryResult second = perform_startup_recovery(root);
    CHECK(second.diagnostics.tmp_orphans_removed == 2);  // 终态 + 无行
    CHECK(second.state.transfers.size() == 2);
    aki::persistence::FileStore store(root);
    CHECK(store.part_exists("t-active"));   // 活动行保留
    CHECK_FALSE(store.part_exists("t-done"));
    CHECK_FALSE(store.part_exists("t-unknown"));
}

// ---- 宿主路径 shutdown 排空不丢作业（M2 退出-1 复验；M2-05 DOD-02 宿主组合）----

TEST_CASE("Host-style shutdown drain loses no admitted jobs",
    "[integration][restart_recovery]") {
    const std::string root = temp_root("drain");
    ExecutorOwner owner;
    REQUIRE(owner.initialize());

    RecoveryResult recovery = perform_startup_recovery(root);
    auto control = std::make_shared<DatabaseWorkerControl>(
        std::move(recovery.repositories));
    executor::BlockingWorkerSpec spec;
    spec.name = "aki.db-worker";
    spec.config.thread_name = "aki-db-worker";
    spec.worker = std::make_unique<DatabaseWorkerRunnable>(control);
    REQUIRE(owner.start_blocking_worker(std::move(spec)));
    control->mark_registered();

    // 批量入队、不逐个等待（宿主关闭前的在途形态）。
    constexpr int kJobs = 25;
    std::vector<std::future<void>> futures;
    for (int i = 0; i < kJobs; ++i) {
        DbJob job = aki::persistence::make_device_upsert_job(
            make_device("d-" + std::to_string(i), TrustState::Trusted));
        futures.push_back(job.done->get_future());
        REQUIRE(control->enqueue(std::move(job)));
    }

    const auto report = owner.shutdown([&] {
        control->request_drain();
        REQUIRE(wait_until([&] { return control->drain_completed(); }, 3s));
    });
    REQUIRE(report.fully_stopped());

    for (auto& future : futures) {
        REQUIRE_NOTHROW(future.get());  // admitted 零丢失
    }
    REQUIRE(control->completed_count() == kJobs);
    REQUIRE(control->failed_count() == 0);
    REQUIRE(control->rejected_count() == 0);

    // 作业真实落库（重开断言）。
    RecoveryResult reopened = perform_startup_recovery(root);
    REQUIRE(reopened.state.devices.size() == kJobs);
}
