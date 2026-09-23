// LAN 发现观察管道（DEC-006 映射 2；设计第 8.1 节触发语义；M3-04）。
//
// start/stop_discovery 的真实语义 = 启停本管道（Node 常驻 LAN 广播/监听）：
// executor timer（EXEC-04 submit_periodic_with_handle，EXEC-04 timer 能力
// 首次启用）周期轮询 NodeSession::endpoints()，diff 出新出现的**未信任**端点
// 合成 on_device_discovered（设计第 8.1 节：已知设备记录不重放 discovered；
// 重复出现在 diff 中为幂等 no-op，由 seen 集吸收）。TimerHandle 由本管道
// 显式持有（EXEC-07）：stop() 取消句柄后不再产生 tick（已入队/在飞 tick 不
// 被抹除——executor 契约），满足「stop 后无 discovered 事件」验收。
//
// 并发契约（EXEC-02）：sink 在 executor timer 上下文回调——消费方必须做有界
// 校验 + 投递（如 AppStateOwner.submit_update），不得执行业务处理；seen 集
// 由互斥保护（周期任务软调度，tick 可能重叠）。RULE-09：start 失败（重复
// start / executor 拒绝）返回 false 可见，不静默。
//
// RULE-10：heyaki 类型封死在 heyaki/ 层（NodeSession 为层内类型），sink
// 收到 aki::device::DiscoveredDevice。
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

class LanDiscoveryPipeline {
public:
    using Sink = std::function<void(const aki::device::DiscoveredDevice&)>;

    LanDiscoveryPipeline(executor::Executor& executor,
        aki::heyaki::NodeSession& session, Sink sink)
        : executor_(executor), session_(session), sink_(std::move(sink)) {}

    LanDiscoveryPipeline(const LanDiscoveryPipeline&) = delete;
    LanDiscoveryPipeline& operator=(const LanDiscoveryPipeline&) = delete;

    ~LanDiscoveryPipeline() {
        stop();
    }

    // 启动观察管道（幂等：已运行返回 false）。返回 false = 未启动（重复
    // start / executor 拒绝——如已 shutdown，RULE-09 可见）。
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

    // 取消句柄：后续 tick 不再产生（在飞 tick 完成后 sink 不再被调用——
    // stop 后 seen 检查仍互斥，取消后 poll 不再被调度）。
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
    void poll() {
        // diff：新出现的未信任端点合成 on_device_discovered（§8.1 触发语义）。
        std::vector<aki::device::DiscoveredDevice> discovered;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            for (const auto& entry : session_.endpoints()) {
                if (entry.trusted) {
                    continue;  // 已知设备记录不重放 discovered（§8.1）
                }
                if (!seen_.insert(entry.device_id.value).second) {
                    continue;  // 幂等：重复出现不重复合成
                }
                aki::device::DiscoveredDevice device;
                device.identity.id = entry.device_id;
                device.identity.public_key = entry.public_key;
                device.identity.trust_state = aki::device::TrustState::Unknown;
                device.identity.presence = aki::device::PresenceState::Offline;
                // display_name/class/os/capabilities 占位（DEC-006 缺口：
                // LanPresence 不携带元数据）。
                device.identity.display_name = entry.device_id.value.substr(
                    0, 16);
                device.method = aki::device::DiscoveryMethod::LanDiscovery;
                device.endpoint.description =
                    "lan:" + entry.endpoint_id;
                discovered.push_back(std::move(device));
            }
        }
        for (const auto& device : discovered) {
            sink_(device);
            discovered_.fetch_add(1, std::memory_order_relaxed);
        }
        // sink 抛出：设备已入 seen（本轮不再合成），异常沿 tick 进入 executor
        // failure 体系（EXEC-06 可见，不静默重试）。
    }

    executor::Executor& executor_;
    aki::heyaki::NodeSession& session_;
    Sink sink_;
    mutable std::mutex mutex_;
    std::set<std::string> seen_;
    executor::TimerHandle timer_;
    std::atomic<std::uint64_t> discovered_{0};
};

}  // namespace aki::heyaki
