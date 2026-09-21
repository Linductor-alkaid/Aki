// Application State（设计第 10 节）：网络侧与渲染侧之间唯一状态边界
// （DEC-002 / RULE-02）。四个 Store 直接复用 M1-01 领域类型，不含私有状态。
// M1 为带容量预算（AppStateLimits，RULE-09）的值语义集合体；整体复制成本可观时
// 须按设计第 10.1 节硬约束 2 改为 shared_ptr<const AppState> 不可变句柄发布。
#pragma once

#include "conversation/conversation/conversation_types.hpp"
#include "conversation/message/message_types.hpp"
#include "device/device/device_types.hpp"
#include "transfer/transfer/transfer_types.hpp"

#include <cstddef>
#include <vector>

namespace aki::app {

struct DeviceStore {
    std::vector<aki::device::DeviceIdentity> devices;
};

struct ConversationStore {
    std::vector<aki::conversation::Conversation> conversations;
};

struct MessageStore {
    std::vector<aki::conversation::Message> messages;
};

struct TransferStore {
    std::vector<aki::transfer::Transfer> transfers;
};

struct AppState {
    DeviceStore devices;
    ConversationStore conversations;
    MessageStore messages;
    TransferStore transfers;
};

// Store 容量预算（RULE-09）：超限的更新被状态 owner 明确拒绝并可观测，
// 不静默丢弃（设计第 10.1 节；默认值覆盖 M1/M2 规模，调整属公开契约变更）。
struct AppStateLimits {
    std::size_t max_devices = 256;
    std::size_t max_conversations = 256;
    std::size_t max_messages = 4096;
    std::size_t max_transfers = 256;
};

}  // namespace aki::app
