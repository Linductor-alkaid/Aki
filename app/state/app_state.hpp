// Application State（设计第 10 节）：网络侧与渲染侧之间唯一状态边界
// （DEC-002 / RULE-02）。四个 Store 直接复用 M1-01 领域类型，不含私有状态。
// M1 为带容量预算（AppStateLimits，RULE-09）的值语义集合体；整体复制成本可观时
// 须按设计第 10.1 节硬约束 2 改为 shared_ptr<const AppState> 不可变句柄发布。
#pragma once

#include "conversation/conversation/conversation_types.hpp"
#include "conversation/message/message_types.hpp"
#include "device/device/device_types.hpp"
#include "transfer/transfer/transfer_types.hpp"
#include "app/state/local_transfer_artifact.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace aki::app {

// 逐设备连接路径条目（M5-04，DEC-015）：当前会话的连接方式摘要——易失
// （不持久化，恢复后默认无条目即 Unknown），仅由 SetDeviceConnectionPath
// 写入（UpsertDevice 碰不到，避免发现事件整行替换覆写易失字段）。
struct DeviceConnectionPathEntry {
    aki::device::DeviceId device;
    aki::device::ConnectionPath path = aki::device::ConnectionPath::Unknown;

    friend bool operator==(const DeviceConnectionPathEntry&,
        const DeviceConnectionPathEntry&) = default;
};

struct DevicePairingFailureEntry {
    aki::device::DeviceId device;
    bool failed = false;
};

struct DeviceStore {
    std::vector<aki::device::DeviceIdentity> devices;
    // 逐设备连接路径（DEC-015）：按设备键 upsert；向量形态（设备预算
    // 256，线性查找在预算内，RULE-09）保持 AppState 值语义可比较。
    std::vector<DeviceConnectionPathEntry> connection_paths;
    // 易失的配对失败提示；每设备最多一条，不持久化、无明文口令。
    std::vector<DevicePairingFailureEntry> pairing_failures;
};

struct ConversationStore {
    std::vector<aki::conversation::Conversation> conversations;
};

struct MessageStore {
    std::vector<aki::conversation::Message> messages;
};

struct TransferStore {
    std::vector<aki::transfer::Transfer> transfers;
    std::vector<LocalTransferArtifact> local_artifacts;
};

// Relay 控制面注册与连接态（M7，DEC-028 决策 8）：配置状态非信任域，不做
// 状态机——SetRelayStatus 整体替换、幂等（同值 no-op 不发布）；易失不
// 持久化（enrollment 权威记录在 heyaki profile，重启后由装配首推重建本
// 字段）。connection_state 为上游 RelayNodeState 数值（语义名随行携带，
// 解释收敛在 heyaki/session 层）。
struct RelayStatus {
    bool enrolled = false;   // 存在有效（未撤销）enrollment 记录
    std::string relay_url;
    std::string tenant;
    int connection_state = 0;             // RelayNodeState 数值（0=disabled）
    std::string connection_state_name;    // 语义名（disabled/ready/…）
    std::string last_error;               // 最近一次注册/连接错误（可展示）

    friend bool operator==(const RelayStatus&, const RelayStatus&) = default;
};

struct AppState {
    DeviceStore devices;
    ConversationStore conversations;
    MessageStore messages;
    TransferStore transfers;
    // relay 状态（M7）：无注册时保持 nullopt（「未注册」即缺省展示）。
    std::optional<RelayStatus> relay;
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
