// UI 操作出站面（设计 §9.1 视图模型派生条款「操作一律经 Application 出站面」
// 的具体化，M5-03；M5-04 信任三操作扩展，DEC-008 Manager 模式；M5-05 发送
// 面页面形改造——wire id 生成收敛注入 + 图片 hash-first 编排）。
//
// 页面只持本接口（std::function 绑定面），不持有 Manager/Adapter/transport
// 对象（RULE-01/RULE-02）；绑定由组合根完成（aki_host 暴露 Manager 访问面，
// GUI main.cpp / console 驱动调用 make_ui_actions）。每个绑定直呼 Manager
// 公开出站方法 / 编排层入口（返回值即入队 admission），拒绝可见不静默
//（RULE-09）。
//
// M5-05 发送面：
//   - new_message_id/new_transfer_id 生成器绑定 heyaki/adapter/wire_ids 的
//     规范 id 入口（§6.1：TransferId 唯一性由调用方生成保证；MessageId 须
//     过真实 NodeSession 规范串双射）——页面不携带 wire 编码知识（RULE-10），
//     只经本接口取 id；生成器为纯函数（std::random_device），点击回调上下文
//     可调。
//   - send_image 绑定编排层 send_image_message_with_hash（app/application/
//     image_flow.hpp，DEC-010/DEC-011 hash-first：先传输准入，消息等
//     stored_sha256 完成后经 TM 泵延续发出；source_path 为文件对话框选取的
//     本地路径）。返回值为闸门第 1 步 admission。
//
// EUI-NEO 无关（RULE-10，纯 std/aki 类型）；设备认证从 Unknown 主动建链，
// Pending 输入目标端口令确认，经 Manager/Adapter 传递（DEC-018）。
#pragma once

#include "app/application/conversation_manager.hpp"
#include "app/application/device_manager.hpp"
#include "app/application/message_manager.hpp"
#include "app/application/transfer_manager.hpp"

#include <filesystem>
#include <functional>

namespace aki::ui::models {

struct UiActions {
    // wire 标识生成（M5-05；规范 hym1_/hyt1_ 串，§6.1 生成入口收敛）。
    std::function<aki::conversation::MessageId()> new_message_id;
    std::function<aki::transfer::TransferId()> new_transfer_id;

    // 消息域（MessageManager::send_text/send_image）。message_id 由页面经
    // new_message_id 取得后传入（RULE-08 稳定 id：id 在发送前生成、可用于
    // 本地乐观展示的对账键）。
    std::function<bool(aki::device::DeviceId, aki::conversation::MessageId,
        std::string)>
        send_text;
    // 图片发送 hash-first 编排（M5-05，DEC-010/DEC-011）：transfer_id 由
    // 页面经 new_transfer_id 取得；media 仅 name/size_bytes/mime_type（
    // stored_sha256 由编排层 hash 延续填充）；source_path 为文件对话框只读
    // 选取的本地路径。返回 false = 传输准入失败（消息行经编排补偿记 Failed）。
    std::function<bool(aki::device::DeviceId, aki::conversation::MessageId,
        aki::transfer::FileMetadata, aki::transfer::TransferId,
        std::filesystem::path)>
        send_image;
    std::function<bool(aki::device::DeviceId, aki::conversation::MessageId,
        aki::transfer::FileMetadata, aki::transfer::TransferId,
        std::filesystem::path)>
        send_file;

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
    std::function<bool(aki::device::DeviceId)> begin_pairing;
    std::function<bool(aki::device::DeviceId, std::string)> confirm_pairing;
    std::function<bool(std::string, std::string&)> set_local_pairing_password;
    std::function<bool(std::string)> set_language;
    // 本机设备名改名（HostRuntime::set_device_name；M5-16 Settings 入口）。
    std::function<bool(std::string)> set_device_name;
    std::function<bool(aki::device::DeviceId)> reject_device;
    std::function<bool(aki::device::DeviceId)> revoke_device;
    std::function<bool(aki::device::DeviceId, std::string)> set_device_remark;

    // Platform commands run only in user click handlers; no compose IO.
    std::function<bool(std::filesystem::path, std::string&)> open_file_folder;
    std::function<void(std::string)> copy_local_path;

    // 会话域（ConversationManager::ensure_conversation——显式建会话，不从
    // 事件隐式建，设计 §8.3）。
    std::function<bool(aki::device::DeviceId, aki::device::DeviceId)>
        ensure_conversation;
    std::function<bool(aki::conversation::ConversationId, bool)> set_conversation_pinned;
    std::function<bool(aki::conversation::ConversationId)> hide_conversation;
};

// 组合根绑定：四 Manager + wire id 生成器 + 图片 hash-first 编排 → UiActions。
[[nodiscard]] UiActions make_ui_actions(aki::app::DeviceManager& devices,
    aki::app::ConversationManager& conversations,
    aki::app::MessageManager& messages, aki::app::TransferManager& transfers);

}  // namespace aki::ui::models
