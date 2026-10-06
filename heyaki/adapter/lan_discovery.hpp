// LAN 发现观察管道（DEC-006 映射 2；设计第 8.1 节触发语义；M3-04）。
//
// start/stop_discovery 的真实语义 = 启停本管道（Node 常驻 LAN 广播/监听）：
// executor timer（EXEC-04 submit_periodic_with_handle，EXEC-04 timer 能力
// 首次启用）周期轮询 NodeSession::endpoints()，diff 出新出现的**未信任**端点
// 合成 on_device_discovered（设计第 8.1 节：已知设备记录不重放 discovered；
// 重复出现在 diff 中为幂等 no-op，由 seen 集吸收）。目录条目即「对端正在
// 运行 Aki/已登录 Heyaki」的存活事实（LanPresence 签名验证 + 租约过期，
// heyaki 目录侧权威）：新合成设备 presence 取 Online；上一 tick 仍在广播、
// 本 tick 消失的设备经 on_device_presence(Offline) 回落——扫描列表只反映
// 当前存活的对端。TimerHandle 由本管道显式持有（EXEC-07）：stop() 取消句柄
// 后不再产生 tick（已入队/在飞 tick 不被抹除——executor 契约），满足
// 「stop 后无 discovered 事件」验收。start() 重置 seen/live 集：重新扫描
// 重新发现当前存活对端（历史 Unknown 行不阻塞再次上报）。
//
// 并发契约（EXEC-02）：sink 在 executor timer 上下文回调——消费方必须做有界
// 校验 + 投递（如 AppStateOwner.submit_update），不得执行业务处理；seen/live
// 集由互斥保护（周期任务软调度，tick 可能重叠）。RULE-09：start 失败（重复
// start / executor 拒绝）返回 false 可见，不静默。
//
// RULE-10：heyaki 类型封死在 heyaki/ 层（NodeSession 为层内类型），sink
// 收到 aki::device::DiscoveredDevice / DeviceId / PresenceState。
#pragma once

#include "device/discovery/discovery_types.hpp"
#include "device/device/device_types.hpp"
#include "heyaki/session/runtime_node.hpp"

#include <executor/executor.hpp>
#include <executor/timer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aki::heyaki {

// 单 tick 纯函数 diff 的输出（网络无关；unit 二进制独立覆盖）。
struct LanDiscoveryTick {
    // 新出现的未信任端点（presence = Online——条目即存活证明）。
    std::vector<aki::device::DiscoveredDevice> discovered;
    // 上轮在广播、本轮从目录消失的设备（租约过期/对端退出）。
    std::vector<aki::device::DeviceId> went_offline;
};

// 发现累积态（seen：已合成 discovered 的设备——重复出现幂等 no-op；
// live：上一 tick 仍在广播的设备）。start() 重扫即整体重置。
struct LanDiscoveryState {
    std::set<std::string> seen;
    std::set<std::string> live;
};

// 纯函数 diff（peer_sessions_pipeline::diff_peer_sessions 同型）：entries =
// 本轮目录条目（LAN/relay 合并目录，M7/DEC-028 决策 5——relay 条目与 LAN
// 条目同一 diff 语义，不建第二条管道）。trusted 条目跳过（已知设备不重放
// discovered，§8.1；其 presence 由 peer_sessions 会话事件承载）——若其 id
// 在上一轮 live 集中（配对完成瞬间由未信任毕业为信任），仅从 live 集移除、
// **不**合成 went_offline（信任转移不是离线，M5-11 修订）；无 32 字节身份
// 公钥的条目视为非 Aki 广播（指纹面缺失，不可确认）跳过。来源映射：LAN
// 可见（含 LAN+relay 双可见——LAN 直连语义优先）→ LanDiscovery/`lan:`
// 前缀；仅 relay 可见 → Relay/`relay:` 前缀。返回本轮事件并把 seen/live
// 推进到本 tick 之后的取值。
[[nodiscard]] inline LanDiscoveryTick diff_lan_discovery(
    const std::vector<EndpointView>& entries, LanDiscoveryState& state) {
    LanDiscoveryTick tick;
    std::set<std::string> live_now;
    for (const auto& entry : entries) {
        if (entry.trusted) {
            // 毕业为已知设备：presence 归会话事件承载（§8.1），从 live 集
            // 移除以免下面对账误报离线。
            state.live.erase(entry.device_id.value);
            continue;  // 已知设备记录不重放 discovered（§8.1）
        }
        if (entry.public_key.bytes.size() != 32) {
            continue;  // 非真实 Aki 身份广播（缺 identity_public_key）
        }
        live_now.insert(entry.device_id.value);
        if (!state.seen.insert(entry.device_id.value).second) {
            continue;  // 幂等：重复出现不重复合成
        }
        aki::device::DiscoveredDevice device;
        device.identity.id = entry.device_id;
        device.identity.public_key = entry.public_key;
        device.identity.trust_state = aki::device::TrustState::Unknown;
        // 目录条目 = 对端正在广播（LAN 签名验证/relay 记录签名验证 + 租约
        // 内）——即「正在运行 Aki」的存活证明（§8.1 触发语义，M5-11 修订；
        // relay 条目同语义——Ed25519 记录签名 + device_id 推导一致，M7）。
        device.identity.presence = aki::device::PresenceState::Online;
        // display_name/class/os/capabilities 占位（DEC-006 缺口：
        // LanPresence 不携带元数据；relay 记录同缺口——名称经配对信任后
        // 持久化，或对端同 LAN 时经 DEC-020 广播补充）。
        device.identity.display_name =
            entry.device_id.value.substr(0, 16);
        if (entry.lan_visible || !entry.relay_visible) {
            device.method = aki::device::DiscoveryMethod::LanDiscovery;
            device.endpoint.description = "lan:" + entry.endpoint_id;
        } else {
            device.method = aki::device::DiscoveryMethod::Relay;
            device.endpoint.description = "relay:" + entry.endpoint_id;
        }
        tick.discovered.push_back(std::move(device));
    }
    for (const auto& id : state.live) {
        if (live_now.count(id) == 0) {
            tick.went_offline.push_back(aki::device::DeviceId{id});
        }
    }
    state.live = std::move(live_now);
    return tick;
}

class LanDiscoveryPipeline {
public:
    using Sink = std::function<void(const aki::device::DiscoveredDevice&)>;
    // 存活回落面（M5-11）：上轮在广播、本轮消失的设备（EXEC-02 有界校验 +
    // 投递）。空 function 合法（不观察离线事件）。
    using PresenceSink =
        std::function<void(const aki::device::DeviceId&)>;

    LanDiscoveryPipeline(executor::Executor& executor,
        aki::heyaki::NodeSession& session, Sink sink,
        PresenceSink presence_sink = {})
        : executor_(executor), session_(session), sink_(std::move(sink)),
          presence_sink_(std::move(presence_sink)) {}

    LanDiscoveryPipeline(const LanDiscoveryPipeline&) = delete;
    LanDiscoveryPipeline& operator=(const LanDiscoveryPipeline&) = delete;

    ~LanDiscoveryPipeline() {
        stop();
    }

    // 启动观察管道（幂等：已运行返回 false）。重扫语义：seen/live 重置——
    // 重新扫描重新发现当前存活对端。返回 false = 未启动（重复 start /
    // executor 拒绝——如已 shutdown，RULE-09 可见）。
    [[nodiscard]] bool start(
        std::chrono::milliseconds period = std::chrono::milliseconds{500}) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (timer_.valid()) {
            return false;
        }
        state_.seen.clear();
        state_.live.clear();
        const auto generation = ++generation_;
        timer_ = executor_.submit_periodic_with_handle(
            static_cast<std::int64_t>(period.count()),
            [this, generation] { poll(generation); });
        return timer_.valid();
    }

    // cancel 不移除已排队/在飞 tick。与查询共用互斥边界，stop 返回后
    // 旧 tick 不再访问 Node；已合成事件仍可能投递。owner 必须保留
    // 本管道及 sink 至 Executor 排空，之后才能销毁。
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

    // 观测：已合成的 discovered 数（EXEC-06；跨上下文原子读）。
    [[nodiscard]] std::uint64_t discovered_count() const noexcept {
        return discovered_.load(std::memory_order_relaxed);
    }

private:
    void poll(std::uint64_t generation) {
        // diff：新出现的未信任端点合成 on_device_discovered（§8.1 触发语义）；
        // 消失端点合成 on_device_presence 离线回落（M5-11 存活语义）。
        LanDiscoveryTick tick;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (!timer_.valid() || generation != generation_) {
                return;
            }
            tick = diff_lan_discovery(session_.endpoints(), state_);
        }
        for (const auto& device : tick.discovered) {
            sink_(device);
            discovered_.fetch_add(1, std::memory_order_relaxed);
        }
        if (presence_sink_ != nullptr) {
            for (const auto& device : tick.went_offline) {
                presence_sink_(device);
            }
        }
        // sink 抛出：设备已入 seen（本轮不再合成），异常沿 tick 进入 executor
        // failure 体系（EXEC-06 可见，不静默重试）。
    }

    executor::Executor& executor_;
    aki::heyaki::NodeSession& session_;
    Sink sink_;
    PresenceSink presence_sink_;
    mutable std::mutex mutex_;
    LanDiscoveryState state_;
    executor::TimerHandle timer_;
    std::uint64_t generation_{0};
    std::atomic<std::uint64_t> discovered_{0};
};

}  // namespace aki::heyaki
