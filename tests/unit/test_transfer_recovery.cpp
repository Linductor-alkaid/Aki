// M4-06：重启恢复语义与全链路组合断言（DEC-013/DEC-012⑤；网络无关，无
// [skip] 受控退出——始终完整执行）。
//
// 覆盖：
//   - 孤儿活动行重启降级（DEC-013①）：非终态行经 perform_startup_recovery
//     改写 Paused（诊断计数 + 落库）+ 先于清扫（.part 保留）+ 终态行不动；
//   - 播种推进（DEC-013②）：TM 注入播种行后 wire 进度事件走「已知的
//     Paused 行」路径（Paused→Transferring 合法推进）+ started 一次性
//     可见拒绝计入基线；
//   - 无会话 cancel 直接终态写入（发送行唯一出口）：行缺失可见拒绝 +
//     存在行 Cancelled 收敛（discard 作业经处理器入队）；
//   - 全链路组合断言（Fake/wire 事件组合）：发送归档（hash-first）→ 图片
//     消息（stored_sha256 携带）→ 接收合并（DB 终态组自接收根供源）→
//     对账断言「消息 media.stored_sha256 == 传输行 stored_sha256」
//    （DEC-012⑤）→ 重启一致。
#include "app/application/image_flow.hpp"
#include "app/application/transfer_manager.hpp"
#include "app/lifecycle/executor_owner.hpp"
#include "app/state/app_state_owner.hpp"
#include "heyaki/adapter/fake_heyaki_adapter.hpp"
#include "persistence/database/database.hpp"
#include "persistence/database/database_worker.hpp"
#include "persistence/database/database_worker_adapter.hpp"
#include "persistence/migration/migration.hpp"
#include "persistence/migration/schema_v1.hpp"
#include "persistence/recovery/startup_recovery.hpp"
#include "persistence/repository/repositories.hpp"
#include "persistence/repository/update_jobs.hpp"
#include "persistence/storage/file_jobs.hpp"
#include "persistence/storage/file_store.hpp"
#include "persistence/storage/sha256.hpp"
#include "persistence/storage/transfer_io_worker.hpp"

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
#include <stdexcept>
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
using aki::persistence::perform_startup_recovery;
using aki::persistence::TransferIoControl;
using aki::persistence::TransferIoRunnable;
using aki::persistence::TransferIoWorkerOptions;
using aki::persistence::schema_v1_steps;
using aki::persistence::sha256_hex;
using aki::persistence::Statement;
using aki::transfer::FileMetadata;
using aki::transfer::Transfer;
using aki::transfer::TransferId;
using aki::transfer::TransferState;

int g_counter = 0;

std::string temp_root(const std::string& tag) {
    auto path = std::filesystem::temp_directory_path()
        / ("aki-m406-" + tag + "-"
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

// 直接建库（迁移 + 仓储写行），供恢复段用例播种非终态/终态行。
void seed_database(const std::string& root,
    const std::vector<Transfer>& transfers) {
    Database database = Database::open(
        (std::filesystem::path{root} / "db" / "aki.db3").string());
    const Migrator seed_migrator{schema_v1_steps()};
    if (seed_migrator.bring_up_to_date(database) != 1) {
        throw std::runtime_error("seed: migration failed");
    }
    aki::persistence::Repositories repos{std::move(database)};
    for (const auto& transfer : transfers) {
        repos.transfers.upsert(transfer);
    }
}

}  // namespace

// ---- 孤儿降级：非终态改写 Paused + 先于清扫 + 终态不动 ----

TEST_CASE("Startup recovery demotes orphan active rows to Paused before "
    "sweeping",
    "[unit][m406][recovery]") {
    const std::string root = temp_root("orphan");
    // 预置：1 个在途发送行（.part 残留）+ 1 个在途接收行 + 1 个终态行。
    std::error_code ec;
    std::filesystem::create_directories(root + "/db", ec);
    seed_database(root,
        {Transfer{TransferId{"t-orphan-send"}, DeviceId{"local-1"},
             DeviceId{"beta"},
             FileMetadata{"send.bin", 32, std::string{}, ""}, 16, 32,
             TransferState::Transferring},
            Transfer{TransferId{"t-orphan-recv"}, DeviceId{"beta"},
                DeviceId{"local-1"},
                FileMetadata{"recv.bin", 16, std::string{}, ""}, 0, 16,
                TransferState::Negotiating},
            Transfer{TransferId{"t-done"}, DeviceId{"local-1"},
                DeviceId{"beta"},
                FileMetadata{"done.bin", 8, std::string{}, ""}, 8, 8,
                TransferState::Completed}});
    // 在途发送行的 .part 残留（清扫按活动行判定保留）。
    FileStore store{root};
    {
        const auto part = store.part_path("t-orphan-send");
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out << "partial";
    }

    const auto recovery = perform_startup_recovery(root);

    // 降级语义（DEC-013①）：非终态 → Paused + 诊断计数；终态不动。
    REQUIRE(recovery.diagnostics.orphan_rows_paused == 2);
    REQUIRE(recovery.diagnostics.tmp_orphans_removed == 0);  // .part 保留
    REQUIRE(recovery.state.transfers.size() == 3);
    for (const auto& row : recovery.state.transfers) {
        if (row.id == TransferId{"t-done"}) {
            REQUIRE(row.state == TransferState::Completed);  // 终态不动
        } else {
            REQUIRE(row.state == TransferState::Paused);     // 降级
        }
    }
    REQUIRE(store.part_exists("t-orphan-send"));  // 先于清扫 → .part 保留
    // 落库复核（重开 DB 独立连接读回）。
    {
        Database reopened = Database::open(
            (std::filesystem::path{root} / "db" / "aki.db3").string());
        aki::persistence::Repositories repos{std::move(reopened)};
        const auto row = repos.transfers.find(TransferId{"t-orphan-send"});
        REQUIRE(row.has_value());
        REQUIRE(row->state == TransferState::Paused);
    }
}

// ---- 播种推进 + started 一次性可见拒绝 + 无会话 cancel 直接终态 ----

TEST_CASE("Seeded Paused rows resume via wire progress and cancel terminates "
    "without a session",
    "[unit][m406][recovery]") {
    const std::string root = temp_root("seed");
    std::filesystem::create_directories(root + "/db");
    seed_database(root,
        {Transfer{TransferId{"t-seed"}, DeviceId{"beta"}, DeviceId{"local-1"},
             FileMetadata{"seed.bin", 40, std::string{}, ""}, 0, 40,
             TransferState::Negotiating}});
    const auto recovery = perform_startup_recovery(root);
    REQUIRE(recovery.diagnostics.orphan_rows_paused == 1);

    ExecutorOwner host;
    REQUIRE(host.initialize());
    // 播种（组合根同款：恢复数据播种 owner 快照 + 改写后的非终态行注入 TM）。
    aki::app::AppState seed_state;
    seed_state.transfers.transfers = recovery.state.transfers;
    AppStateOwner state_owner{aki::app::AppStateOwnerOptions{}, seed_state};
    TransferManagerOptions transfer_options;
    transfer_options.pump.name = "aki.tm";
    transfer_options.sender = DeviceId{"local-1"};
    for (const auto& row : recovery.state.transfers) {
        if (!aki::transfer::is_terminal(row.state)) {
            transfer_options.seeded_rows.push_back(row);
        }
    }
    FakeHeyakiAdapter adapter;
    TransferManager transfers{host.executor(), state_owner, adapter,
        transfer_options};

    struct RouteSink final : aki::heyaki::HeyakiAdapterSink {
        TransferManager* transfers = nullptr;
        bool on_device_discovered(aki::device::DiscoveredDevice) override {
            return true;
        }
        bool on_device_connected(DeviceId, aki::device::ConnectionPath)
            override {
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
    } sink;
    sink.transfers = &transfers;
    adapter.set_sink(&sink);

    const auto settle = [&] {
        REQUIRE(transfers.flush(2s));
        for (;;) {
            const auto watermark = [](const AppStateOwner& owner) {
                return owner.stats().updates_applied
                    + owner.stats().updates_rejected
                    + owner.stats().events_forwarded
                    + owner.stats().events_dropped;
            };
            const std::uint64_t before = watermark(state_owner);
            state_owner.drain();
            if (watermark(state_owner) == before) {
                return;
            }
        }
    };

    // started(Negotiating) 对 Paused 播种行：一次性可见拒绝（DEC-013 风险②
    // 定案——计入断言基线）。
    Transfer started;
    started.id = TransferId{"t-seed"};
    started.sender = DeviceId{"beta"};
    started.receiver = DeviceId{"local-1"};
    started.file = FileMetadata{"seed.bin", 40, std::string{}, ""};
    started.total = 40;
    started.state = TransferState::Negotiating;
    REQUIRE(adapter.inject_transfer_started(started));
    settle();
    const auto rejected_after_started = state_owner.stats().updates_rejected;
    REQUIRE(rejected_after_started == 1);

    // wire 进度事件走「已知的 Paused 行」路径：Paused → Transferring 合法
    // 推进（DEC-013② 接收行恢复形态）。
    REQUIRE(adapter.inject_transfer_progress(TransferId{"t-seed"}, 16, 40));
    settle();
    {
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        REQUIRE(state_owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.transfers.transfers.front().state
            == TransferState::Transferring);
        REQUIRE(snapshot.value.transfers.transfers.front().transferred == 16);
    }

    // 无会话 cancel（发送行唯一出口，DEC-013②③）：直接终态写入 → Cancelled。
    REQUIRE(transfers.cancel_transfer(TransferId{"t-seed"}));
    settle();
    {
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        REQUIRE(state_owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.transfers.transfers.front().state
            == TransferState::Cancelled);
    }
    // 行缺失的 cancel：owner 拒绝可见（不静默）。
    REQUIRE(transfers.cancel_transfer(TransferId{"t-missing"}));
    settle();
    REQUIRE(state_owner.stats().updates_rejected
        == rejected_after_started + 1);

    const auto report = host.shutdown();
    REQUIRE(report.fully_stopped());
}

// ---- 全链路组合断言（网络无关）：发送归档 + 图片消息 + 接收合并 + 对账 +
//      重启一致（Fake/wire 事件组合，DEC-012⑤ 对账口径）----

TEST_CASE("Full-chain combo: archive, image message, receive merge, "
    "hash reconciliation, restart consistency",
    "[unit][m406][combo]") {
    const std::string root = temp_root("combo");
    ExecutorOwner host;
    REQUIRE(host.initialize());

    auto store = std::make_shared<FileStore>(root);
    const std::string receive_dir = root + "/receive/inbox";
    std::error_code ec;
    std::filesystem::create_directories(receive_dir, ec);

    std::filesystem::create_directories(root + "/db");
    auto database = Database::open(
        (std::filesystem::path{root} / "db" / "aki.db3").string());
    {
        const Migrator combo_migrator{schema_v1_steps()};
        REQUIRE(combo_migrator.bring_up_to_date(database) == 1);
    }
    auto db = std::make_shared<aki::persistence::DatabaseWorkerControl>(
        std::make_unique<aki::persistence::Repositories>(std::move(database)),
        aki::persistence::DatabaseWorkerOptions{});
    std::vector<std::future<void>> db_futures;

    // owner 播种：出站图片消息的远端会话先行（DEC-009 ② FK 前置校验）。
    aki::app::AppState seed_state;
    {
        aki::conversation::Conversation conversation;
        conversation.id = aki::conversation::ConversationId{"conv-beta"};
        conversation.local_device = DeviceId{"local-1"};
        conversation.remote_device = DeviceId{"beta"};
        seed_state.conversations.conversations.push_back(conversation);
    }
    AppStateOwner state_owner{aki::app::AppStateOwnerOptions{}, seed_state,
        [&store, &db, &db_futures, &receive_dir](
            const aki::app::AppStateUpdate& update) {
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
                                                   aki::app::CompleteTransfer>) {
                        std::vector<aki::persistence::DbJob> jobs;
                        if (concrete.final_state == TransferState::Completed) {
                            jobs.push_back(
                                aki::persistence::make_transfer_complete_job(
                                    store, concrete.transfer.value,
                                    receive_dir));
                        } else {
                            jobs.push_back(
                                aki::persistence::make_transfer_terminal_job(
                                    concrete.transfer, concrete.final_state));
                            jobs.push_back(
                                aki::persistence::make_transfer_discard_job(
                                    store, concrete.transfer.value));
                        }
                        for (auto& job : jobs) {
                            db_futures.push_back(job.done->get_future());
                            (void)db->enqueue(std::move(job));
                        }
                    }
                },
                update);
        }};
    FakeHeyakiAdapter adapter;

    // 发送侧链路（TM + 归档 IO 承载面 + hash-first 图片消息）。
    aki::persistence::TransferIoWorkerOptions io_options;
    io_options.chunk_bytes = 8;
    auto io = std::make_shared<aki::persistence::TransferIoControl>(store,
        io_options);
    MessageId image_message_id{"m-combo"};
    aki::app::MessageManagerOptions message_options;
    message_options.pump.name = "aki.mm";
    message_options.local_device = DeviceId{"local-1"};
    aki::app::MessageManager messages{host.executor(), state_owner, adapter,
        message_options};

    TransferManagerOptions transfer_options;
    transfer_options.pump.name = "aki.tm";
    transfer_options.sender = DeviceId{"local-1"};
    transfer_options.io = io.get();
    TransferManager transfers{host.executor(), state_owner, adapter,
        transfer_options};

    struct RouteSink final : aki::heyaki::HeyakiAdapterSink {
        TransferManager* transfers = nullptr;
        bool on_device_discovered(aki::device::DiscoveredDevice) override {
            return true;
        }
        bool on_device_connected(DeviceId, aki::device::ConnectionPath)
            override {
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
    } sink;
    sink.transfers = &transfers;
    adapter.set_sink(&sink);

    auto io_runnable =
        std::make_unique<aki::persistence::TransferIoRunnable>(io->impl());
    executor::BlockingWorkerSpec io_spec;
    io_spec.name = "aki.transfer-io";
    io_spec.config.thread_name = "aki-transfer-io";
    io_spec.worker = std::move(io_runnable);
    REQUIRE(host.start_blocking_worker(std::move(io_spec)));
    auto db_runnable =
        std::make_unique<aki::persistence::DatabaseWorkerRunnable>(db);
    executor::BlockingWorkerSpec db_spec;
    db_spec.name = "aki.db-worker";
    db_spec.config.thread_name = "aki-db-worker";
    db_spec.worker = std::move(db_runnable);
    REQUIRE(host.start_blocking_worker(std::move(db_spec)));
    db->mark_registered();

    // ① 发送归档（hash-first）+ 图片消息（stored_sha256 携带）。
    const std::string send_root = root + "/send";
    std::filesystem::create_directories(send_root, ec);
    const auto source =
        write_source(send_root + "/combo.bin", 24, 'w');
    const FileMetadata file{"combo.bin", 24, "image/png", std::string{}};
    const TransferId id{"t-combo"};
    // 固定规范串（网络无关组合不链 heyaki 生成入口；Fake 面不校验规范形式，
    // 形态与真实链路一致——见 test_transfer_send_path 对生成入口的验证）。
    const TransferId send_id{"hyt1_aaaaaaaaaaaaaaaaaaaaaaaaae"};
    (void)id;

    std::string got_hash;
    std::atomic<bool> hash_ready{false};
    const auto expected_hash_combo = sha256_hex(read_bytes(source));
    REQUIRE(aki::app::send_image_message_with_hash(transfers, messages,
        DeviceId{"beta"}, image_message_id, file, send_id, source)
        == aki::app::ImageSendFlowResult::Submitted);
    // hash-first：消息行 media.stored_sha256 就绪即 hash 完成（v2 流无回调参）。
    REQUIRE(wait_until([&] {
        (void)transfers.flush(100ms);
        (void)messages.flush(100ms);
        state_owner.drain();
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        if (!state_owner.try_load_snapshot(snapshot)) {
            return false;
        }
        for (const auto& message : snapshot.value.messages.messages) {
            if (message.id == image_message_id) {
                const auto* image =
                    std::get_if<aki::conversation::ImagePayload>(
                        &message.payload);
                if (image != nullptr
                    && image->media.stored_sha256 == expected_hash_combo) {
                    got_hash = image->media.stored_sha256;
                    hash_ready.store(true);
                    return true;
                }
            }
        }
        return false;
    }, 5s));
    const auto expected_hash = sha256_hex(read_bytes(source));
    REQUIRE(got_hash == expected_hash);
    REQUIRE(wait_until([&] {
        state_owner.drain();
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        if (!state_owner.try_load_snapshot(snapshot)) {
            return false;
        }
        for (const auto& message : snapshot.value.messages.messages) {
            if (message.id == image_message_id) {
                const auto* image = std::get_if<aki::conversation::ImagePayload>(
                    &message.payload);
                return image != nullptr
                    && image->media.stored_sha256 == expected_hash;
            }
        }
        return false;
    }, 5s));

    // ② 模拟 heyaki 接收侧（同一 TM 不能同时持有同 id 的发送会话与入站行
    // ——发送/接收分属两端进程；组合断言以独立 recv_id 建模本端接收，
    // 内容与发送侧一致以支撑对账）：committed 时接收根已有完整文件。
    const TransferId recv_id{"hyt1_bbbbbbbbbbbbbbbbbbbbbbbbbbe"};
    const auto receive_file =
        std::filesystem::path{receive_dir} / recv_id.value;
    write_source(receive_file, 24, 'w');

    // ③ 接收链路（wire 事件组合）：started(接收行) → progress → committed
    //    → M2-06 终态组自接收根供源合并。
    Transfer inbound;
    inbound.id = recv_id;
    inbound.sender = DeviceId{"beta"};
    inbound.receiver = DeviceId{"local-1"};
    inbound.file = FileMetadata{recv_id.value, 24, std::string{}, ""};
    inbound.total = 24;
    inbound.state = TransferState::Negotiating;
    REQUIRE(adapter.inject_transfer_started(inbound));
    REQUIRE(adapter.inject_transfer_progress(recv_id, 24, 24));
    REQUIRE(adapter.inject_transfer_completed(recv_id, TransferState::Completed));
    REQUIRE(wait_until([&] {
        (void)transfers.flush(100ms);
        state_owner.drain();
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        if (!state_owner.try_load_snapshot(snapshot)) {
            return false;
        }
        for (const auto& row : snapshot.value.transfers.transfers) {
            if (row.id == recv_id) {
                return row.state == TransferState::Completed;
            }
        }
        return false;
    }, 5s));

    // 排空 DB + 关闭（EXEC-01 序）。
    db->request_drain();
    REQUIRE(wait_until([&] { return db->drain_completed(); }, 5s));
    const auto report = host.shutdown([&] {
        (void)transfers.request_cancel_all();
        CHECK(transfers.flush(2s));
        state_owner.close();
        db->request_drain();
    });
    REQUIRE(report.fully_stopped());
    for (auto& future : db_futures) {
        future.get();  // 全部作业成功（含接收合并组）
    }

    // ④ 对账断言（DEC-012⑤）：消息 media.stored_sha256 == 传输行
    //    stored_sha256（发送方哈希 == 接收方实算落盘哈希）。
    Database reopened = Database::open(
        (std::filesystem::path{root} / "db" / "aki.db3").string());
    // 回写列不在域 Transfer（DB-only）：SQL 断言（同 test_transfer_receive_path）。
    std::string stored_relative;
    std::string stored_hash;
    std::uint64_t stored_size = 0;
    {
        Statement statement = reopened.prepare(
            "SELECT stored_relative_path, stored_sha256, stored_size_bytes"
            " FROM transfer WHERE transfer_id = ?1;");
        statement.bind(1, recv_id.value);
        REQUIRE(statement.step());
        stored_relative = statement.column_text(0);
        stored_hash = statement.column_text(1);
        stored_size = static_cast<std::uint64_t>(statement.column_int64(2));
    }
    REQUIRE(stored_relative
        == "files/" + recv_id.value + "/" + recv_id.value);
    REQUIRE(stored_hash == expected_hash);
    REQUIRE(stored_size == 24);
    REQUIRE(got_hash == stored_hash);  // 对账（发送方哈希 == 接收方实算）
    // 文件本体：接收原件同作业删除、最终文件就位。
    const auto final_path =
        std::filesystem::path{root} / "files" / recv_id.value
        / recv_id.value;
    REQUIRE(std::filesystem::exists(final_path));
    REQUIRE(read_bytes(final_path) == read_bytes(source));
    REQUIRE_FALSE(std::filesystem::exists(receive_file));
}
