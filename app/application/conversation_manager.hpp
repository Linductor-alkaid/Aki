// Conversation Manager 骨架（设计第 8.3 节，DEC-008；M1-05）。
//
// 只写 conversations Store：connected/disconnected 扇出事件按自建会话记录推导
// UpsertConversation（Active <-> Disconnected，RULE-06 路径无关）；会话建立经
// 显式 ensure_conversation(local, remote)（宿主/用户流程调用，不从事件隐式建
// 会话）。自建记录只含 id 与端点（创建记录，不复制 owner 权威状态）；消息或
// 连接事件先于 ensure_conversation 到达时，会话推导为幂等空操作。
//
// M1 会话 id 由本 Manager 确定性派生（prefix + remote，RULE-08 稳定 id）；
// M2 引入持久化后改为存储分配。生命周期（EXEC-07）同 DeviceManager。
#pragma once

#include "app/application/manager_runtime.hpp"
#include "app/state/app_state_owner.hpp"
#include "app/state/app_state_updates.hpp"
#include "conversation/conversation/conversation_types.hpp"
#include "device/device/device_types.hpp"

#include <chrono>
#include <map>
#include <string>
#include <utility>
#include <variant>

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
};

using ConversationManagerWork = std::variant<PeerConnectedWork,
    PeerDisconnectedWork,
    EnsureConversationWork>;

// 构造选项置于命名空间作用域：类内嵌套 Options 的默认实参 `= {}` 在 GCC 下
// 非法（同 app_state_owner.hpp 的 AppStateOwnerOptions 处理）。
struct ConversationManagerOptions {
    ManagerPumpOptions pump{};
    std::string conversation_id_prefix = "conv-";
};

class ConversationManager {
public:
    using Options = ConversationManagerOptions;

    ConversationManager(executor::Executor& executor, AppStateOwner& state_owner,
        ConversationManagerOptions options = {})
        : options_(std::move(options)),
          state_owner_(state_owner),
          pump_(executor, options_.pump,
              [this](ConversationManagerWork& work) { return handle(work); }) {}

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

    bool handle(EnsureConversationWork& work) {
        if (work.local.empty() || work.remote.empty()) {
            return false;
        }
        if (created_.count(work.remote.value) != 0) {
            return true;  // 已建：幂等 no-op，不回写（避免覆盖 owner 权威状态）。
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
