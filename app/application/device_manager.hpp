// Device Manager 骨架（设计第 8.3 节，DEC-008；M1-05）。
//
// 只写 devices Store：发现/连接/断开/路径事件 → UpsertDevice / SetPresence /
// SetConnectionPath（经 AppStateOwner 汇聚，RULE-02/EXEC-03）；发现启停是本域
// 出站操作（→ HeyakiAdapter）。事件与命令经单飞有界排空泵串行处理（EXEC-02：
// 业务不在 Adapter 回调线程），9 类事件中的 DeviceConnected / DeviceDisconnected
// 由本 Manager 发布主路径事件（设计第 8.3 节路由表）。
//
// 生命周期（EXEC-07）：executor 与 owner 依赖经构造注入；本对象必须先于其任务
// 终结——宿主关闭钩子 flush 至泵静止后才进入 EXEC-01 步骤 2~5（设计第 8.3 节）。
#pragma once

#include "app/application/manager_runtime.hpp"
#include "app/state/app_events.hpp"
#include "app/state/app_state_owner.hpp"
#include "app/state/app_state_updates.hpp"
#include "device/device/device_types.hpp"
#include "device/discovery/discovery_types.hpp"
#include "heyaki/adapter/heyaki_adapter.hpp"

#include <chrono>
#include <cstdint>
#include <utility>
#include <variant>

namespace aki::app {

struct DeviceDiscoveredWork {
    aki::device::DiscoveredDevice device;
};

struct DeviceConnectedWork {
    aki::device::DeviceId device;
    aki::device::ConnectionPath path = aki::device::ConnectionPath::Unknown;
};

struct DeviceDisconnectedWork {
    aki::device::DeviceId device;
};

struct ConnectionPathChangedWork {
    aki::device::DeviceId device;
    aki::device::ConnectionPath from = aki::device::ConnectionPath::Unknown;
    aki::device::ConnectionPath to = aki::device::ConnectionPath::Unknown;
};

struct StartDiscoveryWork {
    aki::device::DiscoveryMethod method = aki::device::DiscoveryMethod::LanDiscovery;
};

struct StopDiscoveryWork {};

using DeviceManagerWork = std::variant<DeviceDiscoveredWork,
    DeviceConnectedWork,
    DeviceDisconnectedWork,
    ConnectionPathChangedWork,
    StartDiscoveryWork,
    StopDiscoveryWork>;

class DeviceManager {
public:
    DeviceManager(executor::Executor& executor, AppStateOwner& state_owner,
        aki::heyaki::HeyakiAdapter& adapter, ManagerPumpOptions pump_options = {})
        : state_owner_(state_owner),
          adapter_(adapter),
          pump_(executor, std::move(pump_options),
              [this](DeviceManagerWork& work) { return handle(work); }) {}

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    // ---- Sink 路由入口（RouterSink 扇出；admission = 收件箱入队结果）----

    [[nodiscard]] bool enqueue_discovered(aki::device::DiscoveredDevice device) {
        return pump_.enqueue(DeviceDiscoveredWork{std::move(device)});
    }

    [[nodiscard]] bool enqueue_connected(
        aki::device::DeviceId device, aki::device::ConnectionPath path) {
        return pump_.enqueue(DeviceConnectedWork{std::move(device), path});
    }

    [[nodiscard]] bool enqueue_disconnected(aki::device::DeviceId device) {
        return pump_.enqueue(DeviceDisconnectedWork{std::move(device)});
    }

    [[nodiscard]] bool enqueue_connection_path_changed(aki::device::DeviceId device,
        aki::device::ConnectionPath from, aki::device::ConnectionPath to) {
        return pump_.enqueue(
            ConnectionPathChangedWork{std::move(device), from, to});
    }

    // ---- 本域出站操作（→ Adapter，业务在 Manager 上下文执行）----

    [[nodiscard]] bool start_discovery(aki::device::DiscoveryMethod method) {
        return pump_.enqueue(StartDiscoveryWork{method});
    }

    [[nodiscard]] bool stop_discovery() { return pump_.enqueue(StopDiscoveryWork{}); }

    // ---- 泵静止与观测（EXEC-06/07）----

    [[nodiscard]] bool flush(std::chrono::milliseconds budget) {
        return pump_.flush(budget);
    }

    [[nodiscard]] ManagerPumpStats::Snapshot stats() const noexcept {
        return pump_.stats();
    }

private:
    bool handle(DeviceManagerWork& work) {
        return std::visit([this](auto& item) { return handle(item); }, work);
    }

    bool handle(DeviceDiscoveredWork& work) {
        if (work.device.identity.id.empty()) {
            return false;  // 有界校验（EXEC-02 延续到 Manager 入口）。
        }
        const bool posted = post_event(DeviceDiscoveredEvent{work.device});
        const bool applied = state_owner_.submit_update(UpsertDevice{work.device.identity});
        return posted && applied;
    }

    bool handle(DeviceConnectedWork& work) {
        if (work.device.empty()) {
            return false;
        }
        // 主路径事件只由 DM 投递一次（设计第 8.3 节路由表）。
        const bool posted = post_event(DeviceConnectedEvent{work.device, work.path});
        const bool applied = state_owner_.submit_update(
            SetPresence{work.device, aki::device::PresenceState::Online});
        return posted && applied;
    }

    bool handle(DeviceDisconnectedWork& work) {
        if (work.device.empty()) {
            return false;
        }
        const bool posted = post_event(DeviceDisconnectedEvent{work.device});
        const bool applied = state_owner_.submit_update(
            SetPresence{work.device, aki::device::PresenceState::Offline});
        return posted && applied;
    }

    bool handle(ConnectionPathChangedWork& work) {
        if (work.device.empty()) {
            return false;
        }
        const bool posted =
            post_event(ConnectionPathChangedEvent{work.device, work.from, work.to});
        const bool applied = state_owner_.submit_update(SetConnectionPath{work.to});
        return posted && applied;
    }

    bool handle(StartDiscoveryWork& work) { return adapter_.start_discovery(work.method); }

    bool handle(StopDiscoveryWork&) {
        adapter_.stop_discovery();  // SPI：幂等停止。
        return true;
    }

    template <typename Payload>
    bool post_event(Payload payload) {
        AppEvent event;
        event.payload = std::move(payload);
        return state_owner_.post_event(std::move(event));
    }

    AppStateOwner& state_owner_;
    aki::heyaki::HeyakiAdapter& adapter_;
    ManagerPump<DeviceManagerWork> pump_;
};

}  // namespace aki::app
