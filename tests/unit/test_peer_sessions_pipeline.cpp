// M3-06：peer_sessions diff 管道单元测试（网络无关断言拆分至独立 unit
// 二进制——DEC-006 映射 5 + 设计第 8.1 节触发语义；SCOPE-10）。
//
// 覆盖：
//   - ConnectionPath 四值映射（direct+lan→Lan、direct_srflx→P2p、
//     turn_udp/tcp/tls→Relay、unknown→Unknown；direct_host+relay→P2p）；
//   - diff 转移：新 authenticated → connected；authenticating 不触发；
//     authenticated 缺失/closed → disconnected；两侧 authenticated 且
//     data_path/signaling_route 变化 → connection_path_changed（RULE-06：
//     仅路径摘要，无会话记录语义）。
// M5-11（设计 §8.1 触发语义修订）：LAN 发现单 tick 纯函数 diff
// `diff_lan_discovery`（peer_sessions 同型：trusted/非 Aki 广播跳过、
// seen 去重、live 集回落 went_offline、合成设备 presence = Online）。
// 管道层（EXEC-04 timer / start-stop / DOD-02 六项）沿 M3-04 periodic 路径
// 六项模式（test_discovery_pairing），本二进制不依赖网络与 Node 实例。
#include "heyaki/adapter/lan_discovery.hpp"
#include "heyaki/adapter/peer_sessions_pipeline.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

aki::heyaki::NodeSession::PeerSessionView make_view(
    std::string id, int state, int data_path, int signaling_route) {
    aki::heyaki::NodeSession::PeerSessionView view;
    view.device_id = aki::device::DeviceId{std::move(id)};
    view.endpoint_id = "hye1_test";
    view.state = state;
    view.data_path = data_path;
    view.signaling_route = signaling_route;
    view.authenticated = (state == 4);  // NodePeerSessionState::authenticated
    view.pairing_restricted = (state == 3);
    view.closed = (state == 5);
    return view;
}

}  // namespace

TEST_CASE("ConnectionPath mapping follows the DEC-006 frozen table",
    "[unit][peer_sessions][scope10]") {
    using aki::device::ConnectionPath;
    // direct_host + lan → Lan（DEC-006：direct+lan）。
    CHECK(aki::heyaki::map_connection_path(1, 0) == ConnectionPath::Lan);
    // direct_host + relay 信令 → P2p（直连数据面不因信令通道变 Relay）。
    CHECK(aki::heyaki::map_connection_path(1, 1) == ConnectionPath::P2p);
    // direct_srflx → P2p。
    CHECK(aki::heyaki::map_connection_path(2, 0) == ConnectionPath::P2p);
    CHECK(aki::heyaki::map_connection_path(2, 1) == ConnectionPath::P2p);
    // turn_* → Relay。
    CHECK(aki::heyaki::map_connection_path(3, 0) == ConnectionPath::Relay);
    CHECK(aki::heyaki::map_connection_path(4, 1) == ConnectionPath::Relay);
    CHECK(aki::heyaki::map_connection_path(5, 0) == ConnectionPath::Relay);
    // unknown → Unknown。
    CHECK(aki::heyaki::map_connection_path(0, 0) == ConnectionPath::Unknown);
}

TEST_CASE("Peer session diff synthesizes connected/disconnected transitions",
    "[unit][peer_sessions]") {
    using aki::heyaki::NodeSession;

    std::vector<aki::device::DeviceId> connected;
    std::vector<aki::device::ConnectionPath> connected_paths;
    std::vector<aki::device::DeviceId> disconnected;
    std::vector<aki::device::ConnectionPath> paths;
    aki::heyaki::PeerSessionEvents events;
    events.on_connected = [&](const aki::device::DeviceId& id,
        aki::device::ConnectionPath path) {
        connected.push_back(id);
        connected_paths.push_back(path);
    };
    events.on_disconnected = [&](const aki::device::DeviceId& id) {
        disconnected.push_back(id);
    };
    events.on_connection_path_changed =
        [&](const aki::device::DeviceId&, aki::device::ConnectionPath path) {
            paths.push_back(path);
        };

    // 首轮：authenticating（2）不触发 connected。
    auto prev = std::vector<NodeSession::PeerSessionView>{
        make_view("peer-a", 2, 1, 0)};
    auto curr = std::vector<NodeSession::PeerSessionView>{
        make_view("peer-a", 2, 1, 0)};
    aki::heyaki::diff_peer_sessions(prev, curr, events);
    CHECK(connected.empty());

    // authenticating → authenticated：connected 触发。
    curr = {make_view("peer-a", 4, 1, 0)};
    aki::heyaki::diff_peer_sessions(prev, curr, events);
    REQUIRE(connected.size() == 1);
    CHECK(connected.back().value == "peer-a");
    prev = curr;

    // 路径变化（direct_host+lan → turn_tls）：connection_path_changed（Relay）。
    curr = {make_view("peer-a", 4, 5, 0)};
    aki::heyaki::diff_peer_sessions(prev, curr, events);
    REQUIRE(paths.size() == 1);
    CHECK(paths.back() == aki::device::ConnectionPath::Relay);
    prev = curr;

    // 会话缺失（对端断开）→ disconnected。
    curr = {};
    aki::heyaki::diff_peer_sessions(prev, curr, events);
    REQUIRE(disconnected.size() == 1);
    CHECK(disconnected.back().value == "peer-a");
    prev = curr;

    // 重连（connected）后再 closed → disconnected 再次触发。
    curr = {make_view("peer-a", 4, 1, 0)};
    aki::heyaki::diff_peer_sessions(prev, curr, events);
    REQUIRE(connected.size() == 2);
    prev = curr;
    curr = {make_view("peer-a", 5, 1, 0)};  // closed
    aki::heyaki::diff_peer_sessions(prev, curr, events);
    REQUIRE(disconnected.size() == 2);
}

TEST_CASE("Peer session diff reports a newly restricted pairing session once",
    "[unit][peer_sessions][dec018]") {
    using aki::heyaki::NodeSession;
    std::vector<aki::device::DeviceId> ready;
    aki::heyaki::PeerSessionEvents events;
    events.on_pairing_ready = [&](const aki::device::DeviceId& id) {
        ready.push_back(id);
    };
    auto authenticating = std::vector<NodeSession::PeerSessionView>{
        make_view("peer-a", 2, 1, 0)};
    auto restricted = std::vector<NodeSession::PeerSessionView>{
        make_view("peer-a", 3, 1, 0)};
    aki::heyaki::diff_peer_sessions(authenticating, restricted, events);
    REQUIRE(ready.size() == 1);
    CHECK(ready.front().value == "peer-a");
    aki::heyaki::diff_peer_sessions(restricted, restricted, events);
    CHECK(ready.size() == 1);
}

TEST_CASE("Restricted link connects before authorization and stays connected",
    "[unit][peer_sessions][connection_trust]") {
    using aki::device::ConnectionPath;
    using aki::heyaki::NodeSession;
    int connected = 0;
    int disconnected = 0;
    int authorized = 0;
    std::vector<ConnectionPath> paths;
    aki::heyaki::PeerSessionEvents events;
    events.on_connected = [&](const aki::device::DeviceId&,
        ConnectionPath path) {
        ++connected;
        paths.push_back(path);
    };
    events.on_disconnected = [&](const aki::device::DeviceId&) {
        ++disconnected;
    };
    events.on_trust_changed = [&](const aki::device::DeviceId&) { ++authorized; };
    events.on_connection_path_changed = [&](const aki::device::DeviceId&,
        ConnectionPath path) { paths.push_back(path); };

    auto handshake = std::vector<NodeSession::PeerSessionView>{
        make_view("peer-a", 2, 0, 0)};
    auto restricted = std::vector<NodeSession::PeerSessionView>{
        make_view("peer-a", 3, 0, 0)};
    aki::heyaki::diff_peer_sessions(handshake, restricted, events);
    REQUIRE(connected == 1);
    REQUIRE(paths == std::vector<ConnectionPath>{ConnectionPath::Lan});
    REQUIRE(authorized == 0);

    auto authenticated = std::vector<NodeSession::PeerSessionView>{
        make_view("peer-a", 4, 1, 0)};
    aki::heyaki::diff_peer_sessions(restricted, authenticated, events);
    REQUIRE(connected == 1);
    REQUIRE(disconnected == 0);
    REQUIRE(authorized == 1);
    REQUIRE(paths.size() == 1);

    aki::heyaki::diff_peer_sessions(authenticated, {}, events);
    REQUIRE(disconnected == 1);
}

TEST_CASE("Peer session diff ignores non-authenticated churn",
    "[unit][peer_sessions]") {
    using aki::heyaki::NodeSession;

    int events_fired = 0;
    aki::heyaki::PeerSessionEvents events;
    events.on_connected = [&](const aki::device::DeviceId&,
        aki::device::ConnectionPath) { ++events_fired; };
    events.on_disconnected = [&](const aki::device::DeviceId&) { ++events_fired; };
    events.on_connection_path_changed =
        [&](const aki::device::DeviceId&, aki::device::ConnectionPath) {
            ++events_fired;
        };

    // signaling→transport_connecting→authenticating 的中间态翻动不触发事件。
    auto prev = std::vector<NodeSession::PeerSessionView>{
        make_view("peer-a", 0, 1, 0)};
    for (const int state : {1, 2, 1, 0}) {
        auto curr = std::vector<NodeSession::PeerSessionView>{
            make_view("peer-a", state, 1, 0)};
        aki::heyaki::diff_peer_sessions(prev, curr, events);
        prev = curr;
    }
    CHECK(events_fired == 0);
}

TEST_CASE("A reconnect is not hidden by finished peer session diagnostics",
    "[unit][peer_sessions][reconnect_history]") {
    using aki::device::ConnectionPath;
    for (const int linked_state : {3, 4}) {
        for (const bool history_first : {false, true}) {
            DYNAMIC_SECTION("state=" << linked_state
                << " history_first=" << history_first) {
                int connected = 0;
                int disconnected = 0;
                int ready = 0;
                int authorized = 0;
                std::vector<ConnectionPath> paths;
                aki::heyaki::PeerSessionEvents events;
                events.on_connected = [&](const auto&, ConnectionPath path) {
                    ++connected;
                    paths.push_back(path);
                };
                events.on_disconnected = [&](const auto&) { ++disconnected; };
                events.on_pairing_ready = [&](const auto&) { ++ready; };
                events.on_trust_changed = [&](const auto&) { ++authorized; };
                events.on_connection_path_changed = [&](const auto&,
                    ConnectionPath path) { paths.push_back(path); };

                auto old_link = make_view("peer-a", linked_state, 1, 0);
                old_link.session_id = "old-session";
                auto history = make_view("peer-a", 5, 5, 1);
                history.session_id = "old-session";
                // Epochs from different sessions do not define recency.
                history.session_epoch = 99;
                aki::heyaki::diff_peer_sessions({old_link}, {history}, events);
                REQUIRE(disconnected == 1);

                auto current = make_view("peer-a", 2, 0, 0);
                current.session_id = "new-session";
                const auto with_history = [&](const auto& view) {
                    return history_first ? std::vector{history, view}
                                         : std::vector{view, history};
                };
                auto prev = with_history(current);
                aki::heyaki::diff_peer_sessions({history}, prev, events);
                REQUIRE(connected == 0);
                current = make_view("peer-a", linked_state, 1, 0);
                current.session_id = "new-session";
                auto curr = with_history(current);
                aki::heyaki::diff_peer_sessions(prev, curr, events);
                REQUIRE(connected == 1);
                REQUIRE(disconnected == 1);
                REQUIRE(paths == std::vector{ConnectionPath::Lan});
                REQUIRE(ready == (linked_state == 3 ? 1 : 0));
                REQUIRE(authorized == (linked_state == 4 ? 1 : 0));

                // Both cold-start snapshots and reorderings retain the link.
                int cold_connected = 0;
                aki::heyaki::PeerSessionEvents cold;
                cold.on_connected = [&](const auto&, auto) { ++cold_connected; };
                aki::heyaki::diff_peer_sessions({}, curr, cold);
                REQUIRE(cold_connected == 1);
                prev = curr;
                std::swap(curr[0], curr[1]);
                aki::heyaki::diff_peer_sessions(prev, curr, events);
                REQUIRE(connected == 1);
                REQUIRE(disconnected == 1);
                REQUIRE(ready == (linked_state == 3 ? 1 : 0));
                REQUIRE(authorized == (linked_state == 4 ? 1 : 0));

                prev = curr;
                current.data_path = 5;
                current.signaling_route = 1;
                curr = with_history(current);
                aki::heyaki::diff_peer_sessions(prev, curr, events);
                REQUIRE(paths == std::vector{ConnectionPath::Lan,
                    ConnectionPath::Relay});
                REQUIRE(connected == 1);

                prev = curr;
                current = make_view("peer-a", 5, 5, 1);
                current.session_id = "new-session";
                curr = with_history(current);
                aki::heyaki::diff_peer_sessions(prev, curr, events);
                REQUIRE(disconnected == 2);
                aki::heyaki::diff_peer_sessions(curr, curr, events);
                REQUIRE(disconnected == 2);
            }
        }
    }
}

TEST_CASE("Device link aggregation prefers a linked current endpoint",
    "[unit][peer_sessions][reconnect_history]") {
    int connected = 0;
    int ready = 0;
    int authorized = 0;
    aki::heyaki::PeerSessionEvents events;
    events.on_connected = [&](const auto&, auto) { ++connected; };
    events.on_pairing_ready = [&](const auto&) { ++ready; };
    events.on_trust_changed = [&](const auto&) { ++authorized; };
    auto linked = make_view("peer-a", 4, 1, 0);
    auto restricted = make_view("peer-a", 3, 0, 0);
    restricted.endpoint_id = "hye1_other";
    auto handshake = make_view("peer-a", 2, 0, 0);
    handshake.endpoint_id = "hye1_handshake";
    auto closed = make_view("peer-a", 5, 0, 0);
    for (const auto& rows : {std::vector{linked, restricted, handshake, closed},
             std::vector{closed, handshake, restricted, linked}}) {
        aki::heyaki::diff_peer_sessions({}, rows, events);
    }
    REQUIRE(connected == 2);
    REQUIRE(authorized == 2);
    REQUIRE(ready == 0);
}

// ---- M5-11：LAN 发现单 tick 纯函数 diff（网络无关；lan_discovery.hpp）----

namespace {

aki::heyaki::EndpointView make_entry(std::string id, bool trusted = false,
    std::size_t key_bytes = 32) {
    aki::heyaki::EndpointView entry;
    entry.device_id = aki::device::DeviceId{std::move(id)};
    entry.public_key.bytes.assign(key_bytes, std::uint8_t{0xAB});
    entry.endpoint_id = "hye1_" + entry.device_id.value;
    entry.trusted = trusted;
    return entry;
}

}  // namespace

TEST_CASE("LAN discovery diff synthesizes untrusted 32-byte-key entries as online devices",
    "[unit][lan_discovery][m5_11]") {
    aki::heyaki::LanDiscoveryState state;
    auto tick = aki::heyaki::diff_lan_discovery(
        std::vector<aki::heyaki::EndpointView>{make_entry("dev-1")}, state);

    REQUIRE(tick.discovered.size() == 1);
    REQUIRE(tick.went_offline.empty());
    const auto& device = tick.discovered.front();
    REQUIRE(device.identity.id == aki::device::DeviceId{"dev-1"});
    REQUIRE(device.identity.public_key.bytes.size() == 32);
    REQUIRE(device.identity.trust_state == aki::device::TrustState::Unknown);
    // 目录条目 = 对端正在广播的存活证明：合成设备 presence = Online。
    REQUIRE(device.identity.presence == aki::device::PresenceState::Online);
    REQUIRE(device.method == aki::device::DiscoveryMethod::LanDiscovery);
    REQUIRE(device.endpoint.description == "lan:hye1_dev-1");
    // state 推进：本 tick 的设备进入 seen 与 live。
    REQUIRE(state.seen.count("dev-1") == 1);
    REQUIRE(state.live.count("dev-1") == 1);
}

TEST_CASE("LAN discovery diff skips trusted entries entirely",
    "[unit][lan_discovery][m5_11]") {
    aki::heyaki::LanDiscoveryState state;
    auto tick = aki::heyaki::diff_lan_discovery(
        std::vector<aki::heyaki::EndpointView>{make_entry("trusted-1", true)},
        state);

    // 已知设备记录不重放 discovered（§8.1）；也不进入 live 集——其 presence
    // 由 peer_sessions 会话事件承载，目录消失不产 went_offline。
    REQUIRE(tick.discovered.empty());
    REQUIRE(tick.went_offline.empty());
    REQUIRE(state.seen.empty());
    REQUIRE(state.live.empty());

    auto vanished = aki::heyaki::diff_lan_discovery({}, state);
    REQUIRE(vanished.discovered.empty());
    REQUIRE(vanished.went_offline.empty());
}

TEST_CASE("LAN discovery diff skips entries without a 32-byte identity key",
    "[unit][lan_discovery][m5_11]") {
    aki::heyaki::LanDiscoveryState state;
    // 非真实 Aki 身份广播（缺 identity_public_key / 长度不符）不可确认指纹，
    // 跳过且不入 live。
    auto tick = aki::heyaki::diff_lan_discovery(
        std::vector<aki::heyaki::EndpointView>{
            make_entry("short-key", false, 31),
            make_entry("long-key", false, 33)},
        state);
    REQUIRE(tick.discovered.empty());
    REQUIRE(tick.went_offline.empty());
    REQUIRE(state.seen.empty());
    REQUIRE(state.live.empty());
}

TEST_CASE("LAN discovery diff deduplicates repeated ids within and across ticks",
    "[unit][lan_discovery][m5_11]") {
    aki::heyaki::LanDiscoveryState state;
    // 同一 tick 内重复出现：幂等 no-op（seen 集吸收）。
    auto tick = aki::heyaki::diff_lan_discovery(
        std::vector<aki::heyaki::EndpointView>{
            make_entry("dev-1"), make_entry("dev-1")},
        state);
    REQUIRE(tick.discovered.size() == 1);
    REQUIRE(tick.went_offline.empty());

    // 跨 tick 仍在广播：不重复合成，也不产 went_offline。
    auto next = aki::heyaki::diff_lan_discovery(
        std::vector<aki::heyaki::EndpointView>{make_entry("dev-1")}, state);
    REQUIRE(next.discovered.empty());
    REQUIRE(next.went_offline.empty());
    REQUIRE(state.live.count("dev-1") == 1);
}

TEST_CASE("LAN discovery diff reports vanished live ids as went_offline",
    "[unit][lan_discovery][m5_11]") {
    aki::heyaki::LanDiscoveryState state;
    auto tick = aki::heyaki::diff_lan_discovery(
        std::vector<aki::heyaki::EndpointView>{
            make_entry("dev-1"), make_entry("dev-2")},
        state);
    REQUIRE(tick.discovered.size() == 2);

    // dev-1 从目录消失（租约过期/对端退出）、dev-2 仍在：回落面只报 dev-1。
    auto next = aki::heyaki::diff_lan_discovery(
        std::vector<aki::heyaki::EndpointView>{make_entry("dev-2")}, state);
    REQUIRE(next.discovered.empty());
    REQUIRE(next.went_offline.size() == 1);
    REQUIRE(next.went_offline.front() == aki::device::DeviceId{"dev-1"});
    REQUIRE(state.live.count("dev-1") == 0);
    REQUIRE(state.live.count("dev-2") == 1);
}

TEST_CASE("LAN discovery diff does not re-synthesize a reappearing device",
    "[unit][lan_discovery][m5_11]") {
    aki::heyaki::LanDiscoveryState state;
    auto tick = aki::heyaki::diff_lan_discovery(
        std::vector<aki::heyaki::EndpointView>{make_entry("dev-1")}, state);
    REQUIRE(tick.discovered.size() == 1);

    // 消失：went_offline。
    auto vanished = aki::heyaki::diff_lan_discovery({}, state);
    REQUIRE(vanished.went_offline.size() == 1);

    // 重现：重新进入 live 集（存活事实恢复），但不再次合成 discovered——
    // seen 集幂等（历史 Unknown 行/已知外形不重复上报，§8.1）。
    auto back = aki::heyaki::diff_lan_discovery(
        std::vector<aki::heyaki::EndpointView>{make_entry("dev-1")}, state);
    REQUIRE(back.discovered.empty());
    REQUIRE(back.went_offline.empty());
    REQUIRE(state.seen.count("dev-1") == 1);
    REQUIRE(state.live.count("dev-1") == 1);
}

TEST_CASE("LAN discovery diff graduates a live id to trusted without went_offline",
    "[unit][lan_discovery][m5_11]") {
    aki::heyaki::LanDiscoveryState state;

    // tick1：未信任 + 32 字节身份公钥 → 合成 discovered，进入 live 集。
    auto tick1 = aki::heyaki::diff_lan_discovery(
        std::vector<aki::heyaki::EndpointView>{make_entry("dev-1")}, state);
    REQUIRE(tick1.discovered.size() == 1);
    REQUIRE(tick1.went_offline.empty());
    REQUIRE(state.live.count("dev-1") == 1);

    // tick2：同一 id 毕业为信任（配对完成瞬间，§8.1）——不重放 discovered，
    // 也**不**合成 went_offline（信任转移不是离线，presence 归会话事件承载）；
    // 仅从 live 集移除，避免下轮对账误报。
    auto tick2 = aki::heyaki::diff_lan_discovery(
        std::vector<aki::heyaki::EndpointView>{make_entry("dev-1", true)},
        state);
    REQUIRE(tick2.discovered.empty());
    REQUIRE(tick2.went_offline.empty());
    REQUIRE(state.live.count("dev-1") == 0);
    REQUIRE(state.seen.count("dev-1") == 1);  // seen 去重语义不变。

    // tick3：条目整体消失（租约过期/对端退出）——live 集已在 tick2 移除该
    // id，对账不再补发 went_offline（无二次离线事件）。
    auto tick3 = aki::heyaki::diff_lan_discovery({}, state);
    REQUIRE(tick3.discovered.empty());
    REQUIRE(tick3.went_offline.empty());
    REQUIRE(state.live.empty());
}

TEST_CASE("Grant direction changes calibrate an already authorized link",
          "[unit][peer_sessions][trust_directions][m541]") {
    using aki::heyaki::NodeSession;
    int trust_changes = 0;
    int connects = 0;
    int disconnects = 0;
    aki::heyaki::PeerSessionEvents events;
    events.on_trust_changed = [&](const auto&) { ++trust_changes; };
    events.on_connected = [&](const auto&, auto) { ++connects; };
    events.on_disconnected = [&](const auto&) { ++disconnects; };
    auto old = make_view("peer", 4, 1, 0);
    old.trust_directions = NodeSession::TrustDirections{false, true};
    auto mutual = old;
    mutual.trust_directions->issued = true;
    aki::heyaki::diff_peer_sessions({old}, {mutual}, events);
    REQUIRE(trust_changes == 1);
    REQUIRE(connects == 0);
    REQUIRE(disconnects == 0);
    aki::heyaki::diff_peer_sessions({mutual}, {mutual}, events);
    REQUIRE(trust_changes == 1);
    auto unavailable = mutual;
    unavailable.trust_directions.reset();
    aki::heyaki::diff_peer_sessions({mutual}, {unavailable}, events);
    REQUIRE(trust_changes == 1);
    // Lost issued grant is observable without a connection state change.
    aki::heyaki::diff_peer_sessions({unavailable}, {old}, events);
    REQUIRE(trust_changes == 2);
    auto policy = old;
    policy.policy_scopes = {"message.send", "file.push:inbox"};
    aki::heyaki::diff_peer_sessions({old}, {policy}, events);
    REQUIRE(trust_changes == 2);
}
