// typed 更新 → DB 作业工厂（设计第 11.1 节 ①；M2-07）。
//
// §11.1 ① 写路径映射的域级作业形态：UpsertDevice/UpsertConversation/
// UpsertMessage/UpsertTransfer → 整行 upsert；UpdateTransferProgress → 进度列
// 更新；SetDeliveryState → 送达状态列更新；CompleteTransfer → 终态更新。
// 宿主接线层（组合根，owner 单写者上下文）按接受顺序把这些作业入队
// DatabaseWorker；作业顺序 = 更新接受顺序（单消费者串行保持，§11.1 ①）。
//
// 终态与文件本体联动的作业组（Completed→SHA-256+原子改名+回写位；
// Failed/Cancelled→.part 幂等删除）见 storage/file_jobs.hpp（§11.1 ④）；
// 本文件的 make_transfer_terminal_job 是无文件联动的终态列更新（与 discard
// 作业并列入队，覆盖「终态宣告但无文件动作」的行更新语义）。
//
// SetPresence 与 SetConnectionPath 无作业：presence 是易失在线状态（恢复后
// 默认 Offline），连接路径是覆盖式单值摘要（§11.1 ① 明确不持久化）。
//
// RULE-10：公开面仅领域类型与 std 类型。作业捕获值语义载荷（无共享可变状态）。
#pragma once

#include "conversation/conversation/conversation_types.hpp"
#include "conversation/message/message_types.hpp"
#include "device/device/device_types.hpp"
#include "persistence/database/database_worker.hpp"
#include "transfer/transfer/transfer_types.hpp"

#include <cstdint>
#include <utility>

namespace aki::persistence {

// UpsertDevice → DEVICE 整行 upsert（presence 列不存在，天然不持久化）。
[[nodiscard]] DbJob make_device_upsert_job(aki::device::DeviceIdentity device);

// UpsertConversation → CONVERSATION 整行 upsert。
[[nodiscard]] DbJob make_conversation_upsert_job(
    aki::conversation::Conversation conversation);

// UpsertMessage → MESSAGE 整行 upsert；会话归属由调用方提供（FK：
// MESSAGE 1-* CONVERSATION，M2-04 契约）。
[[nodiscard]] DbJob make_message_upsert_job(aki::conversation::Message message,
    aki::conversation::ConversationId conversation_id);

// SetDeliveryState → MESSAGE 送达状态列更新（不新建行；未命中行经
// runtime_error 经 promise 结算，RULE-09）。
[[nodiscard]] DbJob make_message_delivery_job(
    aki::conversation::MessageId message,
    aki::conversation::DeliveryState state);

// UpsertTransfer → TRANSFER 整行 upsert；message_id 可空（宿主直发传输）。
[[nodiscard]] DbJob make_transfer_upsert_job(aki::transfer::Transfer transfer,
    aki::conversation::MessageId message_id = {});

// UpdateTransferProgress → TRANSFER 进度列更新（不新建行）。
[[nodiscard]] DbJob make_transfer_progress_job(aki::transfer::TransferId transfer,
    std::uint64_t transferred, std::uint64_t total);

// CompleteTransfer（无文件联动分支）→ TRANSFER 终态列更新；final_state 必须
// 为终态（Completed/Failed/Cancelled），否则作业以 invalid_argument 结算。
[[nodiscard]] DbJob make_transfer_terminal_job(aki::transfer::TransferId transfer,
    aki::transfer::TransferState final_state);

}  // namespace aki::persistence
