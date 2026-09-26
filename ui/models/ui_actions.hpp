// UI 操作出站面（设计 §9.1 视图模型派生条款「操作一律经 Application 出站面」
// 的具体化，M5-03；M5-04 信任三操作扩展，DEC-008 Manager 模式）。
//
// 页面只持本接口（std::function 绑定面），不持有 Manager/Adapter/transport
// 对象（RULE-01/RULE-02）；绑定由组合根完成（aki_host 暴露 Manager 访问面，
// GUI main.cpp / console 驱动调用 make_ui_actions）。每个绑定直呼 Manager
// 公开出站方法（= Manager 泵入队 admission），返回值即入队结果——拒绝可见
// 不静默（RULE-09）。
//
// EUI-NEO 无关（RULE-10，纯 std/aki 类型）；信任判定操作（Pending 确认/
// 拒绝/Revoked 撤销）随 M5-04 落地（DEC-006 映射 3；口令处理在
// heyaki/adapter→DeviceManager 内部，DEC-016，不进本接口签名）。
#pragma once

#include "app/application/conversation_manager.hpp"
#include "app/application/device_manager.hpp"
#include "app/application/message_manager.hpp"
#include "app/application/transfer_manager.hpp"

#include <functional>

namespace aki::ui::models {

struct UiActions {
    // 消息域（MessageManager::send_text/send_image；transfer_admitted 语义
    // 见 SendImageWork——编排层闸门，M5-05 图片发送链路接入 hash-first）。
    std::function<bool(aki::device::DeviceId, aki::conversation::MessageId,
        std::string)>
        send_text;
    std::function<bool(aki::device::DeviceId, aki::conversation::MessageId,
        aki::transfer::FileMetadata, aki::transfer::TransferId, bool)>
        send_image;

    // 传输域（TransferManager 四接口）。
    std::function<bool(aki::device::DeviceId, aki::transfer::TransferId,
        aki::transfer::FileMetadata, std::filesystem::path)>
        start_transfer;
    std::function<bool(aki::transfer::TransferId)> pause_transfer;
    std::function<bool(aki::transfer::TransferId)> resume_transfer;
    std::function<bool(aki::transfer::TransferId)> cancel_transfer;

    // 设备域（DeviceManager 发现启停 + 信任三操作 M5-04；reject 为纯本地
    // 判定无 wire 面，confirm/revoke 经 SPI，结果经配对事件异步落地）。
    std::function<bool(aki::device::DiscoveryMethod)> start_discovery;
    std::function<bool()> stop_discovery;
    std::function<bool(aki::device::DeviceId)> confirm_pairing;
    std::function<bool(aki::device::DeviceId)> reject_device;
    std::function<bool(aki::device::DeviceId)> revoke_device;

    // 会话域（ConversationManager::ensure_conversation——显式建会话，不从
    // 事件隐式建，设计 §8.3）。
    std::function<bool(aki::device::DeviceId, aki::device::DeviceId)>
        ensure_conversation;
};

// 组合根绑定：四 Manager → UiActions（直呼公开出站方法，无中间状态）。
[[nodiscard]] UiActions make_ui_actions(aki::app::DeviceManager& devices,
    aki::app::ConversationManager& conversations,
    aki::app::MessageManager& messages, aki::app::TransferManager& transfers);

}  // namespace aki::ui::models
