// 仓储层（DEC-004；设计第 11 节 ER 模型与第 11.1 节 ① 写路径映射；M2-04）。
//
// 四表实体与 M1 领域类型双向转换；API 面与设计第 11.1 节 ① 对齐：
//   UpsertDevice/UpsertConversation/UpsertMessage/UpsertTransfer → 整行 upsert；
//   UpdateTransferProgress → transferred/total 列更新（不新建行）；
//   SetDeliveryState → MESSAGE 送达状态列更新；
//   CompleteTransfer → TRANSFER 终态更新（含第 11.1 节 ④ 的
//   stored_relative_path / stored_sha256 / stored_size_bytes 回写位，M2-06）。
//
// RULE-10：本公开头只暴露 M1 领域类型与 std 标准类型，不含 sqlite3 类型。
// RULE-09：未命中行（进度/送达/终态更新目标不存在）以 std::runtime_error 明确
// 失败，不静默；非法枚举由 schema CHECK 在 DB 层拒绝并经 SqliteError 可见。
//
// 线程契约：非线程安全，与 M2-03 封装一致（启动恢复主线程 / M2-05 worker 串行）。
#pragma once

#include "conversation/conversation/conversation_types.hpp"
#include "conversation/message/message_types.hpp"
#include "device/device/device_types.hpp"
#include "persistence/database/database.hpp"
#include "persistence/repository/statement_cache.hpp"
#include "transfer/transfer/transfer_types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aki::persistence {

// 第 11.1 节 ④ 回写位：Completed 终态作业组（M2-06）产出的文件落地记录。
struct CompletedFile {
    std::string relative_path;  // POSIX 相对路径（files/<transfer_id>/<净化文件名>）
    std::string sha256;
    std::uint64_t size_bytes = 0;
};

class DeviceRepository {
public:
    explicit DeviceRepository(Database& database,
        std::size_t cache_capacity = 16);

    void upsert(const aki::device::DeviceIdentity& identity);
    [[nodiscard]] std::optional<aki::device::DeviceIdentity> find(
        const aki::device::DeviceId& device_id);
    [[nodiscard]] std::vector<aki::device::DeviceIdentity> load_all();

private:
    Database* database_;
    mutable StatementCache cache_;
};

class ConversationRepository {
public:
    explicit ConversationRepository(Database& database,
        std::size_t cache_capacity = 16);

    void upsert(const aki::conversation::Conversation& conversation);
    [[nodiscard]] std::optional<aki::conversation::Conversation> find(
        const aki::conversation::ConversationId& conversation_id);
    [[nodiscard]] std::vector<aki::conversation::Conversation> load_all();

private:
    Database* database_;
    mutable StatementCache cache_;
};

class MessageRepository {
public:
    explicit MessageRepository(Database& database,
        std::size_t cache_capacity = 16);

    // message 的会话归属由调用方提供（FK：MESSAGE 1-* CONVERSATION）。
    void upsert(const aki::conversation::Message& message,
        const aki::conversation::ConversationId& conversation_id);
    [[nodiscard]] std::optional<aki::conversation::Message> find(
        const aki::conversation::MessageId& message_id);
    [[nodiscard]] std::vector<aki::conversation::Message> load_all();

    // 设计第 11.1 节 ①：SetDeliveryState → 送达状态列更新；未命中行抛出。
    void set_delivery_state(const aki::conversation::MessageId& message_id,
        aki::conversation::DeliveryState state);

private:
    Database* database_;
    mutable StatementCache cache_;
};

class TransferRepository {
public:
    explicit TransferRepository(Database& database,
        std::size_t cache_capacity = 16);

    // message_id 可空：M1 宿主直发传输可不经消息行；提供时 FK 校验。
    void upsert(const aki::transfer::Transfer& transfer,
        const aki::conversation::MessageId& message_id = {});
    [[nodiscard]] std::optional<aki::transfer::Transfer> find(
        const aki::transfer::TransferId& transfer_id);
    [[nodiscard]] std::vector<aki::transfer::Transfer> load_all();

    // 设计第 11.1 节 ①：UpdateTransferProgress → 列更新，不新建行。
    void update_progress(const aki::transfer::TransferId& transfer_id,
        std::uint64_t transferred, std::uint64_t total);

    // 设计第 11.1 节 ①④：终态更新 + 文件落地回写位。final_state 必须为终态
    // （Completed/Failed/Cancelled），否则 std::invalid_argument；未命中行抛出。
    void complete(const aki::transfer::TransferId& transfer_id,
        aki::transfer::TransferState final_state,
        const std::optional<CompletedFile>& file = std::nullopt);

private:
    Database* database_;
    mutable StatementCache cache_;
};

}  // namespace aki::persistence
