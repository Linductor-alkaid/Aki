// M3-05：Conversation 建链与文本消息真实化集成测试（DEC-006 映射 4；
// SCOPE-05/06；双节点进程内回环，per-node executor 形态沿 M3-04 偏差 ①）。
//
// 覆盖（网络依赖）：配对成功后的双节点文本收发（B inbound、A acked）、send
// 对不可达 peer 的 Result 失败 → false（RULE-09）。握手被拦时输出 [skip] +
// 受控退出（不冒充已验证；补跑条件见里程碑验证记录）。
//
// 网络无关的 MessageId 双射 / aki.text 信封往返已拆分至
// tests/unit/test_heyaki_message.cpp（评审修正，工程规范第 7 节）：本文件的
// [skip] 降级以受控退出结束进程，若同二进制存在先行失败的用例，其回归会被
// exit 0 掩盖；拆分后本二进制仅剩单个用例——REQUIRE 失败即中止当前用例
// （Catch2 语义），两个 [skip] 退出点只在同用例前置断言全部通过时可达，
// 受控退出不再可能掩盖既有失败。不得在本文件追加先于 [skip] 门的新用例。
#include "app/lifecycle/executor_owner.hpp"
#include "heyaki/adapter/lan_discovery.hpp"
#include "heyaki/adapter/local_identity.hpp"
#include "heyaki/session/runtime_node.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
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
        / ("aki-msg-loopback-" + tag + "-"
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

// 节点域（同 M3-04 测试形态：独立测试 ExecutorOwner + 借用 Runtime）。
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
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
        std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
        std::byte{0x09}, std::byte{0x0a}, std::byte{0x0b}, std::byte{0x0c},
        std::byte{0x0d}, std::byte{0x0e}, std::byte{0x0f}, std::byte{0x10}});
}

}  // namespace

TEST_CASE("Two-node text messaging over the borrowed runtime",
    "[integration][message_loopback]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());

    NodeDomain domain_a(temp_root("msg-a"));
    NodeDomain domain_b(temp_root("msg-b"));
    auto& side_a = *domain_a.session;
    auto& side_b = *domain_b.session;
    const auto identity_a = domain_a.profile.identity();
    const auto identity_b = domain_b.profile.identity();
    if (!side_a.has_lan_interfaces() || !side_b.has_lan_interfaces()) {
        std::printf("[skip] no LAN interface\n");
        std::fflush(nullptr);
        std::_Exit(0);
    }

    // 发现 + 连接 + 配对（沿 M3-04：握手被拦则降级退出，不冒充已验证）。
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
            "[skip] pairing handshake blocked (firewall): messaging loopback "
            "not verified; rerun with inbound TCP allowed\n");
        std::fflush(nullptr);
        std::_Exit(0);
    }
    REQUIRE(side_b.session_pairing_restricted(identity_a.id));
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
    REQUIRE(side_a.pair_peer(identity_b.id, "aki-msg-pw"));
    REQUIRE(side_b.pair_peer(identity_a.id, "aki-msg-pw"));
    REQUIRE(wait_until([&] { return (paired_a.load() && paired_b.load()); }, 20s));

    // 消息面（DEC-006 映射 4）：B 收（协议层已去重 + ACK）、A 收到 acked。
    const auto aki_id_hex =
        aki::heyaki::NodeSession::to_aki_message_id(fixed_wire_id());
    std::atomic<bool> inbound_seen{false};
    std::atomic<bool> acked_seen{false};
    side_b.set_message_handlers(
        [&](const DeviceId& peer, const aki::conversation::MessageId& id,
            const std::string& text) {
            if (peer == identity_a.id && id == aki_id_hex
                && text == "hello from aki") {
                inbound_seen.store(true);
            }
        },
        [](const DeviceId&, const aki::conversation::MessageId&,
            const std::string&) {});
    side_a.set_message_handlers(
        [](const DeviceId&, const aki::conversation::MessageId&,
            const std::string&) {},
        [&](const DeviceId& peer, const aki::conversation::MessageId& id,
            const std::string& event) {
            if (peer == identity_b.id && id == aki_id_hex && event == "acked") {
                acked_seen.store(true);
            }
        });

    // 发送（DEC-006 冻结信封）；送达回报 acked → Delivered 语义。
    REQUIRE(side_a.send_text(identity_b.id, aki_id_hex, "hello from aki"));
    REQUIRE(wait_until([&] { return inbound_seen.load(); }, 20s));  // 验收 ①
    REQUIRE(wait_until([&] { return acked_seen.load(); }, 20s));

    // send 拒绝路径（验收 ③）：不存在端点的 peer → Result 失败 → false。
    REQUIRE_FALSE(side_a.send_text(
        DeviceId{"hy1_00000000000000000000000000000000000000000000000000000000000000"},
        aki::conversation::MessageId{"m-offline"}, "nope"));

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
