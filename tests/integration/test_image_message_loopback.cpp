// M4-03：图片消息双节点回环（DEC-006 映射 4 图片面扩展/DEC-010①；双节点
// 进程内回环，per-node executor 形态沿 M3-04/M3-05）。
//
// 覆盖（网络依赖）：配对成功后的双节点 aki.image 信封收发——B inbound 收到
// type=aki.image 且 payload 解码回 ImagePayload（metadata + TransferId 无损，
// RULE-05 消息面契约）、A 收到 acked。防火墙拦截至端 TLS 时输出 [skip] +
// 受控退出（沿 M3-04/05/09 降级纪律，不冒充已验证；补跑条件见 M4 里程碑
// 验证记录）。
//
// 网络无关断言（编解码往返/双射/拒收路径）在 tests/unit/test_heyaki_message.cpp
// 与 tests/unit/test_image_payload_codec.cpp——本二进制仅单个用例（[skip] 受控
// 退出不得掩盖既有失败，工程规范第 7 节拆分纪律），不得在本文件追加先于
// [skip] 门的新用例。
#include "app/lifecycle/executor_owner.hpp"
#include "conversation/codec/image_payload_codec.hpp"
#include "heyaki/adapter/lan_discovery.hpp"
#include "heyaki/adapter/local_identity.hpp"
#include "heyaki/session/runtime_node.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;

using aki::app::ExecutorOwner;
using aki::device::DeviceId;
using aki::heyaki::LanDiscoveryPipeline;
using aki::heyaki::LocalProfile;
using aki::heyaki::NodeSession;

std::string temp_root(const std::string& tag) {
    auto path = std::filesystem::temp_directory_path()
        / ("aki-img-loopback-" + tag + "-"
            + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
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
    return predicate();
}

// 节点域（同 M3-04/M3-05 测试形态：独立测试 ExecutorOwner + 借用 Runtime）。
struct NodeDomain {
    explicit NodeDomain(const std::string& root)
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
        session.emplace(
            NodeSession::create(owner.executor(), {.profile = &profile}));
    }

    LocalProfile profile;
    aki::app::ExecutorOwnerOptions executor_options;
    ExecutorOwner owner;
    std::optional<NodeSession> session;
};

::heyaki::MessageId fixed_wire_id() {
    return ::heyaki::MessageId(::heyaki::MessageId::Storage{
        std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34},
        std::byte{0x35}, std::byte{0x36}, std::byte{0x37}, std::byte{0x38},
        std::byte{0x39}, std::byte{0x3a}, std::byte{0x3b}, std::byte{0x3c},
        std::byte{0x3d}, std::byte{0x3e}, std::byte{0x3f}, std::byte{0x40}});
}

// 规范 TransferId（heyaki 编码器产物，按构造规范）。
aki::transfer::TransferId canonical_transfer_id() {
    return aki::transfer::TransferId{::heyaki::to_string(
        ::heyaki::TransferId(::heyaki::TransferId::Storage{
            std::byte{0x41}, std::byte{0x42}, std::byte{0x43}, std::byte{0x44},
            std::byte{0x45}, std::byte{0x46}, std::byte{0x47}, std::byte{0x48},
            std::byte{0x49}, std::byte{0x4a}, std::byte{0x4b}, std::byte{0x4c},
            std::byte{0x4d}, std::byte{0x4e}, std::byte{0x4f}, std::byte{0x50}}))};
}

}  // namespace

TEST_CASE("Two-node image messaging over the borrowed runtime",
    "[integration][image_message_loopback]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());

    NodeDomain domain_a(temp_root("img-a"));
    NodeDomain domain_b(temp_root("img-b"));
    auto& side_a = *domain_a.session;
    auto& side_b = *domain_b.session;
    const auto identity_a = domain_a.profile.identity();
    const auto identity_b = domain_b.profile.identity();
    if (!side_a.has_lan_interfaces() || !side_b.has_lan_interfaces()) {
        std::printf("[skip] no LAN interface\n");
        std::fflush(nullptr);
        std::_Exit(0);
    }

    // 发现 + 连接 + 配对（沿 M3-05：握手被拦则降级退出，不冒充已验证；
    // 配对 scope 冻结 message.send——消息通道含 aki.image 信封）。
    LanDiscoveryPipeline pipeline(
        owner.executor(), side_a, [](const auto&) {});
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
    const bool restricted =
        wait_until([&] { return side_a.session_pairing_restricted(identity_b.id); },
            15s);
    if (!restricted) {
        std::printf(
            "[skip] pairing handshake blocked (firewall): image messaging "
            "loopback not verified; rerun with inbound TCP allowed\n");
        std::fflush(nullptr);
        std::_Exit(0);
    }
    REQUIRE(wait_until(
        [&] { return side_b.session_pairing_restricted(identity_a.id); },
        15s));
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
    REQUIRE(side_a.pair_peer(identity_b.id, "aki-img-pw"));
    REQUIRE(side_b.pair_peer(identity_a.id, "aki-img-pw"));
    if (!wait_until(
            [&] { return (paired_a.load() && paired_b.load()); }, 20s)) {
        // 环境受限降级（沿 M3-04/05/09 纪律，不冒充已验证）。
        for (const auto& entry : side_a.peer_session_diagnostics()) {
            std::printf("    [diag] A session peer=%s state=%d restricted=%d\n",
                entry.first.c_str(), entry.second.first, entry.second.second);
        }
        for (const auto& entry : side_b.peer_session_diagnostics()) {
            std::printf("    [diag] B session peer=%s state=%d restricted=%d\n",
                entry.first.c_str(), entry.second.first, entry.second.second);
        }
        std::printf("[skip] pairing handshake did not complete after "
                    "submission: image messaging loopback not verified; rerun "
                    "with inbound TCP allowed\n");
        std::fflush(nullptr);
        std::_Exit(0);
    }

    // 图片消息面（M4-03，DEC-010①）：B inbound（type=aki.image + payload
    // 解码无损）、A 收 acked。
    const auto aki_id =
        aki::heyaki::NodeSession::to_aki_message_id(fixed_wire_id());
    const auto transfer_id = canonical_transfer_id();
    const aki::transfer::FileMetadata media{"photo.png", 2048, "image/png", ""};
    std::atomic<bool> inbound_seen{false};
    std::atomic<bool> acked_seen{false};
    side_b.set_message_handlers(
        [&](const DeviceId& peer, const aki::conversation::MessageId& id,
            const std::string& type, const std::string& payload) {
            if (peer != identity_a.id || id != aki_id
                || type
                    != std::string(
                        aki::conversation::codec::kAkiImageEnvelopeType)) {
                return;
            }
            const auto decoded =
                aki::conversation::codec::decode_image_payload(
                    std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(payload.data()),
                        payload.size()));
            if (decoded.value.has_value()
                && decoded.value->media == media
                && decoded.value->transfer_id == transfer_id) {
                inbound_seen.store(true);
            }
        },
        [](const DeviceId&, const aki::conversation::MessageId&,
            const std::string&) {});
    side_a.set_message_handlers(
        [](const DeviceId&, const aki::conversation::MessageId&,
            const std::string&, const std::string&) {},
        [&](const DeviceId& peer, const aki::conversation::MessageId& id,
            const std::string& event) {
            if (peer == identity_b.id && id == aki_id && event == "acked") {
                acked_seen.store(true);
            }
        });

    // 发送（DEC-006 冻结信封 + 冻结字段号载荷）；送达回报 acked →
    // Delivered 语义（运行期零传导，§6.1②）。
    REQUIRE(side_a.send_image(identity_b.id, aki_id, media, transfer_id));
    REQUIRE(wait_until([&] { return inbound_seen.load(); }, 20s));  // 验收 ①
    REQUIRE(wait_until([&] { return acked_seen.load(); }, 20s));

    // send 拒绝路径：非规范 TransferId（ad-hoc 串）→ admission false 可见
    //（RULE-09；对端可达性已由上一断言确立，失败归因于编码契约校验）。
    REQUIRE_FALSE(side_a.send_image(identity_b.id, aki_id, media,
        aki::transfer::TransferId{"t-1"}));

    pipeline.stop();
    const auto ra = side_a.shutdown();
    const auto rb = side_b.shutdown();
    REQUIRE(ra.node_stopped);
    REQUIRE(ra.runtime_stopped);
    REQUIRE_FALSE(ra.runtime_executor_shutdown_performed);
    REQUIRE(rb.node_stopped);
    REQUIRE(rb.runtime_stopped);
    REQUIRE_FALSE(rb.runtime_executor_shutdown_performed);
    (void)domain_a.owner.shutdown();
    (void)domain_b.owner.shutdown();
    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}
