// 四域视图模型派生（设计 §9.1 视图模型派生条款，M5-03）。
//
// 纯函数：快照（Store 值语义集合）→ 只读视图模型；网络无关可单测
// （tests/unit/test_ui_models.cpp）。本层为 EUI-NEO 无关的独立构建目标
// aki_ui_models（DEC-005「测试 exe 不链 eui」的落地面；RULE-10：仅 std/
// aki 类型）。页面不持有业务状态、不直写 Store（RULE-02）——只消费本处
// 派生结果；操作一律经注入出站接口 UiActions 下达（ui_actions.hpp）。
//
// 派生语义：
//   - 设备列表 = DeviceStore × presence/连接路径摘要（连接路径为状态 owner
//     LatestMailbox 的最新单值摘要，随派生入参注入——Store 不含路径字段）；
//   - 会话列表 = ConversationStore × 最后消息摘要（MessageStore 按会话端点
//     归属过滤后取最新一条；DEC-009 ② 的会话解析约定：消息属于其
//     {sender, receiver} == {local, remote} 的会话）；
//   - 消息流 = MessageStore 按会话过滤（同端点归属约定，时间升序保持
//     Store 顺序）；
//   - 传输列表 = TransferStore（含进度 fraction、方向、终态标志）。
#pragma once

#include "app/state/app_state.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace aki::ui::models {

// ---- 设备域 ----

struct DeviceView {
    aki::device::DeviceId id;
    std::string display_name;
    std::string os_name;
    aki::device::DeviceClass device_class = aki::device::DeviceClass::Other;
    aki::device::TrustState trust_state = aki::device::TrustState::Unknown;
    aki::device::PresenceState presence = aki::device::PresenceState::Offline;
    // 连接路径摘要（LatestMailbox 最新单值；无逐设备路径字段——M5-04 展示
    // 语义按 aki_ui_design §3 细化时如需逐设备路径，经 DEC 记录扩展 Store）。
    aki::device::ConnectionPath connection_path = aki::device::ConnectionPath::Unknown;
};

[[nodiscard]] std::vector<DeviceView> derive_device_views(
    const aki::app::DeviceStore& store,
    aki::device::ConnectionPath latest_connection_path);

// ---- 会话域 ----

// 最后消息摘要（无消息时 has_value=false）。
struct LastMessageSummary {
    bool has_value = false;
    aki::conversation::MessageId id;
    aki::conversation::MessageType type = aki::conversation::MessageType::Text;
    aki::conversation::DeliveryState delivery = aki::conversation::DeliveryState::Queued;
    std::chrono::system_clock::time_point timestamp{};
    // 预览文本：Text/System 为文本本体；Image/Video/File 为媒体名
    // （"[image] name" 形态）；具体展示语义归 M5-05。
    std::string preview;
};

struct ConversationView {
    aki::conversation::ConversationId id;
    aki::device::DeviceId remote_device;
    aki::conversation::ConversationState state = aki::conversation::ConversationState::Active;
    LastMessageSummary last_message;
};

[[nodiscard]] std::vector<ConversationView> derive_conversation_views(
    const aki::app::ConversationStore& conversations,
    const aki::app::MessageStore& messages);

// ---- 消息域 ----

// 消息流按会话过滤（端点归属约定；返回值语义副本，M5-05 滚动窗口优化时
// 再评估只读索引形态——公开契约变更须先改设计）。
[[nodiscard]] std::vector<aki::conversation::Message> derive_conversation_messages(
    const aki::app::MessageStore& store,
    const aki::conversation::Conversation& conversation,
    aki::device::DeviceId local_device);

// ---- 传输域 ----

struct TransferView {
    aki::transfer::TransferId id;
    std::string file_name;
    aki::device::DeviceId peer;  // 方向对端（outbound=receiver，inbound=sender）。
    aki::transfer::TransferState state = aki::transfer::TransferState::Queued;
    std::uint64_t transferred = 0;
    std::uint64_t total = 0;
    // 进度 fraction ∈ [0,1]；total==0 时为 0（避免除零；终态行由
    // state 解释）。
    double progress = 0.0;
    bool outbound = false;  // sender==local_device。
    bool terminal = false;  // is_terminal(state)。
};

[[nodiscard]] std::vector<TransferView> derive_transfer_views(
    const aki::app::TransferStore& store, aki::device::DeviceId local_device);

}  // namespace aki::ui::models
