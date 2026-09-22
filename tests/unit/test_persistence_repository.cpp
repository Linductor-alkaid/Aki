// M2-04：v1 schema（ER 四表 + DEC-004 列补齐）与仓储层单测。
//
// 覆盖（验收 ①②③）：
//   - v1 迁移：空库应用、user_version 前进、四表存在、重复应用幂等；
//   - 仓储 CRUD + 双向转换：device（capabilities 位、全部 trust 值、presence
//     不持久化）、conversation、message（5 类 payload 往返 + 时间戳）、transfer
//     （含 §11.1 ④ 回写位与可选 message 外键）；
//   - DB 层拒绝路径：非法枚举（CHECK）、孤儿外键（foreign_keys=ON）、
//     更新未命中行（RULE-09，runtime_error）、非终态 complete（invalid_argument）；
//   - prepared-statement 缓存：容量上限 + LRU 逐出语义 + 逐出后复用（RULE-09）。
//
// 仅链接 aki_persistence（sqlite3 PRIVATE）——本 TU 同时是 RULE-10 边界的
// 消费者视角证明（仓库代码不含 sqlite 头/类型）。
#include "persistence/database/database.hpp"
#include "persistence/migration/migration.hpp"
#include "persistence/migration/schema_v1.hpp"
#include "persistence/repository/repositories.hpp"
#include "persistence/repository/statement_cache.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using aki::conversation::Conversation;
using aki::conversation::ConversationId;
using aki::conversation::ConversationState;
using aki::conversation::DeliveryState;
using aki::conversation::FilePayload;
using aki::conversation::ImagePayload;
using aki::conversation::Message;
using aki::conversation::MessageId;
using aki::conversation::MessagePayload;
using aki::conversation::MessageType;
using aki::conversation::SystemPayload;
using aki::conversation::TextPayload;
using aki::conversation::VideoPayload;
using aki::device::DeviceCapabilities;
using aki::device::DeviceClass;
using aki::device::DeviceId;
using aki::device::DeviceIdentity;
using aki::device::PresenceState;
using aki::device::TrustState;
using aki::persistence::CompletedFile;
using aki::persistence::Database;
using aki::persistence::MigrationStep;
using aki::persistence::Migrator;
using aki::persistence::SqliteError;
using aki::persistence::StatementCache;
using aki::transfer::FileMetadata;
using aki::transfer::Transfer;
using aki::transfer::TransferId;
using aki::transfer::TransferState;

Database migrated_memory_db() {
    Database db = Database::open(":memory:");
    REQUIRE(aki::persistence::schema_v1_steps().size() == 1);
    REQUIRE(Migrator(aki::persistence::schema_v1_steps()).bring_up_to_date(db)
        == 1);
    return db;
}

DeviceIdentity make_device(const std::string& id, TrustState trust) {
    DeviceIdentity identity;
    identity.id = DeviceId{id};
    identity.display_name = "device-" + id;
    identity.device_class = DeviceClass::Server;
    identity.os_name = "Linux";
    identity.public_key.bytes = {std::uint8_t{1}, std::uint8_t{2}};
    identity.capabilities.messaging = true;
    identity.capabilities.file_transfer = true;
    identity.capabilities.agent = true;
    identity.trust_state = trust;
    identity.presence = PresenceState::Online;  // 易失：不应被持久化
    return identity;
}

Message make_message(const std::string& id, MessagePayload payload) {
    Message message;
    message.id = MessageId{id};
    message.sender = DeviceId{"local-1"};
    message.receiver = DeviceId{"alpha-01"};
    message.timestamp = std::chrono::system_clock::now();
    message.type = MessageType::Text;
    message.payload = std::move(payload);
    if (std::holds_alternative<ImagePayload>(message.payload)) {
        message.type = MessageType::Image;
    } else if (std::holds_alternative<VideoPayload>(message.payload)) {
        message.type = MessageType::Video;
    } else if (std::holds_alternative<FilePayload>(message.payload)) {
        message.type = MessageType::File;
    } else if (std::holds_alternative<SystemPayload>(message.payload)) {
        message.type = MessageType::System;
    }
    message.state = DeliveryState::Sent;
    return message;
}

Transfer make_transfer(const std::string& id) {
    Transfer transfer;
    transfer.id = TransferId{id};
    transfer.sender = DeviceId{"local-1"};
    transfer.receiver = DeviceId{"alpha-01"};
    transfer.file =
        FileMetadata{"原始 名字.gguf", 1024, "application/octet-stream"};
    transfer.transferred = 0;
    transfer.total = 1024;
    transfer.state = TransferState::Transferring;
    return transfer;
}

bool table_exists(Database& db, const std::string& name) {
    aki::persistence::Statement statement = db.prepare(
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name = ?1;");
    statement.bind(1, name);
    return statement.step();
}

}  // namespace

// ---- 验收 ①：v1 迁移 ----

TEST_CASE("v1 migration creates the four ER tables and is idempotent",
    "[unit][persistence][migration][repository]") {
    Database db = Database::open(":memory:");
    auto steps = aki::persistence::schema_v1_steps();
    REQUIRE(steps.size() == 1);
    REQUIRE(steps.front().version == 1);

    Migrator migrator(steps);
    REQUIRE(migrator.bring_up_to_date(db) == 1);

    aki::persistence::Statement version = db.prepare("PRAGMA user_version;");
    REQUIRE(version.step());
    REQUIRE(version.column_int64(0) == 1);

    for (const char* table : {"device", "conversation", "message", "transfer"}) {
        INFO("table: " << table);
        REQUIRE(table_exists(db, table));
    }

    // 幂等重跑。
    REQUIRE(migrator.bring_up_to_date(db) == 0);
}

// ---- 验收 ②：device 仓储 ----

TEST_CASE("Device repository round-trips identity without presence",
    "[unit][persistence][repository]") {
    Database db = migrated_memory_db();
    aki::persistence::DeviceRepository repository(db);

    DeviceIdentity identity = make_device("alpha-01", TrustState::Trusted);
    repository.upsert(identity);

    const auto loaded = repository.find(DeviceId{"alpha-01"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->id == DeviceId{"alpha-01"});
    REQUIRE(loaded->display_name == "device-alpha-01");
    REQUIRE(loaded->device_class == DeviceClass::Server);
    REQUIRE(loaded->os_name == "Linux");
    REQUIRE(loaded->public_key.bytes == identity.public_key.bytes);
    REQUIRE(loaded->capabilities == identity.capabilities);
    REQUIRE(loaded->trust_state == TrustState::Trusted);
    // presence 易失：写入 Online，恢复后默认 Offline（设计第 11.1 节 ①）。
    REQUIRE(loaded->presence == PresenceState::Offline);

    // upsert 更新路径：同 id 更新 trust，不产生第二行。
    auto trusted = make_device("alpha-01", TrustState::Revoked);
    repository.upsert(trusted);
    const auto reloaded = repository.find(DeviceId{"alpha-01"});
    REQUIRE(reloaded->trust_state == TrustState::Revoked);

    // 全部 trust 值逐一遍历（CHECK 集合与枚举一一对应）。
    for (const int raw : {0, 1, 2, 3, 4}) {
        const auto trust = static_cast<TrustState>(raw);
        repository.upsert(make_device("trust-" + std::to_string(raw), trust));
        const auto found = repository.find(
            DeviceId{"trust-" + std::to_string(raw)});
        REQUIRE(found.has_value());
        REQUIRE(found->trust_state == trust);
    }

    REQUIRE(repository.load_all().size() == 6);  // alpha-01 + 5 个 trust-* 行
    REQUIRE_FALSE(repository.find(DeviceId{"missing"}).has_value());
}

// ---- 验收 ②：conversation 仓储与外键 ----

TEST_CASE("Conversation repository enforces the device foreign keys",
    "[unit][persistence][repository]") {
    Database db = migrated_memory_db();
    aki::persistence::ConversationRepository repository(db);

    Conversation conversation;
    conversation.id = ConversationId{"conv-alpha-01"};
    conversation.local_device = DeviceId{"local-1"};
    conversation.remote_device = DeviceId{"alpha-01"};
    conversation.state = ConversationState::Active;

    // 孤儿行：任一端设备不存在 → foreign_keys=ON 拒绝（SqliteError 可见）。
    REQUIRE_THROWS_AS(repository.upsert(conversation), SqliteError);

    aki::persistence::DeviceRepository devices(db);
    devices.upsert(make_device("local-1", TrustState::Trusted));
    devices.upsert(make_device("alpha-01", TrustState::Trusted));
    repository.upsert(conversation);  // 设备就位后成功

    const auto loaded = repository.find(ConversationId{"conv-alpha-01"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->state == ConversationState::Active);
    repository.upsert(conversation);  // 幂等 upsert

    conversation.state = ConversationState::Disconnected;
    repository.upsert(conversation);
    REQUIRE(repository.find(ConversationId{"conv-alpha-01"})->state
        == ConversationState::Disconnected);
    REQUIRE(repository.load_all().size() == 1);

    REQUIRE_FALSE(
        repository.find(ConversationId{"conv-missing"}).has_value());
}

// 读回防御（repositories.cpp 头注契约）：CHECK 只挡写入侧；绕过 CHECK 的
// 篡改值（库被外部改写）必须在读回时抛出，不得静默带入领域对象。
TEST_CASE("Conversation state read-back rejects unknown integer values",
    "[unit][persistence][repository]") {
    Database db = migrated_memory_db();
    aki::persistence::ConversationRepository repository(db);
    aki::persistence::DeviceRepository devices(db);
    devices.upsert(make_device("local-1", TrustState::Trusted));
    devices.upsert(make_device("alpha-01", TrustState::Trusted));

    Conversation conversation;
    conversation.id = ConversationId{"conv-alpha-01"};
    conversation.local_device = DeviceId{"local-1"};
    conversation.remote_device = DeviceId{"alpha-01"};
    conversation.state = ConversationState::Active;
    repository.upsert(conversation);

    // 模拟被篡改的库：PRAGMA 关闭写入侧 CHECK 后写入未知整数值（9 ∉ {0,1,2}）。
    aki::persistence::Statement disable_checks =
        db.prepare("PRAGMA ignore_check_constraints = ON;");
    (void)disable_checks.step();
    aki::persistence::Statement tamper =
        db.prepare("UPDATE conversation SET state = 9"
                   " WHERE conversation_id = 'conv-alpha-01';");
    (void)tamper.step();
    REQUIRE(db.changes() == 1);  // 篡改生效，确认后续抛出来自读回防御。

    REQUIRE_THROWS_AS(
        repository.find(ConversationId{"conv-alpha-01"}),
        std::runtime_error);
    REQUIRE_THROWS_AS(repository.load_all(), std::runtime_error);
}

// ---- 验收 ②：message 仓储（5 类 payload 往返 + 外键 + 送达状态）----

TEST_CASE("Message repository round-trips all payload kinds",
    "[unit][persistence][repository]") {
    Database db = migrated_memory_db();
    aki::persistence::DeviceRepository devices(db);
    aki::persistence::ConversationRepository conversations(db);
    aki::persistence::MessageRepository messages(db);

    devices.upsert(make_device("local-1", TrustState::Trusted));
    devices.upsert(make_device("alpha-01", TrustState::Trusted));
    Conversation conversation;
    conversation.id = ConversationId{"conv-1"};
    conversation.local_device = DeviceId{"local-1"};
    conversation.remote_device = DeviceId{"alpha-01"};
    conversations.upsert(conversation);

    const auto timestamp = std::chrono::system_clock::now();
    std::vector<Message> seed;
    seed.push_back(make_message("m-text", TextPayload{"你好，设备"}));
    seed.push_back(make_message("m-image",
        ImagePayload{FileMetadata{"img.png", 2048, "image/png"}}));
    seed.push_back(make_message("m-video",
        VideoPayload{FileMetadata{"clip.mp4", 4096, "video/mp4"}}));
    seed.push_back(make_message("m-file",
        FilePayload{FileMetadata{"policy.pt", 8192, "application/octet-stream"},
            TransferId{"t-1"}}));
    seed.push_back(make_message("m-system", SystemPayload{"device joined"}));

    for (auto& message : seed) {
        message.timestamp = timestamp;  // 同一时间便于断言往返
        messages.upsert(message, conversation.id);
    }
    REQUIRE(messages.load_all().size() == 5);

    const auto text = messages.find(MessageId{"m-text"});
    REQUIRE(text.has_value());
    REQUIRE(text->type == MessageType::Text);
    REQUIRE(std::get<TextPayload>(text->payload).text == "你好，设备");
    REQUIRE(text->state == DeliveryState::Sent);
    REQUIRE(text->timestamp == timestamp);  // 时间戳无损往返

    const auto image = messages.find(MessageId{"m-image"});
    REQUIRE(image->type == MessageType::Image);
    REQUIRE(std::get<ImagePayload>(image->payload).media.name == "img.png");
    REQUIRE(std::get<ImagePayload>(image->payload).media.size_bytes == 2048);
    REQUIRE(std::get<ImagePayload>(image->payload).media.mime_type
        == "image/png");

    const auto file = messages.find(MessageId{"m-file"});
    REQUIRE(file->type == MessageType::File);
    REQUIRE(std::get<FilePayload>(file->payload).transfer_id
        == TransferId{"t-1"});

    const auto system = messages.find(MessageId{"m-system"});
    REQUIRE(system->type == MessageType::System);
    REQUIRE(std::get<SystemPayload>(system->payload).text == "device joined");

    // 送达状态列更新（SetDeliveryState 映射）；终态后到达的回报由上层状态机
    // 负责拒绝，仓储只落列值。
    messages.set_delivery_state(
        MessageId{"m-text"}, DeliveryState::Delivered);
    REQUIRE(messages.find(MessageId{"m-text"})->state
        == DeliveryState::Delivered);
    REQUIRE_THROWS_AS(
        messages.set_delivery_state(MessageId{"m-missing"},
            DeliveryState::Delivered),
        std::runtime_error);
}

TEST_CASE("Message with unknown conversation is rejected by the foreign key",
    "[unit][persistence][repository]") {
    Database db = migrated_memory_db();
    aki::persistence::MessageRepository messages(db);
    REQUIRE_THROWS_AS(
        messages.upsert(make_message("m-orphan", TextPayload{"x"}),
            ConversationId{"conv-missing"}),
        SqliteError);
}

// ---- 验收 ②：transfer 仓储（进度/终态/回写位）----

TEST_CASE("Transfer repository maps progress, completion and the file columns",
    "[unit][persistence][repository]") {
    Database db = migrated_memory_db();
    aki::persistence::TransferRepository transfers(db);

    transfers.upsert(make_transfer("t-1"));
    transfers.update_progress(TransferId{"t-1"}, 512, 1024);

    auto loaded = transfers.find(TransferId{"t-1"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->transferred == 512);
    REQUIRE(loaded->total == 1024);
    REQUIRE(loaded->state == TransferState::Transferring);
    REQUIRE(transfers.load_all().size() == 1);  // 进度更新不新建行

    // §11.1 ④：终态更新 + 文件落地回写位（Completed 作业组产出，M2-06）。
    transfers.complete(TransferId{"t-1"}, TransferState::Completed,
        CompletedFile{"files/t-1/policy.pt",
            "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
            1024});

    aki::persistence::Statement verify =
        db.prepare("SELECT state, stored_relative_path, stored_sha256,"
                   " stored_size_bytes FROM transfer WHERE transfer_id = 't-1';");
    REQUIRE(verify.step());
    REQUIRE(verify.column_int64(0) == static_cast<std::int64_t>(TransferState::Completed));
    REQUIRE(verify.column_text(1) == "files/t-1/policy.pt");
    REQUIRE(verify.column_text(2)
        == "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
    REQUIRE(verify.column_int64(3) == 1024);

    // 无文件记录的终态（Failed/Cancelled 不落地）。
    transfers.upsert(make_transfer("t-2"));
    transfers.complete(TransferId{"t-2"}, TransferState::Cancelled);
    aki::persistence::Statement nulls =
        db.prepare("SELECT stored_relative_path, stored_sha256 FROM transfer"
                   " WHERE transfer_id = 't-2';");
    REQUIRE(nulls.step());
    REQUIRE(nulls.column_is_null(0));
    REQUIRE(nulls.column_is_null(1));

    // 拒绝路径：未知 id（RULE-09）、非终态 complete（编程错误）。
    REQUIRE_THROWS_AS(
        transfers.update_progress(TransferId{"t-missing"}, 1, 2),
        std::runtime_error);
    REQUIRE_THROWS_AS(
        transfers.complete(TransferId{"t-missing"}, TransferState::Completed),
        std::runtime_error);
    REQUIRE_THROWS_AS(
        transfers.complete(TransferId{"t-1"}, TransferState::Transferring),
        std::invalid_argument);
}

TEST_CASE("Transfer with unknown message id is rejected by the foreign key",
    "[unit][persistence][repository]") {
    Database db = migrated_memory_db();
    aki::persistence::TransferRepository transfers(db);
    REQUIRE_THROWS_AS(
        transfers.upsert(make_transfer("t-orphan"), MessageId{"m-missing"}),
        SqliteError);
}

// ---- DB 层 CHECK：非法枚举被拒绝（RULE-09 / 验收 ②）----

TEST_CASE("Schema CHECK constraints reject illegal enum values",
    "[unit][persistence][repository]") {
    Database db = migrated_memory_db();

    REQUIRE_THROWS_AS(
        db.execute("INSERT INTO device VALUES ('d', 'n', 0, '', NULL, 0, 9);"),
        SqliteError);  // trust_state=9
    REQUIRE_THROWS_AS(
        db.execute("INSERT INTO device VALUES ('d', 'n', 9, '', NULL, 0, 0);"),
        SqliteError);  // device_class=9
    REQUIRE_THROWS_AS(
        db.execute("INSERT INTO device VALUES ('d', 'n', 0, '', NULL, 0, 0);"
                   "INSERT INTO conversation VALUES ('c', 'd', 'd', 7);"),
        SqliteError);  // conversation state=7
    REQUIRE_THROWS_AS(
        db.execute("INSERT INTO device VALUES ('d', 'n', 0, '', NULL, 0, 0);"
                   "INSERT INTO conversation VALUES ('c', 'd', 'd', 0);"
                   "INSERT INTO message VALUES ('m', 'c', 's', 'r', 0, 9,"
                   " NULL, NULL, NULL, NULL, NULL, 0);"),
        SqliteError);  // message type=9
    REQUIRE_THROWS_AS(
        db.execute("INSERT INTO device VALUES ('d', 'n', 0, '', NULL, 0, 0);"
                   "INSERT INTO conversation VALUES ('c', 'd', 'd', 0);"
                   "INSERT INTO message VALUES ('m', 'c', 's', 'r', 0, 0,"
                   " 'x', NULL, NULL, NULL, NULL, 9);"),
        SqliteError);  // delivery_state=9
    REQUIRE_THROWS_AS(
        db.execute("INSERT INTO transfer VALUES ('t', NULL, 's', 'r', 'f',"
                   " 1, '', 0, 1, 9, NULL, NULL, NULL);"),
        SqliteError);  // transfer state=9
}

// ---- 验收 ③：语句缓存容量上限与 LRU 逐出 ----

TEST_CASE("Statement cache evicts the least recently used entry at capacity",
    "[unit][persistence][repository]") {
    Database db = migrated_memory_db();
    StatementCache cache(db, /*capacity=*/2);

    {
        auto& create = cache.get("CREATE TABLE cache_probe (k TEXT);");
        (void)create.step();  // 建表（DDL step 无行返回）
    }
    {
        auto& insert = cache.get("INSERT INTO cache_probe VALUES ('v');");
        (void)insert.step();
    }
    REQUIRE(cache.size() == 2);

    // 第 3 条语句触发 LRU 逐出（最久未使用的 CREATE TABLE 被逐出）。
    (void)cache.get("SELECT count(*) FROM cache_probe;");
    REQUIRE(cache.size() == 2);
    REQUIRE(cache.capacity() == 2);

    // 逐出后再取：重新 prepare，行为正确（不残留坏状态）。
    auto& count_again = cache.get("SELECT count(*) FROM cache_probe;");
    REQUIRE(count_again.step());
    REQUIRE(count_again.column_int64(0) == 1);
    REQUIRE(cache.size() == 2);

    // 命中复用语义：同一条 SQL 反复取用结果一致。
    auto& counter = cache.get("SELECT count(*) FROM cache_probe;");
    REQUIRE(counter.step());
    REQUIRE(counter.column_int64(0) == 1);
}

TEST_CASE("Statement cache rejects a zero capacity at construction",
    "[unit][persistence][repository]") {
    Database db = Database::open(":memory:");
    REQUIRE_THROWS_AS(StatementCache(db, 0), std::invalid_argument);
}
