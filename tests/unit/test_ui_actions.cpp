// M5-03：UiActions 出站面边界测试（设计 §9.1「操作一律经 Application 出站
// 面」+ DEC-008 Manager 模式）。
//
// 覆盖：页面 → 注入出站接口（make_ui_actions 绑定）→ Manager 泵 →（Fake
// Adapter SPI / 状态 owner）的完整通道；入队 admission 结果直接可见（拒绝
// 不静默，RULE-09）；网络无关（FakeHeyakiAdapter，DEC-002）。
//
// 本文件不包含 eui 头（aki_ui_models 目标，DEC-005「测试 exe 不链 eui」）。
#include "app/application/conversation_manager.hpp"
#include "app/application/device_manager.hpp"
#include "app/application/message_manager.hpp"
#include "app/application/transfer_manager.hpp"
#include "app/lifecycle/executor_owner.hpp"
#include "app/state/app_state_owner.hpp"
#include "heyaki/adapter/fake_heyaki_adapter.hpp"
#include "ui/models/ui_actions.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>

namespace {

using namespace std::chrono_literals;
using namespace aki::app;

struct ActionStack {
    ExecutorOwner owner;
    AppStateOwner state;
    aki::heyaki::FakeHeyakiAdapter adapter;
    DeviceManager devices;
    ConversationManager conversations;
    MessageManager messages;
    TransferManager transfers;
    aki::ui::models::UiActions actions;

    explicit ActionStack(const std::string& name)
        : state{AppStateOwnerOptions{.name = "aki." + name + ".state"}},
          devices{owner.executor(), state, adapter, ManagerPumpOptions{.name = "aki." + name + ".dm"}},
          conversations{owner.executor(), state,
              ConversationManagerOptions{.pump = ManagerPumpOptions{.name = "aki." + name + ".cm"}}},
          messages{owner.executor(), state, adapter,
              MessageManagerOptions{.pump = ManagerPumpOptions{.name = "aki." + name + ".mm"},
                  .local_device = aki::device::DeviceId{"local"}}},
          transfers{owner.executor(), state, adapter,
              TransferManagerOptions{.pump = ManagerPumpOptions{.name = "aki." + name + ".tm"},
                  .sender = aki::device::DeviceId{"local"}}},
          actions{aki::ui::models::make_ui_actions(
              devices, conversations, messages, transfers)} {}

    void quiesce() {
        REQUIRE(devices.flush(2s));
        REQUIRE(conversations.flush(2s));
        REQUIRE(messages.flush(2s));
        REQUIRE(transfers.flush(2s));
        state.drain();
    }
};

}  // namespace

TEST_CASE("UiActions route page operations through Manager pumps to the SPI",
    "[unit][ui_actions][dec008]") {
    ActionStack stack{"actions"};

    // 会话域：ensure_conversation 显式建会话 → 泵 → 状态快照可见。
    REQUIRE(stack.actions.ensure_conversation(
        aki::device::DeviceId{"local"}, aki::device::DeviceId{"alpha"}));
    stack.quiesce();
    executor::comm::Snapshot<AppState> snapshot;
    int attempts = 0;
    while (!stack.state.try_load_snapshot(snapshot) && attempts < 64) {
        ++attempts;
    }
    REQUIRE(attempts < 64);
    REQUIRE(snapshot.value.conversations.conversations.size() == 1);
    REQUIRE(snapshot.value.conversations.conversations[0].id.value
        == "conv-alpha");
    REQUIRE(snapshot.value.conversations.conversations[0].remote_device.value
        == "alpha");

    // 消息域：send_text → 泵 → Adapter SPI 出站（Fake 记录）+ 消息行入快照。
    REQUIRE(stack.actions.send_text(aki::device::DeviceId{"alpha"},
        aki::conversation::MessageId{"m-1"}, "hello from the ui surface"));
    stack.quiesce();
    REQUIRE(stack.adapter.sent_texts().size() == 1);
    REQUIRE(stack.adapter.sent_texts()[0].to.value == "alpha");
    REQUIRE(stack.adapter.sent_texts()[0].text == "hello from the ui surface");
    REQUIRE(stack.state.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.messages.messages.size() == 1);
    REQUIRE(snapshot.value.messages.messages[0].id.value == "m-1");

    // 设备域：发现启停经出站接口 → Adapter（Fake 记录方法）。
    REQUIRE(stack.actions.start_discovery(
        aki::device::DiscoveryMethod::LanDiscovery));
    stack.quiesce();
    REQUIRE(stack.adapter.discovery_running());
    REQUIRE(stack.actions.stop_discovery());
    stack.quiesce();
    REQUIRE_FALSE(stack.adapter.discovery_running());
    REQUIRE(stack.adapter.discovery_started().size() == 1);

    // 入队 admission 结果直接可见（返回 bool 即泵收件箱 admission；拒绝
    // 不静默——满载拒绝形态由 ManagerPump 收件箱预算测试覆盖，此处断言
    // 正常路径 admission==true 且绑定面不吞返回值）。聚合载荷先落局部变量：
    // REQUIRE 宏的顶层逗号分割不受花括号保护（MSVC 实测）。
    const aki::transfer::FileMetadata image_media{
        .name = "pic.png", .size_bytes = 3, .mime_type = "image/png",
        .stored_sha256 = "ab"};
    const bool image_admitted =
        stack.actions.send_image(aki::device::DeviceId{"alpha"},
            aki::conversation::MessageId{"m-2"}, image_media,
            aki::transfer::TransferId{"hyt1_test"}, true);
    REQUIRE(image_admitted);
    stack.quiesce();
    REQUIRE(stack.adapter.sent_images().size() == 1);
    REQUIRE(stack.adapter.sent_images()[0].transfer_id.value == "hyt1_test");

    // 传输域绑定存在且可调（无接收端环境：start_transfer 入队 admission
    // 为断言面；wire 侧推进语义归 M4 既有回环/单测——此处锁定「页面命令
    // 经 UiActions 到达 Manager 泵」的通道）。
    const aki::transfer::FileMetadata archive_file{
        .name = "policy.pt", .size_bytes = 10,
        .mime_type = "application/octet-stream", .stored_sha256 = "ac"};
    REQUIRE(stack.actions.start_transfer(aki::device::DeviceId{"alpha"},
        aki::transfer::TransferId{"hyt1_t1"}, archive_file,
        std::filesystem::path{"payload/policy.pt"}));
    REQUIRE(stack.actions.pause_transfer(aki::transfer::TransferId{"hyt1_t1"}));
    REQUIRE(stack.actions.cancel_transfer(aki::transfer::TransferId{"hyt1_t1"}));
    stack.quiesce();
    REQUIRE(stack.adapter.transfer_commands().size() >= 1);
    REQUIRE(stack.adapter.transfer_commands().front().kind
        == aki::heyaki::FakeHeyakiAdapter::TransferCommand::Kind::Start);
    REQUIRE(stack.adapter.transfer_commands().front().source_path
        == std::filesystem::path{"payload/policy.pt"});

    // 全栈受控关闭（EXEC-01；本用例每栈独立 owner，AGENTS 规则 7/8）。
    const auto report = stack.owner.shutdown([&] {
        (void)stack.transfers.flush(2s);
        (void)stack.devices.flush(2s);
        (void)stack.conversations.flush(2s);
        (void)stack.messages.flush(2s);
        stack.state.close();
    });
    REQUIRE(report.fully_stopped());
}
