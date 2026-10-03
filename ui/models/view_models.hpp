// 四域视图模型派生（设计 §9.1 视图模型派生条款，M5-03；M5-04 信任/路径扩展；
// M5-05 消息视图/文件卡片 join）。
//
// 纯函数：快照（Store 值语义集合）→ 只读视图模型；网络无关可单测
// （tests/unit/test_ui_models.cpp）。本层为 EUI-NEO 无关的独立构建目标
// aki_ui_models（DEC-005「测试 exe 不链 eui」的落地面；RULE-10：仅 std/
// aki 类型）。页面不持有业务状态、不直写 Store（RULE-02）——只消费本处
// 派生结果；操作一律经注入出站接口 UiActions 下达（ui_actions.hpp）。
//
// 派生语义：
//   - 设备列表 = DeviceStore × presence/逐设备连接路径（DEC-015：路径为
//     DeviceStore 级易失集合，按设备键 join；无条目即 Unknown）× 信任操作
//     可用性（§4：Pending 可输入对端密码；本机持有或签发 grant 可撤销）；
//   - 会话列表 = ConversationStore × 最后消息摘要（MessageStore 按会话端点
//     归属过滤后取最新一条；DEC-009 ② 的会话解析约定：消息属于其
//     {sender, receiver} == {local, remote} 的会话）；
//   - 消息流 = MessageStore 按会话过滤（同端点归属约定，时间升序保持
//     Store 顺序）；MessageView 附加方向/投递态与媒体载荷的文件卡片 join
//     （TransferStore 按 TransferId 关联，§6.1② 消费侧 join；无传输行的
//     媒体消息显式兜底态——DEC-010 已登记的单侧到达边角）；
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
    std::string remark;
    std::string os_name;
    aki::device::DeviceClass device_class = aki::device::DeviceClass::Other;
    aki::device::TrustState trust_state = aki::device::TrustState::Unknown;
    aki::device::PresenceState presence = aki::device::PresenceState::Offline;
    // Heyaki issued grant：本机签发给对端，表示本机信任对端。
    bool inbound_trust = false;
    // 逐设备连接路径（DEC-015：DeviceStore.connection_paths 按设备键 join；
    // 无条目 = Unknown——M5-04 退役全局单值摘要）。
    aki::device::ConnectionPath connection_path = aki::device::ConnectionPath::Unknown;
    // 公钥指纹可用性（指纹=DeviceId 规范串 hy1_…，即 id 字段本身；relay
    // 等来源可能缺完整的 32 字节公钥——缺失态显式呈现，不以 DeviceId
    // 冒充已验证指纹）。
    bool fingerprint_available = false;
    bool pairing_failed = false;

    // 信任操作可用性（§4 固定转移边；M5-04 信任操作面）。
    [[nodiscard]] bool can_confirm() const noexcept {
        return trust_state == aki::device::TrustState::Pending
            && fingerprint_available
            && connection_path != aki::device::ConnectionPath::Unknown;
    }
    [[nodiscard]] bool can_send_basic() const noexcept {
        return connection_path != aki::device::ConnectionPath::Unknown;
    }

    [[nodiscard]] bool can_connect() const noexcept {
        return fingerprint_available
            && presence == aki::device::PresenceState::Online
            && connection_path == aki::device::ConnectionPath::Unknown;
    }
    // 重新配对入口（DEC-021）：终态（Rejected/Revoked）设备可由用户显式
    // 发起全新配对轮，状态机边 Rejected/Revoked→Pending。
    [[nodiscard]] bool can_rebegin() const noexcept {
        return (trust_state == aki::device::TrustState::Rejected
                || trust_state == aki::device::TrustState::Revoked)
            && fingerprint_available;
    }
    [[nodiscard]] bool can_begin() const noexcept {
        return (trust_state == aki::device::TrustState::Unknown
                || can_rebegin())
            && fingerprint_available;
    }
    [[nodiscard]] bool can_reject() const noexcept {
        return trust_state == aki::device::TrustState::Pending
            && !inbound_trust;
    }
    [[nodiscard]] bool can_revoke() const noexcept {
        return trust_state == aki::device::TrustState::Trusted
            || inbound_trust;
    }

    // 四态信任显示键（DEC-022）：已有 grant 的事实优先于配对流程状态，
    // 避免本机已签发授权后仍只显示 Pending。
    // Trusted = 本机持有对端 grant，表示对端信任本机；
    // inbound_trust = 本机签发 grant，表示本机信任对端。
    // 互信/单向/互不信任；Pending/Rejected/Revoked 显示流程状态词。
    [[nodiscard]] const char* trust_relation_key() const noexcept {
        if (trust_state == aki::device::TrustState::Trusted) {
            return inbound_trust ? "Mutual trust" : "Trusted this device";
        }
        if (inbound_trust) return "Trusted by this device";
        switch (trust_state) {
            case aki::device::TrustState::Pending: return "Pending";
            case aki::device::TrustState::Rejected: return "Rejected";
            case aki::device::TrustState::Revoked: return "Revoked";
            case aki::device::TrustState::Unknown:
            case aki::device::TrustState::Trusted: break;
        }
        return "No trust established";
    }
};

[[nodiscard]] std::vector<DeviceView> derive_device_views(
    const aki::app::DeviceStore& store);

// ---- 会话域 ----

// 最后消息摘要（无消息时 has_value=false）。
struct LastMessageSummary {
    bool has_value = false;
    aki::conversation::MessageId id;
    aki::conversation::MessageType type = aki::conversation::MessageType::Text;
    aki::conversation::DeliveryState delivery = aki::conversation::DeliveryState::Queued;
    std::chrono::system_clock::time_point timestamp{};
    // 预览文本：Text/System 为文本本体；Image/Video/File 为媒体名
    //（"[image] name" 形态；M5-05 会话列表消费）。
    std::string preview;
    // 最后消息方向（outbound = sender == conversation.local_device；投递徽标
    // 仅己方消息展示——对端消息无投递态语义，§3）。
    bool outbound = false;
};

struct ConversationView {
    aki::conversation::ConversationId id;
    aki::device::DeviceId remote_device;
    aki::conversation::ConversationState state = aki::conversation::ConversationState::Active;
    LastMessageSummary last_message;
    bool pinned = false;
    std::size_t last_activity_order = 0;
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

// 单条消息的聊天窗口视图（M5-05）：方向/投递态/载荷语义 + 媒体消息的文件
// 卡片 join（TransferStore 按 TransferId 关联，§6.1② 消费侧 join）。
struct MessageView {
    aki::conversation::MessageId id;
    bool outbound = false;  // sender == local_device（§9.1 气泡左右归属）。
    aki::conversation::MessageType type = aki::conversation::MessageType::Text;
    aki::conversation::DeliveryState delivery = aki::conversation::DeliveryState::Queued;
    std::chrono::system_clock::time_point timestamp{};
    // 文本本体（Text/System）；媒体消息为空（媒体语义走 media/transfer）。
    std::string text;
    // 媒体载荷（Image/Video/File）：has_media = true；Image/Video/File 的
    // 气泡渲染为文件卡片（进度组件与 Transfers 页复用，aki_ui_design §4）。
    bool has_media = false;
    aki::transfer::FileMetadata media;
    aki::transfer::TransferId transfer_id;
    // 文件卡片 join（TransferStore 按 transfer_id 查找）：无传输行的媒体
    // 消息是 §6.1 已登记的单侧到达边角——transfer_tracked = false 时卡片
    // 以「无传输行」兜底态显式呈现，不猜测进度（DEC-010 边角登记）。
    bool transfer_tracked = false;
    aki::transfer::TransferState transfer_state = aki::transfer::TransferState::Queued;
    // 进度 fraction ∈ [0,1]；total==0 → 0（不除零，与 TransferView 同口径）。
    double transfer_progress = 0.0;
    // DEC-026: local archive facts, absent until persistence succeeds.
    std::string local_relative_path;
    bool local_file_available = false;
    std::string local_file_error;
};

// 选中会话的消息视图流（端点归属守卫同 derive_conversation_messages；
// 返回值语义副本——消息预算 4096（RULE-09），派生为有界工作单元）。
[[nodiscard]] std::vector<MessageView> derive_message_views(
    const aki::app::MessageStore& store,
    const aki::conversation::Conversation& conversation,
    aki::device::DeviceId local_device,
    const aki::app::TransferStore& transfers);

// ---- 传输域 ----

struct TransferView {
    aki::transfer::TransferId id;
    std::string file_name;
    std::string mime_type;  // 文件卡片媒体标注（M5-06；空 → 卡片兜底标注）。
    aki::device::DeviceId peer;  // 方向对端（outbound=receiver，inbound=sender）。
    aki::transfer::TransferState state = aki::transfer::TransferState::Queued;
    std::uint64_t transferred = 0;
    std::uint64_t total = 0;
    // 进度 fraction ∈ [0,1]；total==0 时为 0（避免除零；终态行由
    // state 解释）。
    double progress = 0.0;
    bool outbound = false;  // sender==local_device。
    bool terminal = false;  // is_terminal(state)。
    std::string local_relative_path;
    bool local_file_available = false;
    std::string local_file_error;

    // 操作可用性（M5-06 Transfers 页操作面；§3/§7 状态机固定边派生——
    // 返回值只作 UI 门控，操作本身经 UiActions 传输三接口下达，admission
    // 拒绝可见）：
    //   - can_pause：仅 Transferring（运行期暂停；Queued→Paused/
    //     Negotiating→Paused 为 DEC-013 重启降级边，非用户动作面）；
    //   - can_resume：仅 Paused（DEC-013② 恢复为显式动作；接收行恢复经
    //     wire 进度事件推进/对端重发，发送行经无会话取消——见 ⑥）；
    //   - can_cancel：全部非终态（含 Paused 孤儿降级行——DEC-013⑥ 无会话
    //     行 cancel_transfer 直接终态入口的 UI 触达）。
    [[nodiscard]] bool can_pause() const noexcept {
        return state == aki::transfer::TransferState::Transferring;
    }
    [[nodiscard]] bool can_resume() const noexcept {
        return state == aki::transfer::TransferState::Paused;
    }
    [[nodiscard]] bool can_cancel() const noexcept { return !terminal; }
};

[[nodiscard]] std::vector<TransferView> derive_transfer_views(
    const aki::app::TransferStore& store, aki::device::DeviceId local_device);

}  // namespace aki::ui::models
