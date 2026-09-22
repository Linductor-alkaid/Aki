// 仓储层实现（M2-04）。领域枚举 ↔ INTEGER 的映射值见 schema_v1.hpp 头注；
// 读回时对未知整数值抛出（防御被篡改的库，CHECK 只挡写入侧）。
#include "persistence/repository/repositories.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace aki::persistence {
namespace {

using aki::conversation::Conversation;
using aki::conversation::ConversationState;
using aki::conversation::MessagePayload;
using aki::conversation::ConversationId;
using aki::conversation::ConversationState;
using aki::conversation::DeliveryState;
using aki::conversation::FilePayload;
using aki::conversation::ImagePayload;
using aki::conversation::Message;
using aki::conversation::MessageId;
using aki::conversation::MessageType;
using aki::conversation::SystemPayload;
using aki::conversation::TextPayload;
using aki::conversation::VideoPayload;
using aki::device::DeviceCapabilities;
using aki::device::DeviceClass;
using aki::device::DeviceId;
using aki::device::DeviceIdentity;
using aki::device::PresenceState;
using aki::device::PublicKey;
using aki::device::TrustState;
using aki::transfer::FileMetadata;
using aki::transfer::Transfer;
using aki::transfer::TransferId;
using aki::transfer::TransferState;

[[noreturn]] void throw_unknown_row(const char* what) {
    throw std::runtime_error(std::string("persistence: no row for ") + what);
}

[[noreturn]] void throw_unknown_enum_value(const char* column, int value) {
    throw std::runtime_error(
        std::string("persistence: unknown value ") + std::to_string(value)
        + " for " + column + " (database content is not a known enum)");
}

int to_int(TrustState v) noexcept { return static_cast<int>(v); }
int to_int(DeviceClass v) noexcept { return static_cast<int>(v); }
int to_int(MessageType v) noexcept { return static_cast<int>(v); }
int to_int(ConversationState v) noexcept { return static_cast<int>(v); }
int to_int(DeliveryState v) noexcept { return static_cast<int>(v); }
int to_int(TransferState v) noexcept { return static_cast<int>(v); }

TrustState trust_state_from(int v) {
    switch (v) {
        case 0: return TrustState::Unknown;
        case 1: return TrustState::Pending;
        case 2: return TrustState::Trusted;
        case 3: return TrustState::Rejected;
        case 4: return TrustState::Revoked;
        default: throw_unknown_enum_value("trust_state", v);
    }
}

DeviceClass device_class_from(int v) {
    switch (v) {
        case 0: return DeviceClass::Desktop;
        case 1: return DeviceClass::Tablet;
        case 2: return DeviceClass::Phone;
        case 3: return DeviceClass::Server;
        case 4: return DeviceClass::Robot;
        case 5: return DeviceClass::Other;
        default: throw_unknown_enum_value("device_class", v);
    }
}

MessageType message_type_from(int v) {
    switch (v) {
        case 0: return MessageType::Text;
        case 1: return MessageType::Image;
        case 2: return MessageType::Video;
        case 3: return MessageType::File;
        case 4: return MessageType::System;
        default: throw_unknown_enum_value("type", v);
    }
}

ConversationState conversation_state_from(int v) {
    switch (v) {
        case 0: return ConversationState::Active;
        case 1: return ConversationState::Disconnected;
        case 2: return ConversationState::Archived;
        default: throw_unknown_enum_value("state", v);
    }
}

DeliveryState delivery_state_from(int v) {
    switch (v) {
        case 0: return DeliveryState::Queued;
        case 1: return DeliveryState::Sending;
        case 2: return DeliveryState::Sent;
        case 3: return DeliveryState::Delivered;
        case 4: return DeliveryState::Failed;
        default: throw_unknown_enum_value("delivery_state", v);
    }
}

TransferState transfer_state_from(int v) {
    switch (v) {
        case 0: return TransferState::Queued;
        case 1: return TransferState::Negotiating;
        case 2: return TransferState::Transferring;
        case 3: return TransferState::Paused;
        case 4: return TransferState::Completed;
        case 5: return TransferState::Failed;
        case 6: return TransferState::Cancelled;
        default: throw_unknown_enum_value("state", v);
    }
}

int capabilities_to_int(const DeviceCapabilities& c) noexcept {
    int bits = 0;
    if (c.messaging) bits |= 1 << 0;
    if (c.file_transfer) bits |= 1 << 1;
    if (c.status_query) bits |= 1 << 2;
    if (c.command_execution) bits |= 1 << 3;
    if (c.remote_terminal) bits |= 1 << 4;
    if (c.agent) bits |= 1 << 5;
    return bits;
}

DeviceCapabilities capabilities_from(int bits) {
    DeviceCapabilities c;
    c.messaging = (bits & (1 << 0)) != 0;
    c.file_transfer = (bits & (1 << 1)) != 0;
    c.status_query = (bits & (1 << 2)) != 0;
    c.command_execution = (bits & (1 << 3)) != 0;
    c.remote_terminal = (bits & (1 << 4)) != 0;
    c.agent = (bits & (1 << 5)) != 0;
    return c;
}

std::int64_t timestamp_to_int(
    std::chrono::system_clock::time_point timestamp) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        timestamp.time_since_epoch())
        .count();
}

std::chrono::system_clock::time_point timestamp_from(std::int64_t ns) {
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        std::chrono::time_point<std::chrono::system_clock,
            std::chrono::nanoseconds>{std::chrono::nanoseconds{ns}});
}

void bind_text_or_null(Statement& statement, int index, const std::string& v) {
    if (v.empty()) {
        statement.bind_null(index);
    } else {
        statement.bind(index, v);
    }
}

std::span<const std::byte> bytes_view(const PublicKey& key) {
    return {reinterpret_cast<const std::byte*>(key.bytes.data()),
        key.bytes.size()};
}

std::vector<std::uint8_t> to_u8_vector(std::vector<std::byte> blob) {
    return {reinterpret_cast<const std::uint8_t*>(blob.data()),
        reinterpret_cast<const std::uint8_t*>(blob.data() + blob.size())};
}

void bind_blob_or_null(Statement& statement, int index,
    const PublicKey& key) {
    if (key.bytes.empty()) {
        statement.bind_null(index);
    } else {
        statement.bind(index, bytes_view(key));
    }
}

// 语句执行助手：SqliteError 时把该语句逐出缓存（SQLite 3.53.4 对
// “约束失败后 reset 复用” 的崩溃缺陷规避，见 M2-04 验证记录），再重抛——
// 调用方重试时总是拿到 fresh prepare 的语句。
template <typename Op>
auto run_cached(StatementCache& cache, const char* sql, Op&& op)
    -> decltype(op(std::declval<Statement&>())) {
    try {
        return op(cache.get(sql));
    } catch (const SqliteError&) {
        cache.invalidate(sql);
        throw;
    }
}

}  // namespace

// ---- DeviceRepository ----

DeviceRepository::DeviceRepository(Database& database,
    std::size_t cache_capacity)
    : database_(&database), cache_(database, cache_capacity) {}

void DeviceRepository::upsert(const DeviceIdentity& identity) {
    // presence 为易失状态，不持久化（设计第 11.1 节 ①；恢复后默认 Offline）。
    run_cached(cache_,
        "INSERT INTO device (device_id, display_name, device_class, os_name,"
        " public_key, capabilities, trust_state) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)"
        " ON CONFLICT (device_id) DO UPDATE SET"
        " display_name = excluded.display_name,"
        " device_class = excluded.device_class,"
        " os_name = excluded.os_name,"
        " public_key = excluded.public_key,"
        " capabilities = excluded.capabilities,"
        " trust_state = excluded.trust_state;",
        [&](Statement& statement) {
            statement.bind(1, identity.id.value);
            statement.bind(2, identity.display_name);
            statement.bind(3, to_int(identity.device_class));
            statement.bind(4, identity.os_name);
            bind_blob_or_null(statement, 5, identity.public_key);
            statement.bind(6, capabilities_to_int(identity.capabilities));
            statement.bind(7, to_int(identity.trust_state));
            (void)statement.step();
        });
}

std::optional<DeviceIdentity> DeviceRepository::find(
    const DeviceId& device_id) {
    return run_cached(cache_,
        "SELECT device_id, display_name, device_class, os_name, public_key,"
        " capabilities, trust_state FROM device WHERE device_id = ?1;",
        [&](Statement& statement) -> std::optional<DeviceIdentity> {
            statement.bind(1, device_id.value);
            if (!statement.step()) {
                return std::nullopt;
            }
    DeviceIdentity identity;
    identity.id = DeviceId{statement.column_text(0)};
    identity.display_name = statement.column_text(1);
    identity.device_class = device_class_from(
        static_cast<int>(statement.column_int64(2)));
    identity.os_name = statement.column_text(3);
    identity.public_key.bytes = to_u8_vector(statement.column_blob(4));
    identity.capabilities = capabilities_from(
        static_cast<int>(statement.column_int64(5)));
    identity.trust_state = trust_state_from(
        static_cast<int>(statement.column_int64(6)));
    identity.presence = PresenceState::Offline;  // 易失状态：恢复后默认离线
            return identity;
        });
}

std::vector<DeviceIdentity> DeviceRepository::load_all() {
    return run_cached(cache_,
        "SELECT device_id, display_name, device_class, os_name, public_key,"
        " capabilities, trust_state FROM device ORDER BY rowid;",
        [&](Statement& statement) {
    std::vector<DeviceIdentity> result;
    while (statement.step()) {
        DeviceIdentity identity;
        identity.id = DeviceId{statement.column_text(0)};
        identity.display_name = statement.column_text(1);
        identity.device_class = device_class_from(
            static_cast<int>(statement.column_int64(2)));
        identity.os_name = statement.column_text(3);
        identity.public_key.bytes = to_u8_vector(statement.column_blob(4));
        identity.capabilities = capabilities_from(
            static_cast<int>(statement.column_int64(5)));
        identity.trust_state = trust_state_from(
            static_cast<int>(statement.column_int64(6)));
        identity.presence = PresenceState::Offline;
        result.push_back(std::move(identity));
    }
    return result;
        });
}

// ---- ConversationRepository ----

ConversationRepository::ConversationRepository(Database& database,
    std::size_t cache_capacity)
    : database_(&database), cache_(database, cache_capacity) {}

void ConversationRepository::upsert(const Conversation& conversation) {
    run_cached(cache_,
        "INSERT INTO conversation (conversation_id, local_device,"
        " remote_device, state) VALUES (?1, ?2, ?3, ?4)"
        " ON CONFLICT (conversation_id) DO UPDATE SET"
        " local_device = excluded.local_device,"
        " remote_device = excluded.remote_device,"
        " state = excluded.state;",
        [&](Statement& statement) {
            statement.bind(1, conversation.id.value);
            statement.bind(2, conversation.local_device.value);
            statement.bind(3, conversation.remote_device.value);
            statement.bind(4, to_int(conversation.state));
            (void)statement.step();
        });
}

std::optional<Conversation> ConversationRepository::find(
    const ConversationId& conversation_id) {
    return run_cached(cache_,
        "SELECT conversation_id, local_device, remote_device, state"
        " FROM conversation WHERE conversation_id = ?1;",
        [&](Statement& statement) -> std::optional<Conversation> {
            statement.bind(1, conversation_id.value);
            if (!statement.step()) {
                return std::nullopt;
            }
            Conversation conversation;
            conversation.id = ConversationId{statement.column_text(0)};
            conversation.local_device = DeviceId{statement.column_text(1)};
            conversation.remote_device = DeviceId{statement.column_text(2)};
            conversation.state = conversation_state_from(
                static_cast<int>(statement.column_int64(3)));
            return conversation;
        });
}

std::vector<Conversation> ConversationRepository::load_all() {
    return run_cached(cache_,
        "SELECT conversation_id, local_device, remote_device, state"
        " FROM conversation ORDER BY rowid;",
        [&](Statement& statement) {
            std::vector<Conversation> result;
            while (statement.step()) {
                Conversation conversation;
                conversation.id = ConversationId{statement.column_text(0)};
                conversation.local_device = DeviceId{statement.column_text(1)};
                conversation.remote_device = DeviceId{statement.column_text(2)};
                conversation.state = conversation_state_from(
                    static_cast<int>(statement.column_int64(3)));
                result.push_back(std::move(conversation));
            }
            return result;
        });
}

// ---- MessageRepository ----

MessageRepository::MessageRepository(Database& database,
    std::size_t cache_capacity)
    : database_(&database), cache_(database, cache_capacity) {}

void MessageRepository::upsert(const Message& message,
    const ConversationId& conversation_id) {
    // 会话归属由调用方提供（FK 强制）；type 与 payload 列按类型写列，
    // 其余置 NULL（读回按 type 重建 payload）。
    run_cached(cache_,
        "INSERT INTO message (message_id, conversation_id, sender, receiver,"
        " timestamp_ns, type, body_text, media_name, media_size, media_mime,"
        " media_transfer_id, delivery_state)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)"
        " ON CONFLICT (message_id) DO UPDATE SET"
        " conversation_id = excluded.conversation_id,"
        " sender = excluded.sender,"
        " receiver = excluded.receiver,"
        " timestamp_ns = excluded.timestamp_ns,"
        " type = excluded.type,"
        " body_text = excluded.body_text,"
        " media_name = excluded.media_name,"
        " media_size = excluded.media_size,"
        " media_mime = excluded.media_mime,"
        " media_transfer_id = excluded.media_transfer_id,"
        " delivery_state = excluded.delivery_state;",
        [&](Statement& statement) {
            statement.bind(1, message.id.value);
    statement.bind(2, conversation_id.value);
    statement.bind(3, message.sender.value);
    statement.bind(4, message.receiver.value);
    statement.bind(5, timestamp_to_int(message.timestamp));
    statement.bind(6, to_int(message.type));
    statement.bind_null(7);
    statement.bind_null(8);
    statement.bind_null(9);
    statement.bind_null(10);
    statement.bind_null(11);
    statement.bind(12, to_int(message.state));

    if (const auto* text = std::get_if<TextPayload>(&message.payload)) {
        statement.bind(7, text->text);
    } else if (const auto* system = std::get_if<SystemPayload>(
                   &message.payload)) {
        statement.bind(7, system->text);
    } else if (const auto* image = std::get_if<ImagePayload>(&message.payload)) {
        statement.bind(8, image->media.name);
        statement.bind(9, static_cast<std::int64_t>(image->media.size_bytes));
        statement.bind(10, image->media.mime_type);
    } else if (const auto* video = std::get_if<VideoPayload>(&message.payload)) {
        statement.bind(8, video->media.name);
        statement.bind(9, static_cast<std::int64_t>(video->media.size_bytes));
        statement.bind(10, video->media.mime_type);
    } else if (const auto* file = std::get_if<FilePayload>(&message.payload)) {
        statement.bind(8, file->file.name);
        statement.bind(9, static_cast<std::int64_t>(file->file.size_bytes));
        statement.bind(10, file->file.mime_type);
        bind_text_or_null(statement, 11, file->transfer_id.value);
    }
            (void)statement.step();
        });
}

MessagePayload payload_from_row(const Statement& row, MessageType type) {
    switch (type) {
        case MessageType::Text:
            return TextPayload{row.column_text(6)};
        case MessageType::System:
            return SystemPayload{row.column_text(6)};
        case MessageType::Image:
            return ImagePayload{FileMetadata{row.column_text(7),
                static_cast<std::uint64_t>(row.column_int64(8)),
                row.column_text(9)}};
        case MessageType::Video:
            return VideoPayload{FileMetadata{row.column_text(7),
                static_cast<std::uint64_t>(row.column_int64(8)),
                row.column_text(9)}};
        case MessageType::File:
            return FilePayload{
                FileMetadata{row.column_text(7),
                    static_cast<std::uint64_t>(row.column_int64(8)),
                    row.column_text(9)},
                TransferId{row.column_text(10)}};
    }
    throw_unknown_enum_value("type", to_int(type));
}

Message read_message(const Statement& row) {
    Message message;
    message.id = MessageId{row.column_text(0)};
    message.sender = DeviceId{row.column_text(2)};
    message.receiver = DeviceId{row.column_text(3)};
    message.timestamp = timestamp_from(row.column_int64(4));
    message.type = message_type_from(static_cast<int>(row.column_int64(5)));
    message.payload = payload_from_row(row, message.type);
    message.state = delivery_state_from(
        static_cast<int>(row.column_int64(11)));
    return message;
}

std::optional<Message> MessageRepository::find(const MessageId& message_id) {
    return run_cached(cache_,
        "SELECT message_id, conversation_id, sender, receiver, timestamp_ns,"
        " type, body_text, media_name, media_size, media_mime,"
        " media_transfer_id, delivery_state FROM message"
        " WHERE message_id = ?1;",
        [&](Statement& statement) -> std::optional<Message> {
            statement.bind(1, message_id.value);
            if (!statement.step()) {
                return std::nullopt;
            }
            return read_message(statement);
        });
}

std::vector<Message> MessageRepository::load_all() {
    return run_cached(cache_,
        "SELECT message_id, conversation_id, sender, receiver, timestamp_ns,"
        " type, body_text, media_name, media_size, media_mime,"
        " media_transfer_id, delivery_state FROM message ORDER BY rowid;",
        [&](Statement& statement) {
            std::vector<Message> result;
            while (statement.step()) {
                result.push_back(read_message(statement));
            }
            return result;
        });
}

void MessageRepository::set_delivery_state(const MessageId& message_id,
    DeliveryState state) {
    run_cached(cache_,
        "UPDATE message SET delivery_state = ?2 WHERE message_id = ?1;",
        [&](Statement& statement) {
            statement.bind(1, message_id.value);
            statement.bind(2, to_int(state));
            (void)statement.step();  // UPDATE 无行返回；命中与否看 changes()
            if (database_->changes() == 0) {
                throw_unknown_row("message (set_delivery_state)");
            }
        });
}

// ---- TransferRepository ----

TransferRepository::TransferRepository(Database& database,
    std::size_t cache_capacity)
    : database_(&database), cache_(database, cache_capacity) {}

void TransferRepository::upsert(const Transfer& transfer,
    const MessageId& message_id) {
    run_cached(cache_,
        "INSERT INTO transfer (transfer_id, message_id, sender, receiver,"
        " file_name, file_size, file_mime, transferred, total, state,"
        " stored_relative_path, stored_sha256, stored_size_bytes)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)"
        " ON CONFLICT (transfer_id) DO UPDATE SET"
        " message_id = excluded.message_id,"
        " sender = excluded.sender,"
        " receiver = excluded.receiver,"
        " file_name = excluded.file_name,"
        " file_size = excluded.file_size,"
        " file_mime = excluded.file_mime,"
        " transferred = excluded.transferred,"
        " total = excluded.total,"
        " state = excluded.state,"
        " stored_relative_path = excluded.stored_relative_path,"
        " stored_sha256 = excluded.stored_sha256,"
        " stored_size_bytes = excluded.stored_size_bytes;",
        [&](Statement& statement) {
            statement.bind(1, transfer.id.value);
            bind_text_or_null(statement, 2, message_id.value);
            statement.bind(3, transfer.sender.value);
            statement.bind(4, transfer.receiver.value);
            statement.bind(5, transfer.file.name);
            statement.bind(6,
                static_cast<std::int64_t>(transfer.file.size_bytes));
            statement.bind(7, transfer.file.mime_type);
            statement.bind(8,
                static_cast<std::int64_t>(transfer.transferred));
            statement.bind(9, static_cast<std::int64_t>(transfer.total));
            statement.bind(10, to_int(transfer.state));
            statement.bind_null(11);
            statement.bind_null(12);
            statement.bind_null(13);
            (void)statement.step();
        });
}

Transfer read_transfer(const Statement& row) {
    Transfer transfer;
    transfer.id = TransferId{row.column_text(0)};
    transfer.sender = DeviceId{row.column_text(2)};
    transfer.receiver = DeviceId{row.column_text(3)};
    transfer.file = FileMetadata{row.column_text(4),
        static_cast<std::uint64_t>(row.column_int64(5)), row.column_text(6)};
    transfer.transferred = static_cast<std::uint64_t>(row.column_int64(7));
    transfer.total = static_cast<std::uint64_t>(row.column_int64(8));
    transfer.state = transfer_state_from(static_cast<int>(row.column_int64(9)));
    return transfer;
}

std::optional<Transfer> TransferRepository::find(
    const TransferId& transfer_id) {
    return run_cached(cache_,
        "SELECT transfer_id, message_id, sender, receiver, file_name,"
        " file_size, file_mime, transferred, total, state,"
        " stored_relative_path, stored_sha256, stored_size_bytes"
        " FROM transfer WHERE transfer_id = ?1;",
        [&](Statement& statement) -> std::optional<Transfer> {
            statement.bind(1, transfer_id.value);
            if (!statement.step()) {
                return std::nullopt;
            }
            return read_transfer(statement);
        });
}

std::vector<Transfer> TransferRepository::load_all() {
    return run_cached(cache_,
        "SELECT transfer_id, message_id, sender, receiver, file_name,"
        " file_size, file_mime, transferred, total, state,"
        " stored_relative_path, stored_sha256, stored_size_bytes"
        " FROM transfer ORDER BY rowid;",
        [&](Statement& statement) {
            std::vector<Transfer> result;
            while (statement.step()) {
                result.push_back(read_transfer(statement));
            }
            return result;
        });
}

void TransferRepository::update_progress(const TransferId& transfer_id,
    std::uint64_t transferred, std::uint64_t total) {
    run_cached(cache_,
        "UPDATE transfer SET transferred = ?2, total = ?3"
        " WHERE transfer_id = ?1;",
        [&](Statement& statement) {
            statement.bind(1, transfer_id.value);
            statement.bind(2, static_cast<std::int64_t>(transferred));
            statement.bind(3, static_cast<std::int64_t>(total));
            (void)statement.step();  // UPDATE 无行返回；命中与否看 changes()
            if (database_->changes() == 0) {
                throw_unknown_row("transfer (update_progress)");
            }
        });
}

void TransferRepository::complete(const TransferId& transfer_id,
    TransferState final_state, const std::optional<CompletedFile>& file) {
    if (!aki::transfer::is_terminal(final_state)) {
        throw std::invalid_argument(
            "transfer completion requires a terminal state");
    }

    if (file.has_value()) {
        run_cached(cache_,
            "UPDATE transfer SET state = ?2, stored_relative_path = ?3,"
            " stored_sha256 = ?4, stored_size_bytes = ?5"
            " WHERE transfer_id = ?1;",
            [&](Statement& statement) {
                statement.bind(1, transfer_id.value);
                statement.bind(2, to_int(final_state));
                statement.bind(3, file->relative_path);
                statement.bind(4, file->sha256);
                statement.bind(5,
                    static_cast<std::int64_t>(file->size_bytes));
                (void)statement.step();  // UPDATE 无行返回；命中看 changes()
                if (database_->changes() == 0) {
                    throw_unknown_row("transfer (complete)");
                }
            });
        return;
    }

    run_cached(cache_,
        "UPDATE transfer SET state = ?2 WHERE transfer_id = ?1;",
        [&](Statement& statement) {
            statement.bind(1, transfer_id.value);
            statement.bind(2, to_int(final_state));
            (void)statement.step();  // UPDATE 无行返回；命中与否看 changes()
            if (database_->changes() == 0) {
                throw_unknown_row("transfer (complete)");
            }
        });
}

}  // namespace aki::persistence
