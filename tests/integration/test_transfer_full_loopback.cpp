// M4-06：双端传输全链路回环（SCOPE-07/08；单用例二进制，[skip] 受控退出
// 纪律沿 M3-05/M4-03~05——不得追加先于 [skip] 门的新用例，工程规范第 7 节）。
//
// 全链路（网络依赖）：发现 → 信任（配对 scope 含 file.push:inbox）→ 图片
// 消息（hash-first，stored_sha256 随载荷）→ 文件传输（Aki 归档 + wire）→
// 暂停/恢复（wire paused 事件 + 控制命令）→ 终态文件本体 SHA-256 一致 →
// 对账断言「消息 media.stored_sha256 == 传输行 stored_sha256」（DEC-012⑤，
// 发送侧查 DB）→ 关闭后传输行落库、消息快照保持。同时动态核实 DEC-012 风险④
// （FileTransferEvent.direction 接收侧取值——以事件驱动计数观察）。
// 防火墙拦截至端 TLS 时输出 [skip] + 受控退出（不冒充已验证；补跑条件见
// M4 里程碑验证记录，与 M3/M4-03~05 登记同批）。
#include "app/application/image_flow.hpp"
#include "app/application/router_sink.hpp"
#include "app/application/transfer_manager.hpp"
#include "app/lifecycle/executor_owner.hpp"
#include "app/state/app_state_owner.hpp"
#include "heyaki/adapter/heyaki_node_adapter.hpp"
#include "heyaki/adapter/lan_discovery.hpp"
#include "heyaki/adapter/local_identity.hpp"
#include "heyaki/session/runtime_node.hpp"
#include "persistence/database/database.hpp"
#include "persistence/database/database_worker_adapter.hpp"
#include "persistence/migration/migration.hpp"
#include "persistence/migration/schema_v1.hpp"
#include "persistence/repository/repositories.hpp"
#include "persistence/repository/update_jobs.hpp"
#include "persistence/storage/file_jobs.hpp"
#include "persistence/storage/file_store.hpp"
#include "persistence/storage/sha256.hpp"
#include "persistence/storage/transfer_io_worker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using aki::app::AppStateOwner;
using aki::app::ExecutorOwner;
using aki::app::TransferManager;
using aki::app::TransferManagerOptions;
using aki::device::DeviceId;
using aki::heyaki::HeyakiNodeAdapter;
using aki::heyaki::LanDiscoveryPipeline;
using aki::heyaki::LocalProfile;
using aki::heyaki::NodeSession;
using aki::persistence::FileStore;
using aki::persistence::TransferIoControl;
using aki::persistence::TransferIoRunnable;
using aki::persistence::TransferIoWorkerOptions;
using aki::persistence::sha256_hex;
using aki::transfer::FileMetadata;
using aki::transfer::TransferState;

int g_counter = 0;

std::string temp_root(const std::string& tag) {
    auto path = std::filesystem::temp_directory_path()
        / ("aki-full-loop-" + tag + "-"
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
        std::this_thread::yield();
    }
    return predicate();
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> chars{std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>()};
    const auto* data = reinterpret_cast<const std::byte*>(chars.data());
    return {data, data + chars.size()};
}

struct NodeDomain {
    explicit NodeDomain(const std::string& root, bool with_receive_root)
        : profile(LocalProfile::open(root, "test-local-password")),
          executor_options([] {
              aki::app::ExecutorOwnerOptions options;
              options.executor_config.min_threads = 2;
              options.executor_config.max_threads = 6;
              return options;
          }()),
          owner(executor_options) {
        if (!owner.initialize()) {
            throw std::runtime_error("node domain: executor initialize failed");
        }
        std::vector<::heyaki::FileRootConfig> receive_roots;
        if (with_receive_root) {
            const std::string receive_dir = root + "/receive/inbox";
            std::error_code ec;
            std::filesystem::create_directories(receive_dir, ec);
            receive_roots.push_back(::heyaki::FileRootConfig{
                .name = "inbox", .directory = receive_dir});
        }
        session.emplace(NodeSession::create(owner.executor(),
            {.profile = &profile,
                .file_receive_roots = std::move(receive_roots)}));
    }

    LocalProfile profile;
    aki::app::ExecutorOwnerOptions executor_options;
    ExecutorOwner owner;
    std::optional<NodeSession> session;
};

struct ShutdownGuard {
    std::function<void()> stop;
    ~ShutdownGuard() {
        if (!stop) return;
        try {
            stop();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "test cleanup failed: %s\n", error.what());
        }
    }
};

}  // namespace

TEST_CASE("Two-node full transfer chain: image message, archive, control, "
    "terminal reconciliation, shutdown persistence",
    "[integration][transfer_full_loopback]") {
    const std::string root = temp_root("a");
    ExecutorOwner host;
    REQUIRE(host.initialize());

    const std::string b_root = temp_root("b");
    NodeDomain domain_a(root, /*with_receive_root=*/false);
    NodeDomain domain_b(b_root, /*with_receive_root=*/true);
    auto& side_a = *domain_a.session;
    auto& side_b = *domain_b.session;
    const auto identity_a = domain_a.profile.identity();
    const auto identity_b = domain_b.profile.identity();
    if (!side_a.has_lan_interfaces() || !side_b.has_lan_interfaces()) {
        std::printf("[skip] no LAN interface\n");
        std::fflush(nullptr);
        std::_Exit(0);
    }

    // 发现 + 连接 + 配对（scope 含 file.push:inbox——DEC-012⑥）。
    LanDiscoveryPipeline pipeline(host.executor(), side_a, [](const auto&) {});
    REQUIRE(pipeline.start(200ms));
    REQUIRE(wait_until([&] {
        for (const auto& entry : side_a.endpoints()) {
            if (entry.device_id == identity_b.id) {
                return true;
            }
        }
        return false;
    }, 15s));
    REQUIRE(side_a.connect_lan(identity_b.id));
    if (!wait_until(
            [&] { return side_a.session_pairing_restricted(identity_b.id); },
            15s)) {
        std::printf(
            "[skip] pairing handshake blocked (firewall): full transfer "
            "loopback not verified; rerun with inbound TCP allowed\n");
        std::fflush(nullptr);
        std::_Exit(0);
    }
    REQUIRE(wait_until(
        [&] { return side_b.session_pairing_restricted(identity_a.id); }, 15s));
    std::atomic<bool> paired_a{false};
    side_a.set_pairing_observer(
        [&](const DeviceId& peer, bool ok, const std::string&) {
            if (ok && peer == identity_b.id) paired_a.store(true);
        });
    REQUIRE(side_a.pair_peer(identity_b.id, "test-local-password"));
    if (!wait_until([&] {
            return paired_a.load() && side_a.session_authenticated(identity_b.id)
                && side_b.session_authenticated(identity_a.id);
        }, 20s)) {
        std::printf("[skip] pairing handshake did not complete after "
                    "submission: full transfer loopback not verified; rerun "
                    "with inbound TCP allowed\n");
        std::fflush(nullptr);
        std::_Exit(0);
    }

    // B 侧 wire 事件观察（DEC-012 风险④ 动态核实：direction 接收侧取值
    // ——入站 committed 事件的 direction 数值记录输出）。
    std::atomic<int> b_committed{0};
    std::atomic<int> b_direction_log{-1};
    side_b.set_file_event_observer(
        [&](const DeviceId&, const NodeSession::FileTransferEventView& event) {
            if (static_cast<::heyaki::FileTransferPhase>(event.phase)
                == ::heyaki::FileTransferPhase::committed) {
                b_direction_log.store(event.direction);
                b_committed.fetch_add(1);
            }
        });

    // A 侧发送组合（真实 Adapter + 归档 IO worker + TM）。
    auto store = std::make_shared<FileStore>(root);
    auto database = aki::persistence::Database::open(root + "/test-state.sqlite");
    REQUIRE(aki::persistence::Migrator{aki::persistence::schema_steps()}
        .bring_up_to_date(database) == 3);
    auto db = std::make_shared<aki::persistence::DatabaseWorkerControl>(
        std::make_unique<aki::persistence::Repositories>(std::move(database)));
    std::vector<std::future<void>> db_futures;
    aki::persistence::TransferIoWorkerOptions io_options;
    io_options.chunk_bytes = 16;
    auto io = std::make_shared<TransferIoControl>(store, io_options);
    AppStateOwner state_owner{aki::app::AppStateOwnerOptions{}, {},
        [&](const aki::app::AppStateUpdate& update) {
            std::optional<aki::persistence::DbJob> job;
            std::visit([&](const auto& concrete) {
                using Update = std::decay_t<decltype(concrete)>;
                if constexpr (std::is_same_v<Update, aki::app::UpsertTransfer>) {
                    job = aki::persistence::make_transfer_upsert_job(
                        concrete.transfer);
                } else if constexpr (std::is_same_v<Update,
                                         aki::app::UpdateTransferProgress>) {
                    job = aki::persistence::make_transfer_progress_job(
                        concrete.transfer, concrete.transferred, concrete.total);
                } else if constexpr (std::is_same_v<Update, aki::app::CompleteTransfer>) {
                    job = concrete.final_state == TransferState::Completed
                        ? aki::persistence::make_transfer_complete_job(
                            store, concrete.transfer.value)
                        : aki::persistence::make_transfer_terminal_job(
                            concrete.transfer, concrete.final_state);
                }
            }, update);
            if (!job) return;
            if (db_futures.size() >= 1024) {
                throw std::runtime_error("test DB job budget exceeded");
            }
            db_futures.push_back(job->done->get_future());
            if (!db->enqueue(std::move(*job))) {
                throw std::runtime_error("test DB job rejected");
            }
        }};
    HeyakiNodeAdapter adapter{host.executor(),
        {.profile = &domain_a.profile,
            .session = &side_a,
            .conversation_for =
                [](const DeviceId& remote) {
                    return aki::conversation::ConversationId{
                        std::string{"conv-"} + remote.value};
                },
            .peer_observation = false}};
    aki::app::DeviceManager devices{host.executor(), state_owner, adapter};
    aki::app::ConversationManager conversations{host.executor(), state_owner};
    aki::app::MessageManagerOptions message_options;
    message_options.pump.name = "aki.mm";
    message_options.local_device = identity_a.id;
    aki::app::MessageManager messages{host.executor(), state_owner, adapter,
        message_options};
    TransferManagerOptions transfer_options;
    transfer_options.pump.name = "aki.tm";
    transfer_options.sender = identity_a.id;
    transfer_options.io = io.get();
    TransferManager transfers{host.executor(), state_owner, adapter,
        transfer_options};
    aki::app::RouterSink router{devices, conversations, messages, transfers};
    adapter.set_sink(&router);
    executor::BlockingWorkerSpec db_spec;
    db_spec.name = "aki.test-db";
    db_spec.config.thread_name = "aki-test-db";
    db_spec.worker = std::make_unique<aki::persistence::DatabaseWorkerRunnable>(db);
    REQUIRE(host.start_blocking_worker(std::move(db_spec)));
    db->mark_registered();
    REQUIRE(conversations.ensure_conversation(identity_a.id, identity_b.id));
    REQUIRE(conversations.flush(2s));
    state_owner.drain();

    auto io_runnable = std::make_unique<TransferIoRunnable>(io->impl());
    executor::BlockingWorkerSpec io_spec;
    io_spec.name = "aki.transfer-io";
    io_spec.config.thread_name = "aki-transfer-io";
    io_spec.worker = std::move(io_runnable);
    REQUIRE(host.start_blocking_worker(std::move(io_spec)));
    // Assertion failures must also stop producers/workers while their
    // callbacks still have live Manager and observer owners.
    ShutdownGuard shutdown{[&] {
        pipeline.stop();
        (void)transfers.request_cancel_all();
        (void)transfers.flush(2s);
        (void)side_a.shutdown();
        (void)side_b.shutdown();
        adapter.set_sink(nullptr);
        (void)domain_a.owner.shutdown();
        (void)domain_b.owner.shutdown();
        state_owner.close();
        db->request_drain();
        (void)wait_until([&] { return db->drain_completed(); }, 2s);
        (void)host.shutdown();
    }};

    // 源文件（48B，chunk 16 → 三块）与规范 TransferId。
    const auto source = std::filesystem::path{root} / "payload.bin";
    {
        std::ofstream out(source, std::ios::binary | std::ios::trunc);
        const std::string bytes(48, 'f');
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    const auto send_id = NodeSession::new_transfer_id();
    const FileMetadata file{"payload.bin", 48, "application/octet-stream",
        std::string{}};

    // ① 图片消息 + 归档（hash-first；消息等 hash 完成后发出）。
    std::string got_hash;
    const auto expected_hash = sha256_hex(read_bytes(source));
    REQUIRE(aki::app::send_image_message_with_hash(transfers, messages,
        identity_b.id, aki::conversation::MessageId{"m-full"}, file, send_id,
        source) == aki::app::ImageSendFlowResult::Submitted);
    REQUIRE(wait_until([&] {
        state_owner.drain();
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        if (!state_owner.try_load_snapshot(snapshot)) {
            return false;
        }
        for (const auto& message : snapshot.value.messages.messages) {
            if (message.id == aki::conversation::MessageId{"m-full"}) {
                const auto* image =
                    std::get_if<aki::conversation::ImagePayload>(
                        &message.payload);
                if (image == nullptr) return false;
                got_hash = image->media.stored_sha256;
                return got_hash == expected_hash;
            }
        }
        return false;
    }, 10s));

    // ② 暂停/恢复（控制命令；A 归档走 wire paused 面的动态断言随补跑）。
    REQUIRE(transfers.pause_transfer(send_id));
    (void)wait_until([&] { return transfers.flush(200ms); }, 2s);
    REQUIRE(transfers.resume_transfer(send_id));

    // ③ 接收侧 committed（B 接收根落盘完成）+ 对账素材。
    REQUIRE(wait_until([&] { return b_committed.load() >= 1; }, 30s));
    // DEC-012 风险④ 动态核实输出（回环可达时记录 direction 取值）。
    std::printf("    [probe] inbound committed direction=%d (push=%d)\n",
        b_direction_log.load(),
        static_cast<int>(::heyaki::FileTransferDirection::push));
    std::fflush(nullptr);
    const auto b_file =
        std::filesystem::path{b_root} / "receive" / "inbox"
        / file.name;
    REQUIRE(wait_until([&] {
        std::error_code ec;
        return std::filesystem::file_size(b_file, ec) == 48;
    }, 10s));
    REQUIRE(sha256_hex(read_bytes(b_file)) == expected_hash);  // 本体一致

    // ④ 对账断言（DEC-012⑤）：发送行 stored_sha256 == 消息 media.stored_sha256
    //    （发送侧 complete 作业组 .part 供源回写；Aki 侧归档完成后 committed
    //    事件由 wire 经 Adapter/RouterSink 分发，不注入伪造终态）。
    REQUIRE(wait_until([&] { return transfers.flush(200ms); }, 5s));
    executor::comm::Snapshot<aki::app::AppState> terminal_snapshot;
    const bool terminal_seen = wait_until([&] {
        (void)transfers.flush(100ms);
        state_owner.drain();
        if (!state_owner.try_load_snapshot(terminal_snapshot)) {
            return false;
        }
        for (const auto& row : terminal_snapshot.value.transfers.transfers) {
            if (row.id == send_id) {
                return row.state == TransferState::Completed;
            }
        }
        return false;
    }, 10s);
    INFO("state update rejections=" << state_owner.stats().updates_rejected
        << ", transfer handler rejections=" << transfers.stats().handler_rejections);
    std::string transfer_states;
    for (const auto& row : terminal_snapshot.value.transfers.transfers) {
        transfer_states += std::string{aki::transfer::to_string(row.state)} + " ";
    }
    INFO("transfer states=" << transfer_states);
    REQUIRE(terminal_seen);
    for (auto& future : db_futures) {
        REQUIRE(future.wait_for(2s) == std::future_status::ready);
        future.get();
    }
    REQUIRE(state_owner.stats().post_accept_failures == 0);
    const auto a_final =
        std::filesystem::path{root} / "files" / send_id.value
        / "payload.bin";
    REQUIRE(std::filesystem::exists(a_final));
    REQUIRE(sha256_hex(read_bytes(a_final)) == expected_hash);
    // 发送侧最终文件与消息载荷哈希一致；DB 回写在 worker 关闭后核对。
    REQUIRE(got_hash == expected_hash);

    // ⑤ 受控关闭后保留消息快照并读取已落库传输行；不冒充进程重启恢复。
    pipeline.stop();
    (void)transfers.request_cancel_all();
    REQUIRE(transfers.flush(2s));
    const auto ra = side_a.shutdown();
    REQUIRE(ra.node_stopped);
    const auto rb = side_b.shutdown();
    REQUIRE(rb.node_stopped);
    adapter.set_sink(nullptr);
    (void)domain_a.owner.shutdown();
    (void)domain_b.owner.shutdown();
    executor::comm::Snapshot<aki::app::AppState> final_snapshot;
    REQUIRE(state_owner.try_load_snapshot(final_snapshot));
    // 终态行 + 消息行（含 stored_sha256）在关闭后的快照中保持。
    bool row_terminal = false;
    bool message_intact = false;
    for (const auto& row : final_snapshot.value.transfers.transfers) {
        if (row.id == send_id) {
            row_terminal = row.state == TransferState::Completed;
        }
    }
    for (const auto& message : final_snapshot.value.messages.messages) {
        if (message.id == aki::conversation::MessageId{"m-full"}) {
            const auto* image =
                std::get_if<aki::conversation::ImagePayload>(&message.payload);
            message_intact = image != nullptr
                && image->media.stored_sha256 == expected_hash;
        }
    }
    REQUIRE(row_terminal);
    REQUIRE(message_intact);

    state_owner.close();
    db->request_drain();
    REQUIRE(wait_until([&] { return db->drain_completed(); }, 2s));
    const auto report = host.shutdown();
    shutdown.stop = {};
    REQUIRE(report.fully_stopped());
    REQUIRE(io->idle());
    const auto stored = db->repositories().transfers.find(send_id);
    REQUIRE(stored.has_value());
    REQUIRE(stored->state == TransferState::Completed);
    auto stored_hash = db->repositories().database.prepare(
        "SELECT stored_sha256 FROM transfer WHERE transfer_id = ?1");
    stored_hash.bind(1, send_id.value);
    REQUIRE(stored_hash.step());
    REQUIRE(stored_hash.column_text(0) == expected_hash);
}
