// M5-04：Devices 域信任操作面测试（DEC-006 映射 3；SCOPE-02/03；设计 §8.1）。
//
// 覆盖：
//   - DeviceManager 信任三操作（confirm/reject/revoke）合法/非法转移边
//     （§4 固定转移边：仅 Pending 可确认/拒绝、仅 Trusted 可撤销；非法
//     转移/未知设备拒绝可见，RULE-08/09）；
//   - 配对一次性结果路由（Fake inject → sink 第 12 方法 → RouterSink →
//     DM → UpsertDevice 信任转移；状态经 Store 快照可见）；
//   - UiActions 信任通道（页面→注入出站接口→Manager 泵→Fake SPI；
//     M5-03 make_ui_actions 形态扩展；入队 admission 与 handler 结果的
//     可见性分层——泵入队恒 true，业务拒绝经 stats/状态可见）；
//   - 逐设备路径（DEC-015）：connected 提交映射路径、断连置 Unknown。
//
// 网络无关（FakeHeyakiAdapter，DEC-002）；console exe 不链 eui（DEC-005）。
#include "app/application/conversation_manager.hpp"
#include "app/application/device_manager.hpp"
#include "app/application/message_manager.hpp"
#include "app/application/router_sink.hpp"
#include "app/application/transfer_manager.hpp"
#include "app/lifecycle/executor_owner.hpp"
#include "app/state/app_state_owner.hpp"
#include "app/state/app_state_updates.hpp"
#include "heyaki/adapter/fake_heyaki_adapter.hpp"
#include "ui/models/ui_actions.hpp"
#include "ui/models/view_models.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

namespace {

using namespace std::chrono_literals;
using namespace aki::app;

aki::device::DeviceIdentity make_device(const std::string& id,
    aki::device::TrustState trust) {
    aki::device::DeviceIdentity device;
    device.id = aki::device::DeviceId{id};
    device.display_name = "dev-" + id;
    device.os_name = "test-os";
    device.trust_state = trust;
    device.public_key.bytes = {1, 2, 3};
    return device;
}

struct TrustStack {
    ExecutorOwner owner;
    AppStateOwner state{AppStateOwnerOptions{.name = "aki.trust.state"}};
    aki::heyaki::FakeHeyakiAdapter adapter;
    DeviceManager devices{owner.executor(), state, adapter,
        ManagerPumpOptions{.name = "aki.trust.dm"}};
    ConversationManager conversations{owner.executor(), state,
        ConversationManagerOptions{
            .pump = ManagerPumpOptions{.name = "aki.trust.cm"}}};
    MessageManager messages{owner.executor(), state, adapter,
        MessageManagerOptions{.pump = ManagerPumpOptions{.name = "aki.trust.mm"},
            .local_device = aki::device::DeviceId{"local"}}};
    TransferManager transfers{owner.executor(), state, adapter,
        TransferManagerOptions{.pump = ManagerPumpOptions{.name = "aki.trust.tm"},
            .sender = aki::device::DeviceId{"local"}}};
    RouterSink router{devices, conversations, messages, transfers};
    aki::ui::models::UiActions actions{
        aki::ui::models::make_ui_actions(devices, conversations, messages,
            transfers)};

    TrustStack() {
        adapter.set_sink(&router);  // EXEC-02 启动段纪律（事件源最后接通）。
    }

    void settle() {
        REQUIRE(devices.flush(2s));
        (void)conversations.flush(2s);
        (void)messages.flush(2s);
        (void)transfers.flush(2s);
        state.drain();
    }

    [[nodiscard]] aki::device::TrustState trust_of(const std::string& id) {
        executor::comm::Snapshot<AppState> snapshot;
        int attempts = 0;
        while (!state.try_load_snapshot(snapshot) && attempts < 64) {
            ++attempts;
        }
        REQUIRE(attempts < 64);
        for (const auto& device : snapshot.value.devices.devices) {
            if (device.id.value == id) {
                return device.trust_state;
            }
        }
        return aki::device::TrustState::Unknown;
    }

    [[nodiscard]] aki::device::ConnectionPath path_of(const std::string& id) {
        executor::comm::Snapshot<AppState> snapshot;
        REQUIRE(state.try_load_snapshot(snapshot));
        for (const auto& entry : snapshot.value.devices.connection_paths) {
            if (entry.device.value == id) {
                return entry.path;
            }
        }
        return aki::device::ConnectionPath::Unknown;
    }
};

}  // namespace

TEST_CASE("Pairing completion routes to trust transitions (Pending edges)",
    "[unit][device_trust][dec006]") {
    TrustStack stack;
    REQUIRE(stack.state.submit_update(
        UpsertDevice{make_device("alpha", aki::device::TrustState::Pending)}));
    REQUIRE(stack.state.submit_update(
        UpsertDevice{make_device("beta", aki::device::TrustState::Pending)}));
    stack.settle();

    // 配对成功 → Trusted（DEC-006 映射 3；状态经 Store 快照可见）。
    REQUIRE(stack.adapter.inject_pairing_completed(
        aki::device::DeviceId{"alpha"}, true));
    stack.settle();
    REQUIRE(stack.trust_of("alpha") == aki::device::TrustState::Trusted);

    // 配对失败 → Rejected（Pending→Rejected 合法边）。
    REQUIRE(stack.adapter.inject_pairing_completed(
        aki::device::DeviceId{"beta"}, false, "password mismatch"));
    stack.settle();
    REQUIRE(stack.trust_of("beta") == aki::device::TrustState::Rejected);

    // 未知设备的配对结果：入队 admission true（RouterSink 层），业务面拒绝
    // 经 DM handler_rejections 可见（RULE-09：可见性分层——状态不变）。
    const auto handler_rejections_before =
        stack.devices.stats().handler_rejections;
    REQUIRE(stack.adapter.inject_pairing_completed(
        aki::device::DeviceId{"ghost"}, true));
    stack.settle();
    REQUIRE(stack.devices.stats().handler_rejections
        == handler_rejections_before + 1);
}

TEST_CASE("DeviceManager trust operations respect the fixed transition edges",
    "[unit][device_trust][rule08]") {
    TrustStack stack;
    REQUIRE(stack.state.submit_update(
        UpsertDevice{make_device("alpha", aki::device::TrustState::Pending)}));
    REQUIRE(stack.state.submit_update(
        UpsertDevice{make_device("beta", aki::device::TrustState::Trusted)}));
    stack.settle();

    // reject（纯本地）：Pending → Rejected；无 wire 调用记录（Fake 出站面
    // 的 pairing_submits/revoked_peers 均不增长）。
    REQUIRE(stack.devices.reject_device(aki::device::DeviceId{"alpha"}));
    stack.settle();
    REQUIRE(stack.trust_of("alpha") == aki::device::TrustState::Rejected);
    REQUIRE(stack.adapter.pairing_submits().empty());
    REQUIRE(stack.adapter.revoked_peers().empty());

    // revoke（wire 面）：Trusted → Revoked + SPI 记录。
    REQUIRE(stack.devices.revoke_device(aki::device::DeviceId{"beta"}));
    stack.settle();
    REQUIRE(stack.trust_of("beta") == aki::device::TrustState::Revoked);
    REQUIRE(stack.adapter.revoked_peers().size() == 1);
    REQUIRE(stack.adapter.revoked_peers()[0].value == "beta");

    // 非法转移边：对 Rejected 行 confirm——wire 提交 admission 可见，Fake
    // 默认成功结果落地时 owner 状态机拒绝 Rejected→Trusted（状态不变）。
    const auto rejected_before = stack.state.stats().updates_rejected;
    REQUIRE(stack.devices.confirm_pairing(aki::device::DeviceId{"alpha"}));
    stack.settle();
    REQUIRE(stack.trust_of("alpha") == aki::device::TrustState::Rejected);
    REQUIRE(stack.state.stats().updates_rejected == rejected_before + 1);

    // revoke 无有效 grant：wire 面无操作可见（revoked_peers 不增长、状态
    // 不变；handler_rejections 计入——可见性分层）。
    stack.adapter.set_has_valid_grant(false);
    const auto handler_rejections_before =
        stack.devices.stats().handler_rejections;
    REQUIRE(stack.devices.revoke_device(aki::device::DeviceId{"beta"}));
    stack.settle();
    REQUIRE(stack.adapter.revoked_peers().size() == 1);
    REQUIRE(stack.trust_of("beta") == aki::device::TrustState::Revoked);
    REQUIRE(stack.devices.stats().handler_rejections
        == handler_rejections_before + 1);
}

TEST_CASE("UiActions trust channel reaches the manager pump and the SPI",
    "[unit][device_trust][dec008][m503]") {
    TrustStack stack;
    REQUIRE(stack.state.submit_update(
        UpsertDevice{make_device("alpha", aki::device::TrustState::Pending)}));
    stack.settle();

    // 页面 → UiActions → DM 泵 → Fake SPI（confirm 记录 + 默认成功结果）。
    REQUIRE(stack.actions.confirm_pairing(aki::device::DeviceId{"alpha"}));
    stack.settle();
    REQUIRE(stack.adapter.pairing_submits().size() == 1);
    REQUIRE(stack.adapter.pairing_submits()[0].value == "alpha");
    REQUIRE(stack.trust_of("alpha") == aki::device::TrustState::Trusted);

    // Trusted 行：UiActions reject → 纯本地拒绝（Trusted→Rejected 非法 →
    // owner 拒绝可见，状态不变）。
    const auto rejected_before = stack.state.stats().updates_rejected;
    REQUIRE(stack.actions.reject_device(aki::device::DeviceId{"alpha"}));
    stack.settle();
    REQUIRE(stack.trust_of("alpha") == aki::device::TrustState::Trusted);
    REQUIRE(stack.state.stats().updates_rejected == rejected_before + 1);

    // Trusted 行：UiActions revoke → SPI + Revoked。
    REQUIRE(stack.actions.revoke_device(aki::device::DeviceId{"alpha"}));
    stack.settle();
    REQUIRE(stack.trust_of("alpha") == aki::device::TrustState::Revoked);
}

TEST_CASE("Connected carries the mapped path and disconnect resets to Unknown",
    "[unit][device_trust][dec015]") {
    TrustStack stack;
    REQUIRE(stack.state.submit_update(
        UpsertDevice{make_device("alpha", aki::device::TrustState::Trusted)}));
    stack.settle();

    // 初连即提交映射路径（DEC-015：删除宿主 Lan 硬编码——路径来自 diff）。
    REQUIRE(stack.adapter.inject_device_connected(
        aki::device::DeviceId{"alpha"}, aki::device::ConnectionPath::Lan));
    stack.settle();
    REQUIRE(stack.path_of("alpha") == aki::device::ConnectionPath::Lan);

    // 换路（路径事件）。
    REQUIRE(stack.adapter.inject_connection_path_changed(
        aki::device::DeviceId{"alpha"}, aki::device::ConnectionPath::Lan,
        aki::device::ConnectionPath::P2p));
    stack.settle();
    REQUIRE(stack.path_of("alpha") == aki::device::ConnectionPath::P2p);

    // 断连置 Unknown（离线不展示陈旧路径）。
    REQUIRE(stack.adapter.inject_device_disconnected(
        aki::device::DeviceId{"alpha"}));
    stack.settle();
    REQUIRE(stack.path_of("alpha") == aki::device::ConnectionPath::Unknown);

    // 重连恢复路径条目。
    REQUIRE(stack.adapter.inject_device_connected(
        aki::device::DeviceId{"alpha"}, aki::device::ConnectionPath::Relay));
    stack.settle();
    REQUIRE(stack.path_of("alpha") == aki::device::ConnectionPath::Relay);

    const auto report = stack.owner.shutdown([&stack] {
        (void)stack.devices.flush(2s);
        stack.state.close();
    });
    REQUIRE(report.fully_stopped());
}
