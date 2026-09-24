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
// 管道层（EXEC-04 timer / start-stop / DOD-02 六项）沿 M3-04 periodic 路径
// 六项模式（test_discovery_pairing），本二进制不依赖网络与 Node 实例。
#include "heyaki/adapter/peer_sessions_pipeline.hpp"

#include <catch2/catch_test_macros.hpp>

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
    std::vector<aki::device::DeviceId> disconnected;
    std::vector<aki::device::ConnectionPath> paths;
    aki::heyaki::PeerSessionEvents events;
    events.on_connected = [&](const aki::device::DeviceId& id) {
        connected.push_back(id);
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

TEST_CASE("Peer session diff ignores non-authenticated churn",
    "[unit][peer_sessions]") {
    using aki::heyaki::NodeSession;

    int events_fired = 0;
    aki::heyaki::PeerSessionEvents events;
    events.on_connected = [&](const aki::device::DeviceId&) { ++events_fired; };
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
