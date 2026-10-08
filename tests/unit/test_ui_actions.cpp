// M5-03：UiActions 出站面边界测试（设计 §9.1「操作一律经 Application 出站
// 面」+ DEC-008 Manager 模式）。
// M5-05：发送面页面形改造覆盖——wire 标识生成（规范 hym1_/hyt1_ 串，§6.1
// 生成入口收敛）+ 图片 hash-first 编排路由（DEC-010/DEC-011）。
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

#include <heyaki/ids.hpp>

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
    kairo::comm::Snapshot<AppState> snapshot;
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
    // M5-05：message_id 由页面经 UiActions::new_message_id 取得（规范串，
    // 真实 NodeSession 双射可解析）。
    const auto text_message_id = stack.actions.new_message_id();
    REQUIRE(stack.actions.send_text(aki::device::DeviceId{"alpha"},
        text_message_id, "hello from the ui surface"));
    stack.quiesce();
    REQUIRE(stack.adapter.sent_texts().size() == 1);
    REQUIRE(stack.adapter.sent_texts()[0].to.value == "alpha");
    REQUIRE(stack.adapter.sent_texts()[0].text == "hello from the ui surface");
    REQUIRE(stack.adapter.sent_texts()[0].message_id.value
        == text_message_id.value);
    REQUIRE(stack.state.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.messages.messages.size() == 1);
    REQUIRE(snapshot.value.messages.messages[0].id.value
        == text_message_id.value);

    // 设备域：发现启停经出站接口 → Adapter（Fake 记录方法）。
    REQUIRE(stack.actions.start_discovery(
        aki::device::DiscoveryMethod::LanDiscovery));
    stack.quiesce();
    REQUIRE(stack.adapter.discovery_running());
    REQUIRE(stack.actions.stop_discovery());
    stack.quiesce();
    REQUIRE_FALSE(stack.adapter.discovery_running());
    REQUIRE(stack.adapter.discovery_started().size() == 1);

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
    // M5-06：恢复入口（Transfers 页操作面）经同一绑定面——Fake 记录 Resume
    // 命令（wire 侧语义归 M4-05 既有单测，此处锁定页面命令通道）。
    REQUIRE(stack.actions.resume_transfer(aki::transfer::TransferId{"hyt1_t1"}));
    REQUIRE(stack.actions.cancel_transfer(aki::transfer::TransferId{"hyt1_t1"}));
    stack.quiesce();
    REQUIRE(stack.adapter.transfer_commands().size() >= 3);
    REQUIRE(stack.adapter.transfer_commands().front().kind
        == aki::heyaki::FakeHeyakiAdapter::TransferCommand::Kind::Start);
    REQUIRE(stack.adapter.transfer_commands().front().source_path
        == std::filesystem::path{"payload/policy.pt"});
    bool saw_resume = false;
    for (const auto& command : stack.adapter.transfer_commands()) {
        if (command.kind
            == aki::heyaki::FakeHeyakiAdapter::TransferCommand::Kind::Resume) {
            saw_resume = true;
        }
    }
    REQUIRE(saw_resume);

    // 图片域（M5-05，hash-first 编排路由，DEC-010/DEC-011）：send_image →
    // TM.start_transfer 准入（闸门第 1 步）→ Fake 收到传输命令；无 IO 承载
    // 时 hash 延续以空 hash 触发、编排不发消息（§6.1②——消息半边全链路归
    // test_transfer_send_path，此处锁定「页面命令经 UiActions 进入编排」的
    // 通道与降级语义）。聚合载荷先落局部变量：REQUIRE 宏的顶层逗号分割不
    // 受花括号保护（MSVC 实测）。
    const aki::transfer::FileMetadata image_media{
        .name = "pic.png", .size_bytes = 3, .mime_type = "image/png",
        .stored_sha256 = ""};
    const auto image_message_id = stack.actions.new_message_id();
    const auto image_transfer_id = stack.actions.new_transfer_id();
    const bool image_admitted =
        stack.actions.send_image(aki::device::DeviceId{"alpha"},
            image_message_id, image_media, image_transfer_id,
            std::filesystem::path{"payload/pic.png"});
    REQUIRE(image_admitted);
    stack.quiesce();
    REQUIRE(stack.adapter.sent_images().empty());
    // 传输命令第二笔 = 图片传输 Start（source_path 为对话框选取路径语义）。
    REQUIRE(stack.adapter.transfer_commands().size() >= 2);
    const auto& image_command = stack.adapter.transfer_commands().back();
    REQUIRE(image_command.kind
        == aki::heyaki::FakeHeyakiAdapter::TransferCommand::Kind::Start);
    REQUIRE(image_command.transfer_id.value == image_transfer_id.value);
    REQUIRE(image_command.source_path
        == std::filesystem::path{"payload/pic.png"});
    REQUIRE(stack.state.try_load_snapshot(snapshot));
    // 传输行已准入；无 IO 承载 → 空 hash 延续 → 无图片消息行（仅 send_text）。
    REQUIRE(snapshot.value.transfers.transfers.size() == 2);
    REQUIRE(snapshot.value.messages.messages.size() == 1);

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

TEST_CASE("UiActions wire id generation is canonical and unique",
    "[unit][ui_actions][dec010][m5_05]") {
    ActionStack stack{"ids"};

    // 规范形式：真实 NodeSession 的 to_heyaki_*_id 双射可解析（非规范串在
    // 真实路径被拒——admission false、消息行 Failed，§6.1）。
    const auto message_a = stack.actions.new_message_id();
    const auto message_b = stack.actions.new_message_id();
    const auto transfer_a = stack.actions.new_transfer_id();
    const auto transfer_b = stack.actions.new_transfer_id();

    const auto parsed_message = ::heyaki::parse_message_id(message_a.value);
    REQUIRE(static_cast<bool>(parsed_message));
    REQUIRE(parsed_message.error == ::heyaki::IdentifierDecodeError::none);
    const auto parsed_transfer = ::heyaki::parse_transfer_id(transfer_a.value);
    REQUIRE(static_cast<bool>(parsed_transfer));
    REQUIRE(parsed_transfer.error == ::heyaki::IdentifierDecodeError::none);

    // 唯一性（16 随机字节；同型两次生成不同——碰撞概率可忽略）。
    REQUIRE(message_a.value != message_b.value);
    REQUIRE(transfer_a.value != transfer_b.value);

    const auto report = stack.owner.shutdown([&] {
        stack.state.close();
    });
    REQUIRE(report.fully_stopped());
}

TEST_CASE("Chat actions preserve hidden history and only new inbound messages reveal it",
          "[unit][ui_actions][dec027]") {
    ActionStack stack{"chat-controls"};
    const aki::conversation::ConversationId id{"conv-alpha"};
    REQUIRE(stack.actions.ensure_conversation(aki::device::DeviceId{"local"},
                                              aki::device::DeviceId{"alpha"}));
    stack.quiesce();
    auto message = aki::conversation::Message{};
    message.id = aki::conversation::MessageId{"existing"};
    message.sender = aki::device::DeviceId{"alpha"};
    message.receiver = aki::device::DeviceId{"local"};
    message.payload = aki::conversation::TextPayload{"history"};
    message.state = aki::conversation::DeliveryState::Delivered;
    REQUIRE(stack.state.submit_update(UpsertMessage{message, id}));
    stack.quiesce();
    REQUIRE(stack.actions.set_conversation_pinned(id, true));
    stack.quiesce();
    kairo::comm::Snapshot<AppState> snapshot;
    REQUIRE(stack.state.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.conversations.conversations[0].pinned);
    REQUIRE(stack.actions.hide_conversation(id));
    stack.quiesce();
    REQUIRE(stack.state.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.conversations.conversations[0].hidden);
    REQUIRE_FALSE(snapshot.value.conversations.conversations[0].pinned);
    REQUIRE(snapshot.value.messages.messages.size() == 1);

    REQUIRE(stack.conversations.enqueue_peer_connected(aki::device::DeviceId{"alpha"}));
    REQUIRE(stack.conversations.enqueue_peer_disconnected(aki::device::DeviceId{"alpha"}));
    // The incoming CM continuation must not itself reveal a duplicate message.
    REQUIRE(stack.conversations.enqueue_incoming(message, [&stack](const auto& incoming) {
        return stack.state.submit_update(
            UpsertMessage{incoming, aki::conversation::ConversationId{"conv-alpha"}});
    }));
    stack.quiesce();
    REQUIRE(stack.state.try_load_snapshot(snapshot));
    REQUIRE(snapshot.value.conversations.conversations[0].hidden);
    REQUIRE(snapshot.value.messages.messages.size() == 1);
    message.id = aki::conversation::MessageId{"new-inbound"};
    REQUIRE(stack.conversations.enqueue_incoming(message, [&stack](const auto& incoming) {
        return stack.state.submit_update(
            UpsertMessage{incoming, aki::conversation::ConversationId{"conv-alpha"}});
    }));
    stack.quiesce();
    REQUIRE(stack.state.try_load_snapshot(snapshot));
    REQUIRE_FALSE(snapshot.value.conversations.conversations[0].hidden);
    REQUIRE(snapshot.value.messages.messages.size() == 2);

    REQUIRE(stack.actions.hide_conversation(id));
    stack.quiesce();
    REQUIRE(stack.actions.ensure_conversation(aki::device::DeviceId{"local"},
                                              aki::device::DeviceId{"alpha"}));
    stack.quiesce();
    REQUIRE(stack.state.try_load_snapshot(snapshot));
    REQUIRE_FALSE(snapshot.value.conversations.conversations[0].hidden);
    REQUIRE(snapshot.value.messages.messages.size() == 2);
    stack.state.close();
    REQUIRE(stack.actions.hide_conversation(id));
    REQUIRE(stack.conversations.flush(2s));
    REQUIRE(stack.conversations.stats().handler_rejections == 1);
}
