// peer_sessions diff 管道（DEC-006 映射 5；SCOPE-10；M3-06；设计第 8.1 节
// 触发语义、第 10.1 节 LatestMailbox 消费）。
//
// 结构（复用 M3-04 LanDiscoveryPipeline 模式，EXEC-04 timer + EXEC-07 句柄
// 显式持有）：
//   - 纯函数层（网络无关，独立 unit 二进制覆盖）：
//     `map_connection_path(data_path, signaling_route)` → aki ConnectionPath
//     （DEC-006 映射 5：direct+lan→Lan、direct_srflx→P2p、turn_*→Relay、
//     unknown→Unknown）；`diff_peer_sessions(prev, curr, events)` →
//     pairing_restricted/authenticated↔closed 变化合成 connected/disconnected + 路径变化合成
//     connection_path_changed。
//   - 管道层：executor timer 周期轮询 NodeSession::peer_session_views()，
//     diff 后经事件回调投递（EXEC-02：消费方有界校验 + 投递）；TimerHandle
//     由管道持有，stop 取消后零回调；RULE-09 start 失败可见。
//
// RULE-10：heyaki 数值不跨层——视图与映射的公开面仅 aki 领域类型与 int
// （数值语义在本层文档内封闭）。
#pragma once

#include "device/device/device_types.hpp"
#include "heyaki/session/runtime_node.hpp"

#include <executor/executor.hpp>
#include <executor/timer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace aki::heyaki {

// 连接路径映射（DEC-006 映射 5）：direct+lan→Lan、direct_srflx→P2p、
// turn_*→Relay、unknown→Unknown。data_path/signaling_route 为
// NodeSession::PeerSessionView 携带的 heyaki 枚举数值。
[[nodiscard]] inline aki::device::ConnectionPath map_connection_path(
    int data_path, int signaling_route) {
    // NodeDataPathKind：0 unknown / 1 direct_host / 2 direct_srflx /
    // 3 turn_udp / 4 turn_tcp / 5 turn_tls。
    switch (data_path) {
        case 1:  // direct_host
            // 直接路径经 LAN 信令 → Lan；经 relay 信令 → P2p（直连不因信令
            // 通道变 Relay——Relay 语义保留给 turn 数据面）。
            return signaling_route == 1 ? aki::device::ConnectionPath::P2p
                                        : aki::device::ConnectionPath::Lan;
        case 2:  // direct_srflx
            return aki::device::ConnectionPath::P2p;
        case 3:
        case 4:
        case 5:  // turn_udp / turn_tcp / turn_tls
            return aki::device::ConnectionPath::Relay;
        default:  // unknown
            return aki::device::ConnectionPath::Unknown;
    }
}

[[nodiscard]] inline bool has_peer_link(
    const NodeSession::PeerSessionView& view) noexcept {
    return view.pairing_restricted || view.authenticated;
}

// Restricted sessions have a verified control channel but no data path yet.
[[nodiscard]] inline aki::device::ConnectionPath session_connection_path(
    const NodeSession::PeerSessionView& view) noexcept {
    if (view.pairing_restricted) {
        return view.signaling_route == 1
            ? aki::device::ConnectionPath::Relay
            : aki::device::ConnectionPath::Lan;
    }
    return map_connection_path(view.data_path, view.signaling_route);
}

// 会话事件（DEC-008 双 Manager 扇出的入口面：connected/disconnected 由
// DM+CM 双扇出，connection_path 变化/初连路径落 DM 的逐设备 Store 字段
// ——DEC-015）。
struct PeerSessionEvents {
    // 初连即携带映射路径（DEC-015：初连不发独立路径事件，本回调一并承载，
    // 消费方无需硬编码默认路径）。
    std::function<void(const aki::device::DeviceId&,
        aki::device::ConnectionPath)>
        on_connected;
    std::function<void(const aki::device::DeviceId&)> on_disconnected;
    std::function<void(const aki::device::DeviceId&,
        aki::device::ConnectionPath)>
        on_connection_path_changed;
    std::function<void(const aki::device::DeviceId&)> on_pairing_ready;
    std::function<void(const aki::device::DeviceId&)> on_authorized;
};

// 纯函数 diff：prev → curr 的设备级建链变化与路径变化。
// Node snapshots append finished closed diagnostics after current attempts.
// M5-32: discard history before grouping devices, and prefer a linked endpoint
// over another endpoint still handshaking. History never supersedes a live link.
//   - 新 restricted 或 authenticated → on_connected（含映射路径）；
//   - 原已建链现缺失/closed/握手态 → on_disconnected；
//   - 两侧均已建链且路径变化 →
//     on_connection_path_changed（映射后比较，RULE-06：仅路径摘要，不新建
//     会话记录——记录语义归应用层状态边界）。
inline void diff_peer_sessions(
    const std::vector<NodeSession::PeerSessionView>& prev,
    const std::vector<NodeSession::PeerSessionView>& curr,
    const PeerSessionEvents& events) {
    const auto index_current = [](const auto& views) {
        std::map<std::string, NodeSession::PeerSessionView> by_device;
        const auto priority = [](const auto& view) {
            return view.authenticated ? 2 : (view.pairing_restricted ? 1 : 0);
        };
        for (const auto& view : views) {
            if (view.closed) {
                continue;
            }
            auto [it, inserted] = by_device.try_emplace(view.device_id.value, view);
            if (!inserted && priority(view) > priority(it->second)) {
                it->second = view;
            }
        }
        return by_device;
    };
    const auto prev_by_key = index_current(prev);
    const auto curr_by_key = index_current(curr);

    for (const auto& [key, view] : curr_by_key) {
        const auto previous = prev_by_key.find(key);
        const bool was_linked =
            previous != prev_by_key.end() && has_peer_link(previous->second);
        if (view.pairing_restricted
            && (previous == prev_by_key.end()
                || !previous->second.pairing_restricted)
            && events.on_pairing_ready) {
            events.on_pairing_ready(view.device_id);
        }
        if (has_peer_link(view) && !was_linked) {
            if (events.on_connected) {
                events.on_connected(view.device_id,
                    session_connection_path(view));
            }
        }
        if (view.authenticated
            && (previous == prev_by_key.end()
                || !previous->second.authenticated)
            && events.on_authorized) {
            events.on_authorized(view.device_id);
        }
        if (has_peer_link(view) && was_linked) {
            const auto& old = previous->second;
            if (session_connection_path(old)
                != session_connection_path(view)) {
                if (events.on_connection_path_changed) {
                    events.on_connection_path_changed(view.device_id,
                        session_connection_path(view));
                }
            }
        }
    }
    for (const auto& [key, view] : prev_by_key) {
        const auto current = curr_by_key.find(key);
        const bool still_linked =
            current != curr_by_key.end() && has_peer_link(current->second);
        if (has_peer_link(view) && !still_linked) {
            if (events.on_disconnected) {
                events.on_disconnected(view.device_id);
            }
        }
    }
}

// 周期轮询管道（EXEC-04 timer；TimerHandle 由管道持有，EXEC-07）。
class PeerSessionPipeline {
public:
    explicit PeerSessionPipeline(executor::Executor& executor,
        aki::heyaki::NodeSession& session, PeerSessionEvents events)
        : executor_(executor), session_(session),
          events_(std::move(events)) {}

    PeerSessionPipeline(const PeerSessionPipeline&) = delete;
    PeerSessionPipeline& operator=(const PeerSessionPipeline&) = delete;

    ~PeerSessionPipeline() {
        stop();
    }

    [[nodiscard]] bool start(
        std::chrono::milliseconds period = std::chrono::milliseconds{500}) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (timer_.valid()) {
            return false;
        }
        timer_ = executor_.submit_periodic_with_handle(
            static_cast<std::int64_t>(period.count()), [this] { poll(); });
        return timer_.valid();
    }

    void stop() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (timer_.valid()) {
            (void)timer_.cancel();
            timer_ = executor::TimerHandle{};
        }
    }

    [[nodiscard]] bool running() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return timer_.valid();
    }

private:
    void poll() {
        auto curr = session_.peer_session_views();
        std::vector<NodeSession::PeerSessionView> previous;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            previous.swap(previous_);
            diff_peer_sessions(previous, curr, events_);
            previous_ = std::move(curr);
        }
    }

    executor::Executor& executor_;
    aki::heyaki::NodeSession& session_;
    PeerSessionEvents events_;
    mutable std::mutex mutex_;
    std::vector<NodeSession::PeerSessionView> previous_;
    executor::TimerHandle timer_;
};

}  // namespace aki::heyaki
