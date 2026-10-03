// Conversation Manager 骨架（设计第 8.3 节，DEC-008；M1-05）。
//
// 只写 conversations Store：connected/disconnected 扇出事件按自建会话记录推导
// UpsertConversation（Active <-> Disconnected，RULE-06 路径无关）；会话建立经
// 显式 ensure_conversation(local, remote)（宿主/用户流程调用，不从事件隐式建
// 会话）。M5-41: RouterSink 显式要求入站消息先建立会话，再投递 MM。
// 新建/恢复记录只含 id 与端点（不复制 owner 权威状态）；消息或
// 连接事件到达时若既无恢复记录也未 ensure_conversation，会话推导为幂等空操作。
//
// M1 会话 id 由本 Manager 确定性派生（prefix + remote，RULE-08 稳定 id）；
// M2 引入持久化后改为存储分配。生命周期（EXEC-07）同 DeviceManager。
#pragma once

#include "app/application/manager_runtime.hpp"
#include "app/state/app_state_owner.hpp"
#include "app/state/app_state_updates.hpp"
#include "conversation/conversation/conversation_types.hpp"
#include "conversation/message/message_types.hpp"
#include "device/device/device_types.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace aki::app {

struct PeerConnectedWork {
    aki::device::DeviceId remote;
};

struct PeerDisconnectedWork {
    aki::device::DeviceId remote;
};

struct EnsureConversationWork {
    aki::device::DeviceId local;
    aki::device::DeviceId remote;
    bool reveal_existing = true;
};

struct PinConversationWork {
    aki::conversation::ConversationId id;
    bool pinned;
};
struct HideConversationWork {
    aki::conversation::ConversationId id;
};

using IncomingMessageDelivery = std::function<bool(aki::conversation::Message)>;

struct IncomingConversationWork {
    aki::conversation::Message message;
    IncomingMessageDelivery deliver;
};

using ConversationManagerWork =
    std::variant<PeerConnectedWork, PeerDisconnectedWork, EnsureConversationWork,
                 IncomingConversationWork, PinConversationWork, HideConversationWork>;

// 构造选项置于命名空间作用域：类内嵌套 Options 的默认实参 `= {}` 在 GCC 下
// 非法（同 app_state_owner.hpp 的 AppStateOwnerOptions 处理）。
struct ConversationManagerOptions {
    ManagerPumpOptions pump{};
    std::string conversation_id_prefix = "conv-";
    // 组合根在网络事件源启动前传入 owner 同批恢复的会话。
    std::vector<aki::conversation::Conversation> seeded_rows;
};

class ConversationManager {
public:
    using Options = ConversationManagerOptions;

    ConversationManager(executor::Executor& executor, AppStateOwner& state_owner,
        ConversationManagerOptions options = {})
        : options_(std::move(options)),
          state_owner_(state_owner),
          pump_(executor, options_.pump,
              [this](ConversationManagerWork& work) { return handle(work); }) {
        for (const auto& row : options_.seeded_rows) {
            created_.emplace(row.remote_device.value,
                ConversationRecord{row.id, row.local_device, row.remote_device});
        }
        options_.seeded_rows.clear();
    }

    ConversationManager(const ConversationManager&) = delete;
    ConversationManager& operator=(const ConversationManager&) = delete;

    // ---- Sink 扇出入口（RouterSink 在 connected/disconnected 时调用）----

    [[nodiscard]] bool enqueue_peer_connected(aki::device::DeviceId remote) {
        return pump_.enqueue(PeerConnectedWork{std::move(remote)});
    }

    [[nodiscard]] bool enqueue_peer_disconnected(aki::device::DeviceId remote) {
        return pump_.enqueue(PeerDisconnectedWork{std::move(remote)});
    }

    // ---- 本域出站操作：显式建立一对一 Conversation（设计第 5 节）----

    [[nodiscard]] bool ensure_conversation(
        aki::device::DeviceId local, aki::device::DeviceId remote) {
        return pump_.enqueue(EnsureConversationWork{std::move(local), std::move(remote)});
    }

    // The external owner keeps the delivery target alive until CM then MM
    // have drained. This continuation runs on the existing CM pump only.
    [[nodiscard]] bool enqueue_incoming(aki::conversation::Message message,
        IncomingMessageDelivery deliver) {
        return pump_.enqueue(IncomingConversationWork{std::move(message), std::move(deliver)});
    }

    [[nodiscard]] bool set_pinned(aki::conversation::ConversationId id, bool pinned) {
        return pump_.enqueue(PinConversationWork{std::move(id), pinned});
    }

    [[nodiscard]] bool hide(aki::conversation::ConversationId id) {
        return pump_.enqueue(HideConversationWork{std::move(id)});
    }

    [[nodiscard]] bool flush(std::chrono::milliseconds budget) {
        return pump_.flush(budget);
    }

    [[nodiscard]] ManagerPumpStats::Snapshot stats() const noexcept {
        return pump_.stats();
    }

private:
    // 自建会话的创建记录：id 与端点；state 一律由事件推导写入 owner，
    // 不复制权威状态（DEC-008）。
    struct ConversationRecord {
        aki::conversation::ConversationId id;
        aki::device::DeviceId local;
        aki::device::DeviceId remote;
    };

    bool handle(ConversationManagerWork& work) {
        return std::visit([this](auto& item) { return handle(item); }, work);
    }

    bool handle(PeerConnectedWork& work) {
        if (work.remote.empty()) {
            return false;
        }
        const auto it = created_.find(work.remote.value);
        if (it == created_.end()) {
            return true;  // 未建会话：幂等空操作（DEC-008）。
        }
        aki::conversation::Conversation conversation;
        conversation.id = it->second.id;
        conversation.local_device = it->second.local;
        conversation.remote_device = it->second.remote;
        conversation.state = aki::conversation::ConversationState::Active;
        return state_owner_.submit_update(UpsertConversation{std::move(conversation)});
    }

    bool handle(PeerDisconnectedWork& work) {
        if (work.remote.empty()) {
            return false;
        }
        const auto it = created_.find(work.remote.value);
        if (it == created_.end()) {
            return true;
        }
        aki::conversation::Conversation conversation;
        conversation.id = it->second.id;
        conversation.local_device = it->second.local;
        conversation.remote_device = it->second.remote;
        conversation.state = aki::conversation::ConversationState::Disconnected;
        return state_owner_.submit_update(UpsertConversation{std::move(conversation)});
    }

    bool handle(PinConversationWork& work) {
        return !work.id.empty() &&
               state_owner_.submit_update(SetConversationPinned{work.id, work.pinned});
    }

    bool handle(HideConversationWork& work) {
        return !work.id.empty() && state_owner_.submit_update(SetConversationHidden{work.id, true});
    }

    bool handle(IncomingConversationWork& work) {
        if (work.message.id.empty() || work.message.sender.empty()
            || work.message.receiver.empty() || !work.deliver) {
            return false;
        }
        EnsureConversationWork ensure{work.message.receiver, work.message.sender, false};
        if (!handle(ensure)) return false;
        // The Conversation update is already admitted to the owner channel
        // before the Message Manager can enqueue its message update.
        return work.deliver(std::move(work.message));
    }

    bool handle(EnsureConversationWork& work) {
        if (work.local.empty() || work.remote.empty()) {
            return false;
        }
        if (created_.count(work.remote.value) != 0) {
            return !work.reveal_existing || state_owner_.submit_update(SetConversationHidden{
                                                created_.at(work.remote.value).id, false});
        }
        ConversationRecord record;
        record.id = aki::conversation::ConversationId{
            options_.conversation_id_prefix + work.remote.value};
        record.local = work.local;
        record.remote = work.remote;
        aki::conversation::Conversation conversation;
        conversation.id = record.id;
        conversation.local_device = record.local;
        conversation.remote_device = record.remote;
        conversation.state = aki::conversation::ConversationState::Active;
        if (!state_owner_.submit_update(UpsertConversation{std::move(conversation)})) {
            return false;  // owner 拒绝（容量超限）：记录不落地，保持无会话。
        }
        created_.emplace(work.remote.value, std::move(record));
        return true;
    }

    Options options_;
    AppStateOwner& state_owner_;
    std::map<std::string, ConversationRecord> created_;  // 仅排空上下文访问。
    ManagerPump<ConversationManagerWork> pump_;
};

}  // namespace aki::app
