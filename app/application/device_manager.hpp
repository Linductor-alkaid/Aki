// Device Manager（设计第 8.3 节，DEC-008；M1-05；M5-04 信任操作面扩展）。
//
// 只写 devices Store：发现/连接/断开/路径事件 → UpsertDevice / SetPresence /
// SetDeviceConnectionPath（DEC-015 逐设备路径：初连即提交映射路径、断连置
// Unknown，经 AppStateOwner 汇聚，RULE-02/EXEC-03）；发现启停与信任三操作
// （confirm/reject/revoke，DEC-006 映射 3）是本域出站操作（→ HeyakiAdapter /
// 本地状态机）。配对一次性结果（on_pairing_completed）→ PairingCompletedWork
// → UpsertDevice 信任转移（Pending→Trusted/Rejected，状态经 Store 快照可见，
// 不新增 AppEvent 主路径类型）。事件与命令经单飞有界排空泵串行处理
// （EXEC-02：业务不在 Adapter 回调线程），9 类事件中的 DeviceConnected /
// DeviceDisconnected 由本 Manager 发布主路径事件（设计第 8.3 节路由表）。
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
#include <string>
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

// 信任三操作（M5-04，DEC-006 映射 3；转移合法性由 owner 信任状态机校验，
// 非法转移拒绝可见 RULE-08/09）。reject 为纯本地判定（无 wire 面）。
struct ConfirmPairingWork {
    aki::device::DeviceId device;
};

struct RejectDeviceWork {
    aki::device::DeviceId device;
};

struct RevokeDeviceWork {
    aki::device::DeviceId device;
};

// 配对一次性结果（M5-04，on_pairing_completed 路由）：success →
// UpsertDevice(→ Trusted)、失败 → UpsertDevice(→ Rejected)；未知设备行
// 或非法转移由 owner 拒绝可观测。
struct PairingCompletedWork {
    aki::device::DeviceId device;
    bool success = false;
    std::string detail;
};

using DeviceManagerWork = std::variant<DeviceDiscoveredWork,
    DeviceConnectedWork,
    DeviceDisconnectedWork,
    ConnectionPathChangedWork,
    StartDiscoveryWork,
    StopDiscoveryWork,
    ConfirmPairingWork,
    RejectDeviceWork,
    RevokeDeviceWork,
    PairingCompletedWork>;

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

    // 配对一次性结果路由（M5-04，sink 第 12 方法入口）。
    [[nodiscard]] bool enqueue_pairing_completed(aki::device::DeviceId device,
        bool success, std::string detail = {}) {
        return pump_.enqueue(PairingCompletedWork{std::move(device), success,
            std::move(detail)});
    }

    // ---- 本域出站操作（→ Adapter / 本地状态机，业务在 Manager 上下文执行）----

    [[nodiscard]] bool start_discovery(aki::device::DiscoveryMethod method) {
        return pump_.enqueue(StartDiscoveryWork{method});
    }

    [[nodiscard]] bool stop_discovery() { return pump_.enqueue(StopDiscoveryWork{}); }

    // 信任三操作（M5-04）：confirm → SPI confirm_pairing（wire 面）；
    // revoke → SPI revoke_trust（wire 面）；reject → 纯本地判定
    //（Pending → Rejected，无 wire 面）。
    [[nodiscard]] bool confirm_pairing(aki::device::DeviceId device) {
        return pump_.enqueue(ConfirmPairingWork{std::move(device)});
    }

    [[nodiscard]] bool reject_device(aki::device::DeviceId device) {
        return pump_.enqueue(RejectDeviceWork{std::move(device)});
    }

    [[nodiscard]] bool revoke_device(aki::device::DeviceId device) {
        return pump_.enqueue(RevokeDeviceWork{std::move(device)});
    }

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
        // 主路径事件只由 DM 投递一次（设计第 8.3 节路由表）；初连即提交
        // 映射路径（DEC-015，删除宿主侧 Lan 硬编码）。
        const bool posted = post_event(DeviceConnectedEvent{work.device, work.path});
        const bool presence = state_owner_.submit_update(
            SetPresence{work.device, aki::device::PresenceState::Online});
        const bool path = state_owner_.submit_update(
            SetDeviceConnectionPath{work.device, work.path});
        return posted && presence && path;
    }

    bool handle(DeviceDisconnectedWork& work) {
        if (work.device.empty()) {
            return false;
        }
        const bool posted = post_event(DeviceDisconnectedEvent{work.device});
        const bool presence = state_owner_.submit_update(
            SetPresence{work.device, aki::device::PresenceState::Offline});
        // 断连置 Unknown（DEC-015：离线不展示陈旧路径）。
        const bool path = state_owner_.submit_update(
            SetDeviceConnectionPath{work.device,
                aki::device::ConnectionPath::Unknown});
        return posted && presence && path;
    }

    bool handle(ConnectionPathChangedWork& work) {
        if (work.device.empty()) {
            return false;
        }
        const bool posted =
            post_event(ConnectionPathChangedEvent{work.device, work.from, work.to});
        const bool applied = state_owner_.submit_update(
            SetDeviceConnectionPath{work.device, work.to});
        return posted && applied;
    }

    bool handle(StartDiscoveryWork& work) { return adapter_.start_discovery(work.method); }

    bool handle(StopDiscoveryWork&) {
        adapter_.stop_discovery();  // SPI：幂等停止。
        return true;
    }

    // 信任操作的行改写需要当前行（UpsertDevice 为整行替换语义）：从最近
    // 已发布快照读取该设备行、替换信任状态后整体 upsert——owner 信任状态机
    // 校验转移合法性（Pending→Trusted/Rejected、Trusted→Revoked 合法；
    // 未知 id/非法转移拒绝可见）。快照相对在途更新的滞后窗口由用户操作
    // 节奏吸收（操作面数据本就来自快照，M5-04 记录登记）。
    [[nodiscard]] bool apply_trust_transition(
        const aki::device::DeviceId& device, aki::device::TrustState to) {
        if (device.empty()) {
            return false;
        }
        executor::comm::Snapshot<AppState> snapshot;
        int attempts = 0;
        while (!state_owner_.try_load_snapshot(snapshot) && attempts < 64) {
            ++attempts;
        }
        if (attempts >= 64) {
            return false;
        }
        for (const auto& existing : snapshot.value.devices.devices) {
            if (!(existing.id == device)) {
                continue;
            }
            aki::device::DeviceIdentity updated = existing;
            updated.trust_state = to;
            return state_owner_.submit_update(UpsertDevice{std::move(updated)});
        }
        return false;  // 未知设备：可见拒绝。
    }

    bool handle(ConfirmPairingWork& work) {
        // wire 面先行（pair_peer 提交 admission）；信任状态推进经配对结果
        // 事件（on_pairing_completed → PairingCompletedWork）异步落地——
        // 不得在提交时乐观改状态（DEC-006 映射 3 冻结顺序）。
        return adapter_.confirm_pairing(work.device);
    }

    bool handle(RejectDeviceWork& work) {
        // 纯本地判定：Pending → Rejected（无 wire 面，DEC-006 映射 3）。
        return apply_trust_transition(work.device,
            aki::device::TrustState::Rejected);
    }

    bool handle(RevokeDeviceWork& work) {
        // wire 面撤销全部有效 grant（无 grant 时 false 可见）+ 本地
        // Trusted → Revoked。
        if (!adapter_.revoke_trust(work.device)) {
            return false;
        }
        return apply_trust_transition(work.device,
            aki::device::TrustState::Revoked);
    }

    bool handle(PairingCompletedWork& work) {
        if (work.device.empty()) {
            return false;  // 有界校验（EXEC-02 延续到 Manager 入口）。
        }
        return apply_trust_transition(work.device,
            work.success ? aki::device::TrustState::Trusted
                         : aki::device::TrustState::Rejected);
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
