// 视图模型派生实现（纯函数；语义见 view_models.hpp）。
#include "ui/models/view_models.hpp"

#include "transfer/transfer/transfer_types.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace aki::ui::models {
namespace {

// 消息预览文本（M5-03 摘要语义：文本取本体，媒体取名称标注）。
std::string message_preview(const aki::conversation::Message& message) {
    using namespace aki::conversation;
    std::string preview;
    std::visit(
        [&preview](const auto& payload) {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, TextPayload>
                || std::is_same_v<Payload, SystemPayload>) {
                preview = payload.text;
            } else if constexpr (std::is_same_v<Payload, ImagePayload>) {
                preview = "[image] " + payload.media.name;
            } else if constexpr (std::is_same_v<Payload, VideoPayload>) {
                preview = "[video] " + payload.media.name;
            } else {
                preview = "[file] " + payload.file.name;
            }
        },
        message.payload);
    return preview;
}

// 消息归属判定（DEC-009 ② 会话解析约定的只读面）：消息属于会话当且仅当其
// 端点对 == 会话端点对（方向无关）。
bool message_belongs_to(const aki::conversation::Message& message,
    const aki::conversation::Conversation& conversation) {
    const bool forward = message.sender == conversation.local_device
        && message.receiver == conversation.remote_device;
    const bool reverse = message.sender == conversation.remote_device
        && message.receiver == conversation.local_device;
    return forward || reverse;
}

}  // namespace

std::vector<DeviceView> derive_device_views(const aki::app::DeviceStore& store) {
    std::vector<DeviceView> views;
    views.reserve(store.devices.size());
    for (const aki::device::DeviceIdentity& device : store.devices) {
        DeviceView view{device.id, device.display_name, device.os_name,
            device.device_class, device.trust_state, device.presence,
            aki::device::ConnectionPath::Unknown,
            !device.public_key.bytes.empty()};
        // 逐设备路径 join（DEC-015：DeviceStore.connection_paths 按设备键
        // 查找；无条目保持 Unknown）。设备预算 256，线性查找在预算内。
        for (const auto& entry : store.connection_paths) {
            if (entry.device == view.id) {
                view.connection_path = entry.path;
                break;
            }
        }
        views.push_back(std::move(view));
    }
    return views;
}

std::vector<ConversationView> derive_conversation_views(
    const aki::app::ConversationStore& conversations,
    const aki::app::MessageStore& messages) {
    std::vector<ConversationView> views;
    views.reserve(conversations.conversations.size());
    for (const aki::conversation::Conversation& conversation :
        conversations.conversations) {
        ConversationView view{conversation.id, conversation.remote_device,
            conversation.state, LastMessageSummary{}};
        // 最后消息摘要：Store 顺序即权威追加序（owner 单写者按接受顺序
        // push_back）；倒序扫描取首个归属消息，避免全量时间排序。
        for (auto it = messages.messages.rbegin();
            it != messages.messages.rend(); ++it) {
            if (!message_belongs_to(*it, conversation)) {
                continue;
            }
            view.last_message.has_value = true;
            view.last_message.id = it->id;
            view.last_message.type = it->type;
            view.last_message.delivery = it->state;
            view.last_message.timestamp = it->timestamp;
            view.last_message.preview = message_preview(*it);
            break;
        }
        views.push_back(std::move(view));
    }
    return views;
}

std::vector<aki::conversation::Message> derive_conversation_messages(
    const aki::app::MessageStore& store,
    const aki::conversation::Conversation& conversation,
    aki::device::DeviceId local_device) {
    // local_device 必须是会话端点之一（防错配：调用方以快照本地身份传入；
    // 不匹配时返回空流——可见的空态，不猜测归属）。
    std::vector<aki::conversation::Message> stream;
    if (!(local_device == conversation.local_device)
        && !(local_device == conversation.remote_device)) {
        return stream;
    }
    for (const aki::conversation::Message& message : store.messages) {
        if (message_belongs_to(message, conversation)) {
            stream.push_back(message);
        }
    }
    return stream;
}

std::vector<TransferView> derive_transfer_views(
    const aki::app::TransferStore& store, aki::device::DeviceId local_device) {
    std::vector<TransferView> views;
    views.reserve(store.transfers.size());
    for (const aki::transfer::Transfer& transfer : store.transfers) {
        TransferView view;
        view.id = transfer.id;
        view.file_name = transfer.file.name;
        view.peer = transfer.sender == local_device ? transfer.receiver
                                                    : transfer.sender;
        view.state = transfer.state;
        view.transferred = transfer.transferred;
        view.total = transfer.total;
        view.progress = transfer.total > 0
            ? static_cast<double>(transfer.transferred)
                / static_cast<double>(transfer.total)
            : 0.0;
        view.outbound = transfer.sender == local_device;
        view.terminal = aki::transfer::is_terminal(transfer.state);
        views.push_back(std::move(view));
    }
    return views;
}

}  // namespace aki::ui::models
