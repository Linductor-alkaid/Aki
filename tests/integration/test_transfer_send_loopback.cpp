// M4-04：发送侧真实传输链路双节点回环（DEC-006 映射 7/DEC-011；双节点
// 进程内回环，per-node executor 形态沿 M3-04/M4-03）。
//
// 覆盖（网络依赖）：配对成功后的真实 Adapter 发起面——start_file_transfer
// → heyaki push_file admission（对端已认证会话）+ Aki 侧归档链路（hash +
// .part）完成。接收侧落盘/事件路由归 M4-05（对端接收根未配置，wire 终态
// 不在本用例断言面）。防火墙拦截至端 TLS 时输出 [skip] + 受控退出（沿
// M3-04~09/M4-03 降级纪律，不冒充已验证；补跑条件见 M4 里程碑验证记录）。
//
// 网络无关半边（归档链路/闸门/取消/重启一致性）在
// tests/unit/test_transfer_send_path.cpp——本二进制仅单个用例（[skip] 受控
// 退出不得掩盖既有失败，工程规范第 7 节拆分纪律），不得在本文件追加先于
// [skip] 门的新用例。
#include "app/application/transfer_manager.hpp"
#include "app/lifecycle/executor_owner.hpp"
#include "app/state/app_state_owner.hpp"
#include "heyaki/adapter/heyaki_node_adapter.hpp"
#include "heyaki/adapter/lan_discovery.hpp"
#include "heyaki/adapter/local_identity.hpp"
#include "heyaki/session/runtime_node.hpp"
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
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

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
        / ("aki-send-loopback-" + tag + "-"
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

struct NodeDomain {
    explicit NodeDomain(const std::string& root,
        bool with_receive_root = false)
        : profile(LocalProfile::open(root)),
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
            {.profile = &profile, .file_receive_roots = std::move(receive_roots)}));
    }

    LocalProfile profile;
    aki::app::ExecutorOwnerOptions executor_options;
    ExecutorOwner owner;
    std::optional<NodeSession> session;
};

}  // namespace

TEST_CASE("Two-node send-side transfer chain over the borrowed runtime",
    "[integration][transfer_send_loopback]") {
    const std::string root = temp_root("send");
    ExecutorOwner owner;
    REQUIRE(owner.initialize());

    const std::string b_root = temp_root("b");
    NodeDomain domain_a(temp_root("a"));
    NodeDomain domain_b(b_root, /*with_receive_root=*/true);  // M4-05 接收侧
    auto& side_a = *domain_a.session;
    auto& side_b = *domain_b.session;
    const auto identity_a = domain_a.profile.identity();
    const auto identity_b = domain_b.profile.identity();
    if (!side_a.has_lan_interfaces() || !side_b.has_lan_interfaces()) {
        std::printf("[skip] no LAN interface\n");
        std::fflush(nullptr);
        std::_Exit(0);
    }

    // 发现 + 连接 + 配对（沿 M3-05/M4-03：握手被拦则降级退出）。
    LanDiscoveryPipeline pipeline(owner.executor(), side_a, [](const auto&) {});
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
            "[skip] pairing handshake blocked (firewall): send-side transfer "
            "loopback not verified; rerun with inbound TCP allowed\n");
        std::fflush(nullptr);
        std::_Exit(0);
    }
    REQUIRE(wait_until(
        [&] { return side_b.session_pairing_restricted(identity_a.id); }, 15s));
    std::atomic<bool> paired_a{false};
    std::atomic<bool> paired_b{false};
    side_a.set_pairing_observer(
        [&](const DeviceId& peer, bool ok, const std::string&) {
            if (ok && peer == identity_b.id) paired_a.store(true);
        });
    side_b.set_pairing_observer(
        [&](const DeviceId& peer, bool ok, const std::string&) {
            if (ok && peer == identity_a.id) paired_b.store(true);
        });
    REQUIRE(side_a.pair_peer(identity_b.id, aki::heyaki::kAkiPairingPassword));
    REQUIRE(side_b.pair_peer(identity_a.id, aki::heyaki::kAkiPairingPassword));
    if (!wait_until([&] { return paired_a.load() && paired_b.load(); }, 20s)) {
        std::printf("[skip] pairing handshake did not complete after "
                    "submission: send-side transfer loopback not verified; "
                    "rerun with inbound TCP allowed\n");
        std::fflush(nullptr);
        std::_Exit(0);
    }

    // 发送侧组合（M4-04）：真实 Adapter（push_file 接线，root 经 Options）
    // + TransferIoControl/Runnable（aki.transfer-io worker）+ TM。
    auto store = std::make_shared<FileStore>(root);
    TransferIoWorkerOptions io_options;
    io_options.chunk_bytes = 16;
    auto io = std::make_shared<TransferIoControl>(store, io_options);
    AppStateOwner state_owner;
    HeyakiNodeAdapter adapter{owner.executor(),
        {.profile = &domain_a.profile,
            .session = &side_a,
            .conversation_for =
                [](const DeviceId& remote) {
                    return aki::conversation::ConversationId{
                        std::string{"conv-"} + remote.value};
                },
            .peer_observation = false}};
    TransferManagerOptions transfer_options;
    transfer_options.pump.name = "aki.tm";
    transfer_options.sender = identity_a.id;
    transfer_options.io = io.get();
    TransferManager transfers{owner.executor(), state_owner, adapter,
        transfer_options};

    auto io_runnable = std::make_unique<TransferIoRunnable>(io->impl());
    executor::BlockingWorkerSpec io_spec;
    io_spec.name = "aki.transfer-io";
    io_spec.config.thread_name = "aki-transfer-io";
    io_spec.worker = std::move(io_runnable);
    REQUIRE(owner.start_blocking_worker(std::move(io_spec)));

    // B 侧文件事件观察（M4-05：接收链路——八相位经 Adapter 分发；此处直连
    // NodeSession 观察面记录相位到达）。
    std::atomic<int> b_committed{0};
    std::atomic<int> b_progress{0};
    side_b.set_file_event_observer(
        [&](const DeviceId&, const NodeSession::FileTransferEventView& event) {
            if (static_cast<::heyaki::FileTransferPhase>(event.phase)
                == ::heyaki::FileTransferPhase::committed) {
                b_committed.fetch_add(1);
            } else if (static_cast<::heyaki::FileTransferPhase>(event.phase)
                == ::heyaki::FileTransferPhase::transferring) {
                b_progress.fetch_add(1);
            }
        });

    // 源文件（32B，chunk 16 → 双块）与规范 TransferId（DEC-011 ④ 生成入口）。
    const auto source = std::filesystem::path{root} / "payload.bin";
    {
        std::ofstream out(source, std::ios::binary | std::ios::trunc);
        const std::string bytes(32, 'z');
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    const auto transfer_id = NodeSession::new_transfer_id();
    REQUIRE(transfer_id.value.size() == 31);
    const FileMetadata file{"payload.bin", 32, "application/octet-stream", ""};

    // 真实发起面：push_file admission（对端已认证）+ Aki 归档链路完成。
    std::atomic<bool> hash_ready{false};
    std::string got_hash;
    REQUIRE(transfers.start_transfer(identity_b.id, transfer_id, file, source,
        [&](std::string hash_hex) {
            got_hash = std::move(hash_hex);
            hash_ready.store(true);
        }));
    REQUIRE(wait_until([&] { return hash_ready.load(); }, 10s));
    std::string expected_hash;
    {
        std::ifstream in(source, std::ios::binary);
        std::vector<char> chars{std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
        expected_hash = sha256_hex(
            std::as_bytes(std::span<const char>{chars}));
    }
    REQUIRE(got_hash == expected_hash);
    // 归档 .part 完整（消息面外的本体链路，RULE-05）。
    REQUIRE(wait_until([&] {
        std::error_code ec;
        return std::filesystem::file_size(store->part_path(transfer_id.value),
                   ec)
            == 32;
    }, 10s));

    // 非规范 TransferId：真实 Adapter push 转换拒绝（admission false 可见，
    // DEC-011 ④）。
    REQUIRE_FALSE(adapter.start_file_transfer(identity_b.id,
        aki::transfer::TransferId{"t-1"}, file, source));

    // 接收侧（M4-05）：B 于接收根收到完整文件（heyaki committed——BLAKE3
    // verify + fsync + rename 后的落盘形态；合并到 DEC-004 布局归 Aki 侧
    // complete 作业，此处断言 wire 侧接收完成）。
    REQUIRE(wait_until([&] { return b_committed.load() >= 1; }, 20s));
    REQUIRE(b_progress.load() >= 1);

    // 受控关闭（IO 归零 + worker 回收）。
    (void)transfers.request_cancel_all();
    REQUIRE(transfers.flush(2s));
    const auto node_report = side_a.shutdown();
    REQUIRE(node_report.node_stopped);
    const auto rb = side_b.shutdown();
    REQUIRE(rb.node_stopped);
    (void)domain_a.owner.shutdown();
    (void)domain_b.owner.shutdown();
    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
    REQUIRE(io->idle());
}
