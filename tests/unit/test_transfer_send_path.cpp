// M4-04：发送侧真实传输数据链路单测（设计 §7.1①③④/DEC-011；网络无关，
// 无 [skip] 受控退出——始终完整执行）。
//
// 覆盖：
//   - 分块边界（空文件 / 非整块尾部 / 多块）+ 归档 .part 内容逐字节一致 +
//     hash-first（on_hash_ready 的 SHA-256 与 sha256_hex 交叉验证）；
//   - 进度聚合（单飞 dirty：一次排空至多一个 UpdateTransferProgress、
//     latest-wins 终值不丢）；
//   - 终态闸门（wire Completed 先于归档 → 持有；归档完成 → 放行；归档失败
//     → 已知边角释放）；
//   - DOD-02 六项沿真实 IO 路径：正常完成（边界用例）、任务异常（源缺失
//     → failed 事件，worker 存活）、提交拒绝（io 承载面停止后 admission
//     拒绝可见）、执行中取消（.part 幂等删除 + 迟到事件忽略）、超时（沿
//     泵排队软超时——test_app_managers 既有覆盖，此处不重复）、shutdown
//     （在飞会话 + fully_stopped + IO 归零）；
//   - 2-worker 小池夹具（M4-03 观察③ 闭环）：双并发归档 + 文本发送泵恒
//     可调度（无池 worker 停占）；
//   - 重启一致性（完整 DB 组合）：CompleteTransfer(Completed) → M2-06 终态
//     作业组（流式 SHA-256 + 原子改名 + 回写位）真实文件源 → 重开 DB 逐列
//     断言 + 文件本体一致。
//   - flush 顺序（TOCTOU 回归，判定式假想 IO）：先判 IO 归零、后最终泵
//     排空——「泵静止与 idle 判定之间落入的 IO 事件」不得以未处理状态
//     滞留（§11.1③/DEC-011 ③）。
#include "app/application/image_flow.hpp"
#include "app/application/message_manager.hpp"
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
using aki::app::MessageManager;
using aki::app::MessageManagerOptions;
using aki::app::ManagerPumpOptions;
using aki::app::TransferManager;
using aki::app::TransferManagerOptions;
using aki::conversation::Conversation;
using aki::conversation::MessageId;
using aki::device::DeviceId;
using aki::heyaki::FakeHeyakiAdapter;
using aki::persistence::Database;
using aki::persistence::FileStore;
using aki::persistence::Migrator;
using aki::persistence::TransferIoControl;
using aki::persistence::TransferIoRunnable;
using aki::persistence::TransferIoWorkerOptions;
using aki::persistence::schema_v1_steps;
using aki::persistence::sha256_hex;
using aki::transfer::FileMetadata;
using aki::transfer::TransferId;
using aki::transfer::TransferState;

int g_counter = 0;

std::string temp_root(const std::string& tag) {
    auto path = std::filesystem::temp_directory_path()
        / ("aki-send-path-" + tag + "-"
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

// 源文件工厂：pattern 字节循环填充 size 字节。
std::filesystem::path write_source(const std::string& root,
    const std::string& name, std::uint64_t size, char pattern) {
    const auto path = std::filesystem::path{root} / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    std::string chunk(1024, pattern);
    std::uint64_t written = 0;
    while (written < size) {
        const auto want =
            static_cast<std::size_t>(std::min<std::uint64_t>(1024, size - written));
        out.write(chunk.data(), static_cast<std::streamsize>(want));
        written += want;
    }
    out.close();
    return path;
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> chars{std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>()};
    const auto* data = reinterpret_cast<const std::byte*>(chars.data());
    return {data, data + chars.size()};
}

// 传输面专用 sink（inject → TM 路由；其余面恒接受——本栈不装配对应 Manager）。
struct TransferOnlySink final : aki::heyaki::HeyakiAdapterSink {
    explicit TransferOnlySink(TransferManager* target = nullptr)
        : transfers(target) {}

    TransferManager* transfers = nullptr;

    bool on_device_discovered(aki::device::DiscoveredDevice) override {
        return true;
    }
    bool on_device_connected(DeviceId, aki::device::ConnectionPath) override {
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
    bool on_transfer_paused(aki::transfer::TransferId transfer) override {
        return transfers->enqueue_transfer_paused(std::move(transfer));
    }
    bool on_connection_path_changed(DeviceId, aki::device::ConnectionPath,
        aki::device::ConnectionPath) override {
        return true;
    }
};

// 发送路径组合（网络无关）：ExecutorOwner + AppStateOwner（预置会话 conv-beta）
// + Fake Adapter + 真实 TransferIoControl/Runnable（aki.transfer-io worker）
// + TM（io 承载面注入）+ MM（hash-first 流断言用）。
struct SendPathStack {
    explicit SendPathStack(ExecutorOwner::Options host_options = {},
        TransferIoWorkerOptions io_options = {}, bool with_messages = true)
        : root(temp_root("stack")),
          store(std::make_shared<FileStore>(root)),
          io(std::make_shared<TransferIoControl>(store, io_options)),
          host(host_options) {
        if (!host.initialize()) {
            throw std::runtime_error("send path stack: executor init failed");
        }
        TransferManagerOptions transfer_options;
        transfer_options.pump.name = "aki.tm";
        transfer_options.sender = DeviceId{"local-1"};
        transfer_options.io = io.get();
        if (with_messages) {
            MessageManagerOptions message_options;
            message_options.pump.name = "aki.mm";
            message_options.local_device = DeviceId{"local-1"};
            messages.emplace(host.executor(), state_owner,
                adapter, message_options);
        }
        transfers.emplace(host.executor(), state_owner, adapter,
            transfer_options);
        // 传输面 sink 接通（inject → TM 路由；Fake 无 sink 时注入被拒）。
        sink.transfers = &*transfers;
        adapter.set_sink(&sink);
        // 装配序（§11.1③）：TM 构造（事件投递面注册）先于 worker 启动。
        auto runnable = std::make_unique<TransferIoRunnable>(io->impl());
        executor::BlockingWorkerSpec spec;
        spec.name = "aki.transfer-io";
        spec.config.thread_name = "aki-transfer-io";
        spec.worker = std::move(runnable);
        if (!host.start_blocking_worker(std::move(spec))) {
            throw std::runtime_error("send path stack: io worker failed");
        }
    }

    SendPathStack(const SendPathStack&) = delete;
    SendPathStack& operator=(const SendPathStack&) = delete;

    ~SendPathStack() {
        if (!host.is_shutdown()) {
            (void)transfers->request_cancel_all();
            (void)transfers->flush(2s);
            if (messages.has_value()) {
                (void)messages->flush(500ms);
            }
            (void)host.shutdown();
        }
    }

    void quiesce() {
        REQUIRE(transfers->flush(2s));
        if (messages.has_value()) {
            REQUIRE(messages->flush(2s));
        }
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
    }

    std::string root;
    std::shared_ptr<FileStore> store;
    std::shared_ptr<TransferIoControl> io;
    ExecutorOwner host;
    // 预置会话（FK 前置校验：MM 按 sender/receiver 派生 conv-beta）。
    AppStateOwner state_owner{aki::app::AppStateOwnerOptions{}, [] {
        aki::app::AppState state;
        aki::conversation::Conversation conversation;
        conversation.id = aki::conversation::ConversationId{"conv-beta"};
        conversation.local_device = DeviceId{"local-1"};
        conversation.remote_device = DeviceId{"beta"};
        state.conversations.conversations.push_back(conversation);
        return state;
    }()};
    FakeHeyakiAdapter adapter;
    std::optional<MessageManager> messages;
    std::optional<TransferManager> transfers;
    TransferOnlySink sink;  // 声明于 transfers 之后：引用稳定。
};

// 分块边界用例的公共断言：归档 .part 与源逐字节一致 + hash-first 回调哈希
// 与 sha256_hex 一致 + 终态组放行（DB 侧断言见重启一致性用例）。
void run_chunk_case(SendPathStack& stack, const FileMetadata& file,
    const std::filesystem::path& source, const TransferId& id,
    std::uint64_t size) {
    std::atomic<bool> hash_ready{false};
    std::string got_hash;
    REQUIRE(stack.transfers->start_transfer(DeviceId{"beta"}, id, file, source,
        [&](std::string hash_hex) {
            got_hash = std::move(hash_hex);
            hash_ready.store(true);
        }));
    REQUIRE(stack.transfers->flush(2s));
    // hash-first：发送前 SHA-256 完成（消息发送等 hash，§7.1④）。
    REQUIRE(wait_until([&] { return hash_ready.load(); }, 5s));
    const auto expected_hash = sha256_hex(read_bytes(source));
    REQUIRE(got_hash == expected_hash);
    // 归档完成：.part 与源逐字节一致（write_part 顺序写）。
    const auto part = stack.store->part_path(id.value);
    REQUIRE(wait_until([&] {
        std::error_code ec;
        return std::filesystem::exists(part, ec)
            && std::filesystem::file_size(part, ec) == size;
    }, 5s));
    REQUIRE(read_bytes(part) == read_bytes(source));

    // 终态组放行（wire 事件合法链：Queued → Negotiating → Transferring →
    // Completed，§7.1⑤ 相位映射）。
    REQUIRE(stack.adapter.inject_transfer_started(aki::transfer::Transfer{id,
        DeviceId{"local-1"}, DeviceId{"beta"}, file, 0, size,
        TransferState::Negotiating}));
    REQUIRE(stack.adapter.inject_transfer_started(aki::transfer::Transfer{id,
        DeviceId{"local-1"}, DeviceId{"beta"}, file, 0, size,
        TransferState::Transferring}));
    REQUIRE(stack.adapter.inject_transfer_completed(id, TransferState::Completed));
    stack.quiesce();
    {
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        REQUIRE(stack.state_owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.transfers.transfers.size() == 1);
        REQUIRE(snapshot.value.transfers.transfers.front().state
            == TransferState::Completed);
    }
}

// flush 顺序回归（TOCTOU）用判定式假想 IO：首次 idle() 在测试经 sink 投递
// 事件前不返回（复现 worker 线程在泵静止后回投 IO 事件的交错）；此后恒
// idle。事件投递/放行由测试经 executor 任务驱动（AGENTS 规则 2）。
class GatedIdleIo final : public aki::transfer::TransferIo {
public:
    bool start(const TransferId&, std::filesystem::path, std::string,
        std::uint64_t) override {
        return true;
    }
    bool advance(const TransferId&) override { return false; }
    bool cancel(const TransferId&) override { return true; }
    bool release(const TransferId&) override { return true; }
    void set_event_sink(
        std::function<void(const aki::transfer::TransferIoEvent&)> sink)
        override {
        sink_ = std::move(sink);
    }
    void request_stop() noexcept override {}
    bool idle() const noexcept override {
        if (stage_.load() == 0) {
            stage_.store(1);  // 告知驱动任务：idle 判定已进入
            while (stage_.load() < 2) {
                std::this_thread::sleep_for(1ms);  // 等事件投递后放行
            }
        }
        return true;
    }
    std::uint64_t rejected_submissions() const noexcept override { return 0; }

    [[nodiscard]] int stage() const noexcept { return stage_.load(); }
    void release_gate() noexcept { stage_.store(2); }
    void deliver(const aki::transfer::TransferIoEvent& event) {
        if (sink_) {
            sink_(event);  // worker 线程语义的回投（经 TM 投递面入泵收件箱）
        }
    }

private:
    std::function<void(const aki::transfer::TransferIoEvent&)> sink_;
    mutable std::atomic<int> stage_{0};
};

}  // namespace

// ---- 分块边界（空文件 / 非整块尾部 / 多块）+ hash-first 交叉验证 ----

TEST_CASE("Send archive covers chunk boundaries with hash-first verification",
    "[unit][send_path][dod02]") {
    TransferIoWorkerOptions io_options;
    io_options.chunk_bytes = 8;  // 强制多块边界（最小分块）
    SendPathStack stack{ExecutorOwner::Options{}, io_options};

    SECTION("empty file") {
        const auto source = write_source(stack.root, "empty.bin", 0, 'x');
        run_chunk_case(stack,
            FileMetadata{"empty.bin", 0, "application/octet-stream", ""},
            source, TransferId{"t-empty"}, 0);
    }
    SECTION("non-aligned tail (20B, chunk 8)") {
        const auto source = write_source(stack.root, "tail.bin", 20, 'a');
        run_chunk_case(stack,
            FileMetadata{"tail.bin", 20, "application/octet-stream", ""},
            source, TransferId{"t-tail"}, 20);
    }
    SECTION("multi chunk (100B, chunk 8)") {
        const auto source = write_source(stack.root, "multi.bin", 100, 'm');
        run_chunk_case(stack,
            FileMetadata{"multi.bin", 100, "application/octet-stream", ""},
            source, TransferId{"t-multi"}, 100);
    }

    const auto report = stack.host.shutdown();
    REQUIRE(report.fully_stopped());
    REQUIRE(stack.io->idle());
}

// ---- hash-first 图片流（v2）：stored_sha256 随消息载荷 ----

TEST_CASE("Hash-first image flow carries stored_sha256 in the message",
    "[unit][send_path]") {
    TransferIoWorkerOptions io_options;
    io_options.chunk_bytes = 16;
    SendPathStack stack{ExecutorOwner::Options{}, io_options};
    const auto source = write_source(stack.root, "photo.bin", 48, 'p');
    const FileMetadata file{"photo.bin", 48, "image/png", ""};

    REQUIRE(aki::app::send_image_message_with_hash(*stack.transfers,
        *stack.messages, DeviceId{"beta"}, MessageId{"m-img"}, file,
        TransferId{"t-img"}, source)
        == aki::app::ImageSendFlowResult::Submitted);

    // 消息在 hash 完成后异步发出（泵上下文延续）；行 media.stored_sha256
    // 与源文件 SHA-256 一致（DEC-010 字段 5）。
    const auto expected_hash = sha256_hex(read_bytes(source));
    REQUIRE(wait_until([&] {
        stack.state_owner.drain();
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        if (!stack.state_owner.try_load_snapshot(snapshot)) {
            return false;
        }
        for (const auto& message : snapshot.value.messages.messages) {
            if (message.id == MessageId{"m-img"}) {
                const auto* image = std::get_if<aki::conversation::ImagePayload>(
                    &message.payload);
                return image != nullptr
                    && image->media.stored_sha256 == expected_hash;
            }
        }
        return false;
    }, 5s));
    REQUIRE(stack.adapter.sent_images().size() == 1);
    REQUIRE(stack.adapter.sent_images().front().file.stored_sha256
        == expected_hash);

    // Fake 记录 Start 命令含 source_path（M4-02 登记的路径消费落地）。
    REQUIRE(stack.adapter.transfer_commands().size() == 1);
    REQUIRE(stack.adapter.transfer_commands().front().source_path == source);

    const auto report = stack.host.shutdown();
    REQUIRE(report.fully_stopped());
    REQUIRE(stack.io->idle());
}

// ---- 进度聚合：单飞 dirty（一次排空至多一个 UpdateTransferProgress）+
//      latest-wins 终值不丢 ----

TEST_CASE("Progress coalescing applies at most one update per drain and "
    "keeps the final value",
    "[unit][send_path]") {
    TransferIoWorkerOptions io_options;
    io_options.chunk_bytes = 8;
    SendPathStack stack{ExecutorOwner::Options{}, io_options};
    const auto source = write_source(stack.root, "prog.bin", 32, 'g');
    const FileMetadata file{"prog.bin", 32, "application/octet-stream", ""};
    const TransferId id{"t-prog"};

    REQUIRE(stack.transfers->start_transfer(DeviceId{"beta"}, id, file, source));
    stack.quiesce();  // 会话建立（hash/copy 链路就绪或推进中）

    // 会话路径注入三连：单飞 dirty 聚合（一次排空至多一个进度更新）。
    const auto applied_before = stack.state_owner.stats().updates_applied;
    REQUIRE(stack.adapter.inject_transfer_progress(id, 8, 32));
    REQUIRE(stack.adapter.inject_transfer_progress(id, 16, 32));
    REQUIRE(stack.adapter.inject_transfer_progress(id, 24, 32));
    stack.quiesce();
    // latest-wins：终值不丢（进度槽语义）。
    {
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        REQUIRE(stack.state_owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.transfers.transfers.front().transferred == 24);
    }
    // 聚合上界：三连注入 ≤ 3 个进度更新（实际一次排空为 1——上界断言
    // 防止逐条入箱回归；下界为终值已落地的 1）。
    const auto progress_updates =
        stack.state_owner.stats().updates_applied - applied_before;
    REQUIRE(progress_updates >= 1);
    REQUIRE(progress_updates <= 3);

    const auto report = stack.host.shutdown();
    REQUIRE(report.fully_stopped());
    REQUIRE(stack.io->idle());
}

// ---- 终态闸门：wire Completed 先于归档完成 → 持有；归档完成 → 放行 ----
//（确定性：单线程池 + 饱和任务——start+pause 在泵运行前入列，FIFO 保证
//  pause 先于任何 IO 事件被处理，归档停在首块边界。）

TEST_CASE("Terminal gate holds wire completion until the archive finishes",
    "[unit][send_path]") {
    ExecutorOwner::Options host_options;
    host_options.executor_config.min_threads = 1;
    host_options.executor_config.max_threads = 1;
    TransferIoWorkerOptions io_options;
    io_options.chunk_bytes = 8;
    SendPathStack stack{host_options, io_options};
    const auto source = write_source(stack.root, "gate.bin", 40, 'k');
    const FileMetadata file{"gate.bin", 40, "application/octet-stream", ""};
    const TransferId id{"t-gate"};

    auto saturate =
        stack.host.executor().submit_auto([] {
            std::this_thread::sleep_for(150ms);
        });
    REQUIRE(stack.transfers->start_transfer(DeviceId{"beta"}, id, file, source));
    REQUIRE(stack.transfers->pause_transfer(id));
    saturate.get();  // 泵此刻才运行：FIFO 处理 start → pause（先于 IO 事件）
    stack.quiesce();  // 积压 IO 事件到达：paused → 无续接

    // 归档停在首块（hash 相位）：.part 未创建（copy 相位未开始）。
    {
        std::error_code ec;
        REQUIRE_FALSE(std::filesystem::exists(stack.store->part_path(id.value), ec));
    }
    // wire committed 先到（行先推进到 Transferring——合法链）：归档未完成
    // → 持有（行不得 Completed）。
    REQUIRE(stack.adapter.inject_transfer_started(aki::transfer::Transfer{id,
        DeviceId{"local-1"}, DeviceId{"beta"}, file, 0, 40,
        TransferState::Negotiating}));
    REQUIRE(stack.adapter.inject_transfer_started(aki::transfer::Transfer{id,
        DeviceId{"local-1"}, DeviceId{"beta"}, file, 0, 40,
        TransferState::Transferring}));
    REQUIRE(stack.adapter.inject_transfer_completed(id, TransferState::Completed));
    stack.quiesce();
    {
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        REQUIRE(stack.state_owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.transfers.transfers.front().state
            != TransferState::Completed);
        REQUIRE(stack.transfers->active_session_count() == 1);
    }
    // 恢复归档 → copy_done 放行 held 终态（M2-06 作业组随后消费完整 .part）。
    REQUIRE(stack.transfers->resume_transfer(id));
    REQUIRE(wait_until([&] {
        stack.quiesce();
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        if (!stack.state_owner.try_load_snapshot(snapshot)) {
            return false;
        }
        return snapshot.value.transfers.transfers.front().state
            == TransferState::Completed;
    }, 5s));
    REQUIRE(stack.transfers->active_session_count() == 0);

    const auto report = stack.host.shutdown();
    REQUIRE(report.fully_stopped());
    REQUIRE(stack.io->idle());
}

// ---- DOD-02：任务异常（源缺失 → failed 事件，worker 存活）+ 归档失败时
//      hash 延续以空 hash 触发（不发消息）+ held 终态按已知边角释放 ----

TEST_CASE("IO failure surfaces as a failed event and the worker survives",
    "[unit][send_path][dod02]") {
    TransferIoWorkerOptions io_options;
    io_options.chunk_bytes = 8;
    SendPathStack stack{ExecutorOwner::Options{}, io_options};
    const FileMetadata file{"ghost.bin", 16, "application/octet-stream", ""};
    const TransferId id{"t-ghost"};
    std::atomic<bool> hash_ready{false};
    std::string got_hash = "unset";

    REQUIRE(stack.transfers->start_transfer(DeviceId{"beta"}, id, file,
        std::filesystem::path{stack.root} / "does-not-exist.bin",
        [&](std::string hash_hex) {
            got_hash = std::move(hash_hex);
            hash_ready.store(true);
        }));
    REQUIRE(wait_until([&] { return hash_ready.load(); }, 5s));
    REQUIRE(got_hash.empty());  // 归档失败：空 hash（调用方不发消息）

    // 归档失败 + wire Completed 到达：held 终态按已知边角立即释放（不悬挂；
    // M2-06 作业组对缺失 .part 明确失败可见——本栈无 DB，行推进即证据）。
    // 行先推进到 Transferring（合法链）。
    REQUIRE(stack.adapter.inject_transfer_started(aki::transfer::Transfer{id,
        DeviceId{"local-1"}, DeviceId{"beta"}, file, 0, 16,
        TransferState::Negotiating}));
    REQUIRE(stack.adapter.inject_transfer_started(aki::transfer::Transfer{id,
        DeviceId{"local-1"}, DeviceId{"beta"}, file, 0, 16,
        TransferState::Transferring}));
    REQUIRE(stack.adapter.inject_transfer_completed(id, TransferState::Completed));
    stack.quiesce();
    {
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        REQUIRE(stack.state_owner.try_load_snapshot(snapshot));
        REQUIRE(snapshot.value.transfers.transfers.front().state
            == TransferState::Completed);
    }

    // worker 存活：后续会话正常归档（异常不外抛、不终止 worker）。
    const auto source = write_source(stack.root, "alive.bin", 12, 'v');
    const TransferId id2{"t-alive"};
    std::atomic<bool> hash2{false};
    REQUIRE(stack.transfers->start_transfer(DeviceId{"beta"}, id2,
        FileMetadata{"alive.bin", 12, "application/octet-stream", ""}, source,
        [&](std::string) { hash2.store(true); }));
    REQUIRE(wait_until([&] { return hash2.load(); }, 5s));
    REQUIRE(wait_until([&] {
        std::error_code ec;
        return std::filesystem::exists(
            stack.store->part_path(id2.value), ec);
    }, 5s));

    const auto report = stack.host.shutdown();
    REQUIRE(report.fully_stopped());
    REQUIRE(stack.io->idle());
}

// ---- DOD-02：提交拒绝——io 承载面停止后 admission 拒绝可见 ----

TEST_CASE("IO submission rejection after stop is visible",
    "[unit][send_path][dod02]") {
    TransferIoWorkerOptions io_options;
    SendPathStack stack{ExecutorOwner::Options{}, io_options};
    const auto source = write_source(stack.root, "reject.bin", 4, 'r');
    const FileMetadata file{"reject.bin", 4, "application/octet-stream", ""};

    stack.io->request_stop();  // 承载面停止（组合根关闭序内的确定性形态）
    REQUIRE(wait_until([&] { return stack.io->idle(); }, 2s));
    const auto rejected_before = stack.transfers->io_rejected_submissions();
    std::atomic<bool> hash_ready{false};
    REQUIRE(stack.transfers->start_transfer(DeviceId{"beta"},
        TransferId{"t-reject"}, file, source,
        [&](std::string) { hash_ready.store(true); }));
    stack.quiesce();
    REQUIRE(hash_ready.load());  // 空 hash 直通（无归档承载）
    REQUIRE(stack.transfers->io_rejected_submissions() == rejected_before + 1);

    const auto report = stack.host.shutdown();
    REQUIRE(report.fully_stopped());
}

// ---- DOD-02：执行中取消——.part 幂等删除 + 迟到事件忽略 + 会话回收 ----

TEST_CASE("In-flight cancellation discards the partial archive idempotently",
    "[unit][send_path][dod02]") {
    ExecutorOwner::Options host_options;
    host_options.executor_config.min_threads = 1;
    host_options.executor_config.max_threads = 1;
    TransferIoWorkerOptions io_options;
    io_options.chunk_bytes = 8;
    SendPathStack stack{host_options, io_options};
    const auto source = write_source(stack.root, "cancel.bin", 40, 'c');
    const FileMetadata file{"cancel.bin", 40, "application/octet-stream", ""};
    const TransferId id{"t-cancel"};

    // 确定性在飞形态（同闸门用例）：start+cancel 在泵运行前入列——cancel
    // 先于任何 IO 事件处理，归档仅推进 open 作业的首块。
    auto saturate =
        stack.host.executor().submit_auto([] {
            std::this_thread::sleep_for(150ms);
        });
    REQUIRE(stack.transfers->start_transfer(DeviceId{"beta"}, id, file, source));
    REQUIRE(stack.transfers->cancel_transfer(id));
    saturate.get();
    REQUIRE(stack.transfers->flush(2s));
    REQUIRE(wait_until([&] {
        std::error_code ec;
        return !std::filesystem::exists(stack.store->part_path(id.value), ec);
    }, 5s));
    REQUIRE(stack.transfers->active_session_count() == 0);
    REQUIRE(stack.transfers->cancelled_session_count() == 1);
    bool adapter_cancel_seen = false;
    for (const auto& command : stack.adapter.transfer_commands()) {
        if (command.kind == FakeHeyakiAdapter::TransferCommand::Kind::Cancel
            && command.transfer_id == id) {
            adapter_cancel_seen = true;
        }
    }
    REQUIRE(adapter_cancel_seen);
    // 会话已清理：迟到 IO 事件幂等忽略（计数可见，不崩溃不复活）。
    (void)stack.io->advance(id);  // 泵外直驱（迟到形态注入）
    std::this_thread::sleep_for(50ms);
    stack.quiesce();
    REQUIRE(stack.transfers->active_session_count() == 0);
    REQUIRE(stack.transfers->late_io_events() >= 1);

    const auto report = stack.host.shutdown();
    REQUIRE(report.fully_stopped());
    REQUIRE(stack.io->idle());
}

// ---- DOD-02：shutdown——在飞会话 + IO 归零 + fully_stopped ----

TEST_CASE("Shutdown quiesces in-flight archives and stops cleanly",
    "[unit][send_path][dod02]") {
    TransferIoWorkerOptions io_options;
    io_options.chunk_bytes = 8;
    SendPathStack stack{ExecutorOwner::Options{}, io_options};
    const auto source = write_source(stack.root, "sd.bin", 64, 's');
    const FileMetadata file{"sd.bin", 64, "application/octet-stream", ""};

    REQUIRE(stack.transfers->start_transfer(
        DeviceId{"beta"}, TransferId{"t-sd"}, file, source));
    (void)stack.transfers->flush(50ms);  // 归档允许在飞（证据：IO 未归零）
    const auto report = stack.host.shutdown([&] {
        (void)stack.transfers->request_cancel_all();
        CHECK(stack.transfers->flush(2s));
    });
    REQUIRE(report.fully_stopped());
    REQUIRE(report.blocking_workers_stopped == 1);
    REQUIRE(stack.io->idle());
}

// ---- flush 顺序（TOCTOU 回归，§11.1③/DEC-011 ③）：先判 IO 归零、后最终
//      泵排空——「泵静止与 idle 判定之间落入的 IO 事件」不得以未处理状态
//      滞留（旧序在 driver 放行后立即返回 true、事件滞留队列；新序的最终
//      排空消费事件后才返回）----

TEST_CASE("flush drains IO events landing between pump quiescence and idle",
    "[unit][send_path]") {
    ExecutorOwner::Options host_options;
    host_options.executor_config.min_threads = 2;  // w1=饱和、w2=驱动任务
    host_options.executor_config.max_threads = 2;
    ExecutorOwner host{host_options};
    REQUIRE(host.initialize());
    FakeHeyakiAdapter adapter;
    AppStateOwner state_owner;
    GatedIdleIo io;
    TransferManagerOptions transfer_options;
    transfer_options.pump.name = "aki.tm";
    transfer_options.sender = DeviceId{"local-1"};
    transfer_options.io = &io;
    TransferManager transfers{host.executor(), state_owner, adapter,
        transfer_options};

    // 会话建立（start 处理完毕，泵静止；active_session_count == 1）。
    const FileMetadata file{"flush-order.bin", 4, "application/octet-stream", ""};
    REQUIRE(transfers.start_transfer(
        DeviceId{"beta"}, TransferId{"t-flush"}, file));
    REQUIRE(wait_until(
        [&] { return transfers.active_session_count() == 1; }, 2s));

    // w1 饱和：IO 事件入箱后的排空任务在旧序窗口内无处调度（确定性）。
    auto saturate = host.executor().submit_auto([] {
        std::this_thread::sleep_for(700ms);
    });
    // w2 驱动：等 flush 进入 idle 判定 → 投递 cancelled 事件（复现 worker
    // 回投交错）→ 持有 worker 使排空任务滞留队列 → 放行（此后 300ms 内
    // 旧序已返回 true 而事件仍未处理——断言面）。
    auto driver = host.executor().submit_auto([&io] {
        if (!wait_until([&io] { return io.stage() >= 1; }, 2s)) {
            io.release_gate();
            return;
        }
        io.deliver({TransferId{"t-flush"},
            aki::transfer::TransferIoEvent::Phase::cancelled, 0, 0, {}, {}});
        std::this_thread::sleep_for(200ms);
        io.release_gate();
        std::this_thread::sleep_for(300ms);
    });

    // 新序：等 IO 归零（gate 内）→ 最终泵排空消费 cancelled 事件 → idle
    // 复判为真，才返回 true。
    REQUIRE(transfers.flush(3s));
    // 契约断言：flush true ⇒ 箱内无未处理 IoEventWork（cancelled 已消费、
    // 会话回收）；旧序在 driver 放行后立即返回 true，此处为 1（回归陷阱）。
    REQUIRE(transfers.active_session_count() == 0);

    driver.get();
    saturate.get();
    const auto report = host.shutdown();
    REQUIRE(report.fully_stopped());
}

// ---- 2-worker 小池夹具（M4-03 观察③ 闭环）：双并发归档不停占池 worker ----

TEST_CASE("Two-worker pool stays schedulable under concurrent archives",
    "[unit][send_path][starve]") {
    ExecutorOwner::Options host_options;
    host_options.executor_config.min_threads = 2;  // 2 vCPU 设备形态
    host_options.executor_config.max_threads = 2;
    TransferIoWorkerOptions io_options;
    io_options.chunk_bytes = 4;  // 多块拉长链路
    SendPathStack stack{host_options, io_options};

    const auto source_a = write_source(stack.root, "a.bin", 48, 'a');
    const auto source_b = write_source(stack.root, "b.bin", 48, 'b');
    const FileMetadata file{"x.bin", 48, "application/octet-stream", ""};
    std::atomic<int> hashes{0};
    auto on_hash = [&hashes](std::string) { hashes.fetch_add(1); };
    REQUIRE(stack.transfers->start_transfer(DeviceId{"beta"},
        TransferId{"t-two-a"}, file, source_a, on_hash));
    REQUIRE(stack.transfers->start_transfer(DeviceId{"beta"},
        TransferId{"t-two-b"}, file, source_b, on_hash));

    // 旧骨架形态下双会话停占全部 2 worker，MM 泵永无调度（M4-03 观察③）；
    // 新承载（分块 IO 走 blocking worker）下文本发送在预算内完成。
    REQUIRE(stack.messages->send_text(DeviceId{"beta"}, MessageId{"m-alive"},
        "pump still schedulable"));
    REQUIRE(stack.messages->flush(2s));

    // 双归档完成（hash + .part 逐字节一致）。
    REQUIRE(wait_until([&] { return hashes.load() == 2; }, 10s));
    REQUIRE(wait_until([&] {
        std::error_code ec;
        return std::filesystem::file_size(
                   stack.store->part_path("t-two-a"), ec)
                == 48
            && std::filesystem::file_size(
                   stack.store->part_path("t-two-b"), ec)
                == 48;
    }, 10s));
    REQUIRE(read_bytes(stack.store->part_path("t-two-a"))
        == read_bytes(source_a));
    REQUIRE(read_bytes(stack.store->part_path("t-two-b"))
        == read_bytes(source_b));

    const auto report = stack.host.shutdown();
    REQUIRE(report.fully_stopped());
}

// ---- 重启一致性（完整 DB 组合）：M2-06 终态作业组真实文件源 ----

TEST_CASE("Send archive completes through the DB terminal job group and "
    "survives restart",
    "[unit][send_path][restart]") {
    const std::string root = temp_root("restart");
    TransferIoWorkerOptions io_options;
    io_options.chunk_bytes = 8;

    ExecutorOwner host;
    REQUIRE(host.initialize());
    auto store = std::make_shared<FileStore>(root);
    auto io = std::make_shared<TransferIoControl>(store, io_options);

    // DB 侧：迁移 + 仓储 + DatabaseWorker + 接受后处理器（WritePathSink 的
    // 传输子集——镜像 main.cpp 装配）。
    auto database = Database::open(
        (std::filesystem::path{root} / "aki.db3").string());
    const Migrator migrator{schema_v1_steps()};
    REQUIRE(migrator.bring_up_to_date(database) == 1);
    auto db = std::make_shared<aki::persistence::DatabaseWorkerControl>(
        std::make_unique<aki::persistence::Repositories>(std::move(database)),
        aki::persistence::DatabaseWorkerOptions{});
    std::vector<std::future<void>> db_futures;
    std::uint64_t db_admitted = 0;
    AppStateOwner state_owner{aki::app::AppStateOwnerOptions{}, aki::app::AppState{}, [&](
        const aki::app::AppStateUpdate& update) {
        std::visit(
            [&](const auto& concrete) {
                using Update = std::decay_t<decltype(concrete)>;
                if constexpr (std::is_same_v<Update, aki::app::UpsertTransfer>) {
                    auto job = aki::persistence::make_transfer_upsert_job(
                        concrete.transfer);
                    db_futures.push_back(job.done->get_future());
                    if (db->enqueue(std::move(job))) {
                        ++db_admitted;
                    }
                } else if constexpr (std::is_same_v<Update,
                                               aki::app::UpdateTransferProgress>) {
                    auto job = aki::persistence::make_transfer_progress_job(
                        concrete.transfer, concrete.transferred,
                        concrete.total);
                    db_futures.push_back(job.done->get_future());
                    if (db->enqueue(std::move(job))) {
                        ++db_admitted;
                    }
                } else if constexpr (std::is_same_v<Update,
                                               aki::app::CompleteTransfer>) {
                    std::vector<aki::persistence::DbJob> jobs;
                    if (concrete.final_state == TransferState::Completed) {
                        jobs.push_back(aki::persistence::make_transfer_complete_job(
                            store, concrete.transfer.value));
                    } else {
                        jobs.push_back(
                            aki::persistence::make_transfer_terminal_job(
                                concrete.transfer, concrete.final_state));
                        jobs.push_back(aki::persistence::make_transfer_discard_job(
                            store, concrete.transfer.value));
                    }
                    for (auto& job : jobs) {
                        db_futures.push_back(job.done->get_future());
                        if (db->enqueue(std::move(job))) {
                            ++db_admitted;
                        }
                    }
                }
            },
            update);
    }};
    FakeHeyakiAdapter adapter;

    TransferManagerOptions transfer_options;
    transfer_options.pump.name = "aki.tm";
    transfer_options.sender = DeviceId{"local-1"};
    transfer_options.io = io.get();
    TransferManager transfers{host.executor(), state_owner, adapter,
        transfer_options};
    TransferOnlySink sink{&transfers};
    adapter.set_sink(&sink);

    auto io_runnable = std::make_unique<TransferIoRunnable>(io->impl());
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

    const auto source = write_source(root, "model.bin", 40, 'd');
    const FileMetadata file{"model.bin", 40, "application/octet-stream", ""};
    const TransferId id{"t-db"};
    const auto expected_hash = sha256_hex(read_bytes(source));

    std::string got_hash;
    std::atomic<bool> hash_ready{false};
    REQUIRE(transfers.start_transfer(DeviceId{"beta"}, id, file, source,
        [&](std::string hash_hex) {
            got_hash = std::move(hash_hex);
            hash_ready.store(true);
        }));
    REQUIRE(wait_until([&] { return hash_ready.load(); }, 5s));
    REQUIRE(got_hash == expected_hash);
    // 归档链路完成（flush true ⇔ 泵静止 + IO 归零）后再注入 wire 事件——
    // 避免 copy_progress 与注入进度在最新槽上交错（latest-wins 语义下
    // 测试期望终值 = 注入值）。
    REQUIRE(wait_until([&] { return transfers.flush(200ms); }, 5s));
    // wire 事件：进度 + committed（合法链）。
    REQUIRE(adapter.inject_transfer_started(aki::transfer::Transfer{id,
        DeviceId{"local-1"}, DeviceId{"beta"}, file, 0, 40,
        TransferState::Negotiating}));
    REQUIRE(adapter.inject_transfer_started(aki::transfer::Transfer{id,
        DeviceId{"local-1"}, DeviceId{"beta"}, file, 0, 40,
        TransferState::Transferring}));
    REQUIRE(transfers.enqueue_transfer_progress(id, 40, 40));
    // 进度先落地再宣告 committed（RULE-08：终态后到达的进度被拒——wire
    // 上 progress 亦恒先于 committed）。
    REQUIRE(wait_until([&] {
        (void)transfers.flush(100ms);
        state_owner.drain();
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        return state_owner.try_load_snapshot(snapshot)
            && snapshot.value.transfers.transfers.front().transferred == 40;
    }, 5s));
    REQUIRE(adapter.inject_transfer_completed(id, TransferState::Completed));

    // 等待终态组（M2-06：SHA-256 + 原子改名 + 回写位）执行完毕。
    REQUIRE(wait_until([&] {
        (void)transfers.flush(100ms);
        state_owner.drain();
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        return state_owner.try_load_snapshot(snapshot)
            && snapshot.value.transfers.transfers.front().state
                == TransferState::Completed;
    }, 5s));
    db->request_drain();
    REQUIRE(wait_until([&] { return db->drain_completed(); }, 5s));
    for (auto& future : db_futures) {
        future.get();  // 全部作业成功（RULE-09：失败可见）
    }

    // 关闭（EXEC-01 序：flush（含 IO 归零）→ close → db drain → owner 步骤
    // 2/3 由 shutdown 统一回收两 worker）。
    const auto report = host.shutdown([&] {
        (void)transfers.request_cancel_all();
        CHECK(transfers.flush(2s));
        state_owner.close();
        db->request_drain();
    });
    REQUIRE(report.fully_stopped());
    REQUIRE(report.blocking_workers_stopped == 2);

    // 终态落盘断言（关闭后、重启前）：files/<id>/<净化名> 存在 + .part 消失。
    const auto final_path =
        std::filesystem::path{root} / "files" / id.value / "model.bin";
    REQUIRE(std::filesystem::exists(final_path));
    REQUIRE(read_bytes(final_path) == read_bytes(source));
    REQUIRE(!std::filesystem::exists(store->part_path(id.value)));

    // ---- 重启：重开 DB + 断言行与文件本体一致（验收：重启一致性）----
    auto reopened = Database::open(
        (std::filesystem::path{root} / "aki.db3").string());
    const Migrator reopen_migrator{schema_v1_steps()};
    REQUIRE(reopen_migrator.bring_up_to_date(reopened) == 0);
    // 写回位断言先行（prepare 于 Database 移交仓储前——Database move-only）。
    std::string stored_relative_path;
    std::string stored_sha256;
    std::uint64_t stored_size_bytes = 0;
    {
        aki::persistence::Statement statement = reopened.prepare(
            "SELECT stored_relative_path, stored_sha256, stored_size_bytes"
            " FROM transfer WHERE transfer_id = ?1;");
        statement.bind(1, id.value);
        REQUIRE(statement.step());
        stored_relative_path = statement.column_text(0);
        stored_sha256 = statement.column_text(1);
        stored_size_bytes =
            static_cast<std::uint64_t>(statement.column_int64(2));
    }
    REQUIRE(stored_relative_path == "files/" + id.value + "/model.bin");
    REQUIRE(stored_sha256 == expected_hash);
    REQUIRE(stored_size_bytes == 40);
    auto reopened_repos =
        std::make_unique<aki::persistence::Repositories>(std::move(reopened));
    const auto row = reopened_repos->transfers.find(id);
    REQUIRE(row.has_value());
    REQUIRE(row->state == TransferState::Completed);
    REQUIRE(row->transferred == 40);
    REQUIRE(row->total == 40);
    REQUIRE(std::filesystem::exists(final_path));
    REQUIRE(read_bytes(final_path) == read_bytes(source));
}
