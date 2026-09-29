// Device Manager（设计第 8.3 节，DEC-008；M1-05；M5-04 信任操作面扩展）。
//
// 只写 devices Store：发现/连接/断开/路径事件 → UpsertDevice / SetPresence /
// SetDeviceConnectionPath（DEC-015 逐设备路径：初连即提交映射路径、断连置
// Unknown，经 AppStateOwner 汇聚，RULE-02/EXEC-03）；发现启停与信任三操作
// （confirm/reject/revoke，DEC-006 映射 3）是本域出站操作（→ HeyakiAdapter /
// 本地状态机）。配对一次性结果（on_pairing_completed）→ PairingCompletedWork
// → UpsertDevice 信任转移（成功 Pending→Trusted、失败保留 Pending 可重试，
// 状态经 Store 快照可见，
// 不新增 AppEvent 主路径类型）。DEC-022：会话连接不改变 grant 状态，
// 本机取得对端签发 grant 后推进 Trusted，表示对端已信任本机。事件与命令经单飞有界排空泵串行处理
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

// 发现存活回落（M5-11）：LAN 目录租约过期/对端退出 → SetPresence（易失，
// 无主路径事件；在线方向由发现合成事件与 connected 承载）。
struct DevicePresenceWork {
    aki::device::DeviceId device;
    aki::device::PresenceState presence = aki::device::PresenceState::Offline;
};

struct DeviceNamedWork {
    aki::device::DeviceId device;
    aki::device::PublicKey public_key;
    std::string name;
};

struct SetDeviceRemarkWork {
    aki::device::DeviceId device;
    std::string remark;
};

struct DeviceConnectedWork {
    aki::device::DeviceId device;
    aki::device::ConnectionPath path = aki::device::ConnectionPath::Unknown;
};

struct TrustCalibrationWork {
    aki::device::DeviceId device;
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

struct BeginPairingWork {
    aki::device::DeviceId device;
};

struct PairingReadyWork {
    aki::device::DeviceId device;
    aki::device::PublicKey public_key;
};

// 信任三操作（M5-04，DEC-006 映射 3；转移合法性由 owner 信任状态机校验，
// 非法转移拒绝可见 RULE-08/09）。reject 为纯本地判定（无 wire 面）。
struct ConfirmPairingWork {
    aki::device::DeviceId device;
    std::string password;
};

struct RejectDeviceWork {
    aki::device::DeviceId device;
};

struct RevokeDeviceWork {
    aki::device::DeviceId device;
};

// 配对一次性结果（M5-04，on_pairing_completed 路由）：success →
// UpsertDevice(→ Trusted)、失败 → 易失失败标志；未知设备行
// 或非法转移由 owner 拒绝可观测。
struct PairingCompletedWork {
    aki::device::DeviceId device;
    bool success = false;
    std::string detail;
};

using DeviceManagerWork = std::variant<DeviceDiscoveredWork,
    DevicePresenceWork,
    DeviceNamedWork,
    SetDeviceRemarkWork,
    DeviceConnectedWork,
    TrustCalibrationWork,
    DeviceDisconnectedWork,
    ConnectionPathChangedWork,
    StartDiscoveryWork,
    StopDiscoveryWork,
    BeginPairingWork,
    PairingReadyWork,
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

    // 发现存活回落入口（M5-11；RouterSink 第 13 方法路由）。
    [[nodiscard]] bool enqueue_presence(aki::device::DeviceId device,
        aki::device::PresenceState presence) {
        return pump_.enqueue(
            DevicePresenceWork{std::move(device), presence});
    }

    [[nodiscard]] bool enqueue_connected(
        aki::device::DeviceId device, aki::device::ConnectionPath path) {
        return pump_.enqueue(DeviceConnectedWork{std::move(device), path});
    }

    [[nodiscard]] bool enqueue_trust_calibration(aki::device::DeviceId device) {
        return pump_.enqueue(TrustCalibrationWork{std::move(device)});
    }

    [[nodiscard]] bool enqueue_disconnected(aki::device::DeviceId device) {
        return pump_.enqueue(DeviceDisconnectedWork{std::move(device)});
    }

    [[nodiscard]] bool enqueue_name(aki::device::DeviceId device,
        aki::device::PublicKey public_key, std::string name) {
        return pump_.enqueue(DeviceNamedWork{std::move(device),
            std::move(public_key), std::move(name)});
    }

    [[nodiscard]] bool set_remark(aki::device::DeviceId device,
        std::string remark) {
        return pump_.enqueue(SetDeviceRemarkWork{
            std::move(device), std::move(remark)});
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

    [[nodiscard]] bool enqueue_pairing_ready(aki::device::DeviceId device,
        aki::device::PublicKey public_key = {}) {
        return pump_.enqueue(PairingReadyWork{std::move(device),
            std::move(public_key)});
    }

    // ---- 本域出站操作（→ Adapter / 本地状态机，业务在 Manager 上下文执行）----

    [[nodiscard]] bool start_discovery(aki::device::DiscoveryMethod method) {
        return pump_.enqueue(StartDiscoveryWork{method});
    }

    [[nodiscard]] bool stop_discovery() { return pump_.enqueue(StopDiscoveryWork{}); }

    // 信任三操作（M5-04）：confirm → SPI confirm_pairing（wire 面）；
    // revoke → SPI revoke_trust（wire 面）；reject → 纯本地判定
    //（Pending → Rejected，无 wire 面）。
    [[nodiscard]] bool begin_pairing(aki::device::DeviceId device) {
        return pump_.enqueue(BeginPairingWork{std::move(device)});
    }

    [[nodiscard]] bool confirm_pairing(aki::device::DeviceId device,
        std::string password) {
        return pump_.enqueue(ConfirmPairingWork{std::move(device),
            std::move(password)});
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
        // Discovery is a whole-row upsert; keep local-only remarks and a
        // previously verified name when the transport advertises no name.
        executor::comm::Snapshot<AppState> snapshot;
        if (state_owner_.try_load_snapshot(snapshot)) {
            for (const auto& existing : snapshot.value.devices.devices) {
                if (existing.id == work.device.identity.id) {
                    work.device.identity.remark = existing.remark;
                    if (!existing.display_name.empty()
                        && existing.display_name
                            != existing.id.value.substr(0, 16)) {
                        work.device.identity.display_name = existing.display_name;
                    }
                    break;
                }
            }
        }
        const bool posted = post_event(DeviceDiscoveredEvent{work.device});
        const bool applied = state_owner_.submit_update(UpsertDevice{work.device.identity});
        return posted && applied;
    }

    bool handle(DevicePresenceWork& work) {
        if (work.device.empty()) {
            return false;
        }
        // 只写易失 presence（未知 id 由 owner 拒绝可见）。
        return state_owner_.submit_update(SetPresence{work.device,
            work.presence});
    }

    bool handle(DeviceNamedWork& work) {
        if (work.device.empty() || work.name.empty() || work.name.size() > 64
            || work.public_key.bytes.size() != 32) return false;
        return state_owner_.submit_update(SetDeviceName{
            std::move(work.device), std::move(work.public_key),
            std::move(work.name)});
    }

    bool handle(SetDeviceRemarkWork& work) {
        if (work.device.empty() || work.remark.size() > 128) return false;
        return state_owner_.submit_update(SetDeviceRemark{
            std::move(work.device), std::move(work.remark)});
    }

    bool handle(DeviceConnectedWork& work) {
        if (work.device.empty()) {
            return false;
        }
        // 连接状态与 grant 方向分离。对端持有本机签发的 grant 对应
        // inbound_trust；本机取得对端签发 grant 才推进 Trusted。
        // 主路径事件只由 DM 投递一次（设计第 8.3 节路由表）；初连即提交
        // 映射路径（DEC-015，删除宿主侧 Lan 硬编码）。
        const bool posted = post_event(DeviceConnectedEvent{work.device, work.path});
        const bool presence = state_owner_.submit_update(
            SetPresence{work.device, aki::device::PresenceState::Online});
        const bool path = state_owner_.submit_update(
            SetDeviceConnectionPath{work.device, work.path});
        // 对向信任校准（DEC-021）：会话（重）裁定通过，本机 TrustStore 的
        // 双向 grant 有效性可能已变。有界本地查询后字段级落库；查询失败
        // （对端无 endpoint/会话上下文）不阻塞连接事实。
        (void)calibrate_trust(work.device);
        return posted && presence && path;
    }

    bool handle(TrustCalibrationWork& work) {
        return calibrate_trust(work.device);
    }

    bool calibrate_trust(const aki::device::DeviceId& device) {
        if (device.empty()) return false;
        auto directions = adapter_.trust_directions(device);
        if (!directions) return false;
        return state_owner_.submit_update(
            SetDeviceInboundTrust{device, directions->issued});
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

    bool handle(BeginPairingWork& work) {
        return !work.device.empty() && adapter_.begin_pairing(work.device);
    }

    bool handle(PairingReadyWork& work) {
        if (work.device.empty()) return false;
        executor::comm::Snapshot<AppState> snapshot;
        int attempts = 0;
        while (!state_owner_.try_load_snapshot(snapshot) && attempts < 64) {
            ++attempts;
        }
        if (attempts >= 64) return false;
        for (const auto& existing : snapshot.value.devices.devices) {
            if (existing.id != work.device) continue;
            if (existing.trust_state == aki::device::TrustState::Pending) {
                return true;
            }
            // Trusted 会话被裁定 restricted：双向有效 grant 均不存在——
            // 对端撤销（或 grant 过期）的可观测信号（DEC-021）。本机信任
            // 降级 Trusted→Revoked，双向显示随之归零。
            if (existing.trust_state == aki::device::TrustState::Trusted) {
                aki::device::DeviceIdentity downgraded = existing;
                downgraded.trust_state = aki::device::TrustState::Revoked;
                downgraded.inbound_trust = false;
                return state_owner_.submit_update(
                    UpsertDevice{std::move(downgraded)});
            }
            // 终态唯一出口：用户可见的重新配对轮把行放回 Pending
            // （Rejected/Revoked→Pending，DEC-021）；Unknown 正常首轮。
            const bool rebegin = existing.trust_state
                == aki::device::TrustState::Rejected
                || existing.trust_state == aki::device::TrustState::Revoked;
            if (existing.trust_state != aki::device::TrustState::Unknown
                && !rebegin) {
                return false;
            }
            aki::device::DeviceIdentity updated = existing;
            updated.trust_state = aki::device::TrustState::Pending;
            return state_owner_.submit_update(UpsertDevice{std::move(updated)});
        }
        // 对端先发起连接而本机尚未点击扫描：会话进入 restricted 时的 LAN
        // endpoint 携带已验证公钥，按 Unknown→Pending 两条更新有序入队。
        if (work.public_key.bytes.size() != 32) return false;
        aki::device::DeviceIdentity discovered;
        discovered.id = work.device;
        discovered.public_key = std::move(work.public_key);
        discovered.display_name = work.device.value.substr(0, 16);
        discovered.trust_state = aki::device::TrustState::Unknown;
        aki::device::DiscoveredDevice notice;
        notice.identity = discovered;
        notice.method = aki::device::DiscoveryMethod::LanDiscovery;
        const bool posted = post_event(DeviceDiscoveredEvent{notice});
        const bool inserted = state_owner_.submit_update(
            UpsertDevice{discovered});
        discovered.trust_state = aki::device::TrustState::Pending;
        const bool pending = state_owner_.submit_update(
            UpsertDevice{std::move(discovered)});
        return posted && inserted && pending;
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
        if (work.password.empty()
            || !adapter_.confirm_pairing(work.device,
                std::move(work.password))) {
            return false;
        }
        return state_owner_.submit_update(SetPairingFailure{work.device, false});
    }

    bool handle(RejectDeviceWork& work) {
        // 纯本地判定：Pending → Rejected（无 wire 面，DEC-006 映射 3）。
        const bool cleared = state_owner_.submit_update(
            SetPairingFailure{work.device, false});
        return cleared && apply_trust_transition(work.device,
            aki::device::TrustState::Rejected);
    }

    bool handle(RevokeDeviceWork& work) {
        // wire 面撤销全部有效 grant（无 grant 时 false 可见）。本机持有
        // 对端 grant 时 Trusted→Revoked；仅本机签发 grant 时清除
        // inbound_trust，不伪造 Unknown/Pending→Revoked 边。
        executor::comm::Snapshot<AppState> snapshot;
        if (!state_owner_.try_load_snapshot(snapshot)) return false;
        const aki::device::DeviceIdentity* existing = nullptr;
        for (const auto& device : snapshot.value.devices.devices) {
            if (device.id == work.device) {
                existing = &device;
                break;
            }
        }
        if (existing == nullptr
            || (existing->trust_state != aki::device::TrustState::Trusted
                && !existing->inbound_trust)) return false;
        if (!adapter_.revoke_trust(work.device)) {
            return false;
        }
        const bool transitioned = existing->trust_state
                == aki::device::TrustState::Trusted
            ? apply_trust_transition(work.device,
                aki::device::TrustState::Revoked)
            : true;
        const bool cleared = state_owner_.submit_update(
            SetDeviceInboundTrust{work.device, false});
        return transitioned && cleared;
    }

    bool handle(PairingCompletedWork& work) {
        if (work.device.empty()) {
            return false;  // 有界校验（EXEC-02 延续到 Manager 入口）。
        }
        if (!work.success) {
            // 错误口令保持 Pending，用户可重试；Rejected 只对应用户明确拒绝。
            return state_owner_.submit_update(
                SetPairingFailure{work.device, true});
        }
        const bool cleared = state_owner_.submit_update(
            SetPairingFailure{work.device, false});
        executor::comm::Snapshot<AppState> snapshot;
        if (state_owner_.try_load_snapshot(snapshot)) {
            for (const auto& existing : snapshot.value.devices.devices)
                if (existing.id == work.device
                    && existing.trust_state == aki::device::TrustState::Trusted)
                    return cleared;  // grant refreshed, trust state already terminal.
        }
        return cleared && apply_trust_transition(work.device,
            aki::device::TrustState::Trusted);
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
