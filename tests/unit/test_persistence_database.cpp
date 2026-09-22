// M2-03：persistence 薄 RAII 封装与迁移框架单测（DEC-004；设计第 11.1 节 ②）。
//
// 覆盖（验收 ①②）：
//   - 迁移框架全部路径：前进成功、失败回滚（user_version 不前进、既有数据
//     不破坏）、幂等重跑（no-op 返回 0）、乱序/缺步/重复/空步骤构造期拒绝、
//     部分前进（中间版本只补尾）、库版本新于已知步骤干净失败；
//   - RAII：open 损坏 DB 干净失败（SqliteError，不静默）、open 期 pragma 生效
//     （journal_mode/synchronous/foreign_keys/busy_timeout 回读）、语句错误可见
//     （prepare/step）、事务提交/显式回滚/异常自动回滚、blob/text/null 往返、
//     移动语义与空壳拒绝、close 生命周期契约（在飞语句显式暴露）。
//
// 说明：本套件只链接 aki_persistence（sqlite3 为 PRIVATE），不经由 sqlite3
// 头/常量断言（RULE-10 边界的消费者视角）；损坏 DB 断言错误语义字符串与
// 非零错误码。catch 捕获本进程无 executor/线程需求（RULE-07）。
#include "persistence/database/database.hpp"
#include "persistence/migration/migration.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

using aki::persistence::Database;
using aki::persistence::MigrationStep;
using aki::persistence::Migrator;
using aki::persistence::SqliteError;
using aki::persistence::Statement;
using aki::persistence::Transaction;

int g_counter = 0;

// 唯一临时 DB 路径（存在则先删，避免上一轮残留影响断言）。
std::string temp_db_path(const std::string& tag) {
    auto path = std::filesystem::temp_directory_path()
        / ("aki-persistence-test-" + tag + "-"
            + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())
            + "-" + std::to_string(++g_counter) + ".db3");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path.string();
}

int read_user_version(Database& db) {
    Statement statement = db.prepare("PRAGMA user_version;");
    REQUIRE(statement.step());
    return static_cast<int>(statement.column_int64(0));
}

bool table_exists(Database& db, const std::string& name) {
    Statement statement = db.prepare(
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name = ?1;");
    statement.bind(1, name);
    return statement.step();
}

}  // namespace

TEST_CASE("Open applies the DEC-004 pragma set and it reads back", "[unit][persistence]") {
    const auto path = temp_db_path("pragma");
    Database::Options options;
    options.busy_timeout = std::chrono::milliseconds{1234};
    Database db = Database::open(path, options);

    {
        Statement journal = db.prepare("PRAGMA journal_mode;");
        REQUIRE(journal.step());
        REQUIRE(journal.column_text(0) == "wal");  // 文件库才生效 WAL（内存库返回 memory）

        Statement synchronous = db.prepare("PRAGMA synchronous;");
        REQUIRE(synchronous.step());
        REQUIRE(synchronous.column_int64(0) == 1);  // NORMAL

        Statement foreign_keys = db.prepare("PRAGMA foreign_keys;");
        REQUIRE(foreign_keys.step());
        REQUIRE(foreign_keys.column_int64(0) == 1);  // ON

        Statement busy_timeout = db.prepare("PRAGMA busy_timeout;");
        REQUIRE(busy_timeout.step());
        REQUIRE(busy_timeout.column_int64(0) == 1234);
    }  // 语句析构（确定性 finalize）后再 close（生命周期契约）

    db.close();
    REQUIRE_FALSE(db.is_open());
    std::filesystem::remove(path);
}

TEST_CASE("Open on a corrupted database file fails cleanly (no silent fallback)",
    "[unit][persistence]") {
    const auto path = temp_db_path("corrupt");
    {
        std::fstream garbage(path, std::ios::binary | std::ios::out);
        REQUIRE(garbage.is_open());
        // >512 字节非 SQLite 头，避免命中“空文件=合法新库”的语义。
        const std::string junk(600, 'x');
        garbage << "this is definitely not a sqlite database\n" << junk;
    }
    REQUIRE_THROWS_AS(Database::open(path), SqliteError);

    bool message_visible = false;
    try {
        (void)Database::open(path);
    } catch (const SqliteError& error) {
        message_visible = error.code() != 0
            && std::string(error.what()).find("not a database") != std::string::npos;
    }
    REQUIRE(message_visible);  // sqlite3_errmsg 语义可见（RULE-09，不静默）
    std::filesystem::remove(path);
}

TEST_CASE("Statement prepare and step surface errors visibly",
    "[unit][persistence]") {
    Database db = Database::open(temp_db_path("stmt-errors"));
    db.execute("CREATE TABLE t (a INTEGER PRIMARY KEY);");

    // prepare 期错误：语法错误与缺表（prepare_v2 即时解析）。
    REQUIRE_THROWS_AS(db.prepare("SELEC 1;"), SqliteError);
    REQUIRE_THROWS_AS(
        db.prepare("INSERT INTO missing_table VALUES (1);"), SqliteError);

    Statement insert = db.prepare("INSERT INTO t (a) VALUES (?1);");
    insert.bind(1, std::int64_t{42});
    REQUIRE_FALSE(insert.step());  // 完成，无行

    Statement select = db.prepare("SELECT a FROM t;");
    REQUIRE(select.step());
    REQUIRE(select.column_int64(0) == 42);

    // step 期错误：主键冲突（约束检查发生在 step），errmsg 语义可见。
    Statement duplicate = db.prepare("INSERT INTO t (a) VALUES (?1);");
    duplicate.bind(1, std::int64_t{42});
    REQUIRE_THROWS_AS(duplicate.step(), SqliteError);
    try {
        (void)duplicate.step();
    } catch (const SqliteError& error) {
        REQUIRE(error.code() != 0);
        REQUIRE(std::string(error.what()).find("UNIQUE") != std::string::npos);
    }
}

TEST_CASE("Transactions commit, roll back, and auto-roll-back on exceptions",
    "[unit][persistence]") {
    Database db = Database::open(temp_db_path("tx"));
    db.execute("CREATE TABLE t (a INTEGER);");

    SECTION("commit persists") {
        {
            Transaction tx(db);
            db.execute("INSERT INTO t VALUES (1);");
            tx.commit();
        }
        Statement select = db.prepare("SELECT count(*) FROM t;");
        REQUIRE(select.step());
        REQUIRE(select.column_int64(0) == 1);
    }

    SECTION("explicit rollback discards") {
        {
            Transaction tx(db);
            db.execute("INSERT INTO t VALUES (1);");
            tx.rollback();
        }
        Statement select = db.prepare("SELECT count(*) FROM t;");
        REQUIRE(select.step());
        REQUIRE(select.column_int64(0) == 0);
    }

    SECTION("exception path auto-rolls back via the guard destructor") {
        try {
            Transaction tx(db);
            db.execute("INSERT INTO t VALUES (1);");
            throw std::runtime_error("business failure");
        } catch (const std::runtime_error&) {
        }
        Statement select = db.prepare("SELECT count(*) FROM t;");
        REQUIRE(select.step());
        REQUIRE(select.column_int64(0) == 0);
    }
}

TEST_CASE("Blob, text and null columns round-trip", "[unit][persistence]") {
    Database db = Database::open(temp_db_path("columns"));
    db.execute("CREATE TABLE t (data BLOB, name TEXT, score INTEGER);");

    const std::vector<std::byte> blob{
        std::byte{1}, std::byte{2}, std::byte{3}};
    Statement insert = db.prepare("INSERT INTO t (data, name, score) VALUES (?1, ?2, ?3);");
    insert.bind(1, blob);
    insert.bind(2, std::string{"原文件名.txt"});
    insert.bind(3, std::int64_t{-7});
    REQUIRE_FALSE(insert.step());

    Statement insert_null = db.prepare(
        "INSERT INTO t (data, name, score) VALUES (?1, ?2, ?3);");
    insert_null.bind(1, std::span<const std::byte>{});
    insert_null.bind_null(2);
    insert_null.bind_null(3);
    REQUIRE_FALSE(insert_null.step());

    Statement select = db.prepare("SELECT data, name, score FROM t ORDER BY rowid;");
    REQUIRE(select.step());
    REQUIRE(select.column_blob(0) == blob);
    REQUIRE(select.column_text(1) == "原文件名.txt");
    REQUIRE(select.column_int64(2) == -7);
    REQUIRE_FALSE(select.column_is_null(0));

    REQUIRE(select.step());
    REQUIRE(select.column_blob(0).empty());
    REQUIRE(select.column_is_null(1));
    REQUIRE(select.column_text(1).empty());  // NULL → 空串
    REQUIRE(select.column_is_null(2));
    REQUIRE_FALSE(select.step());
}

TEST_CASE("Move semantics transfer ownership and empty handles are rejected",
    "[unit][persistence]") {
    const auto path = temp_db_path("move");
    Database source = Database::open(path);
    Database target = std::move(source);
    REQUIRE_FALSE(source.is_open());
    REQUIRE(target.is_open());

    // 空壳 Database：is_open 为假，使用抛 std::logic_error（不静默）。
    REQUIRE_THROWS_AS(source.prepare("SELECT 1;"), std::logic_error);
    REQUIRE_THROWS_AS(source.execute("SELECT 1;"), std::logic_error);
    REQUIRE_THROWS_AS(source.last_insert_rowid(), std::logic_error);

    // 空壳 Statement：同样显式拒绝。
    Statement statement = target.prepare("SELECT 1;");
    Statement moved = std::move(statement);
    REQUIRE_FALSE(static_cast<bool>(statement));
    REQUIRE_THROWS_AS(statement.step(), std::logic_error);
    REQUIRE_THROWS_AS(statement.bind(1, std::int64_t{1}), std::logic_error);
    REQUIRE(moved.step());  // move 后目标仍可用
    REQUIRE(moved.column_int64(0) == 1);
}

TEST_CASE("close rejects a database with live statements, then succeeds",
    "[unit][persistence]") {
    Database db = Database::open(temp_db_path("close"));
    {
        Statement live = db.prepare("PRAGMA user_version;");
        REQUIRE_THROWS_AS(db.close(), std::logic_error);  // 在飞语句显式暴露
        REQUIRE(db.is_open());
    }  // 语句析构 → 确定性 finalize
    db.close();
    REQUIRE_FALSE(db.is_open());
    db.close();  // 幂等
}

// ---- 迁移框架（验收 ①）----

TEST_CASE("Migrations apply in order, are idempotent, and record user_version",
    "[unit][persistence][migration]") {
    Database db = Database::open(":memory:");
    Migrator migrator({MigrationStep{1, "devices",
                          "CREATE TABLE devices ("
                          "device_id TEXT PRIMARY KEY, trust_state INTEGER NOT NULL);"},
        MigrationStep{2, "messages",
                          "CREATE TABLE messages ("
                          "message_id TEXT PRIMARY KEY, body TEXT NOT NULL);"
                          "INSERT INTO messages VALUES ('m-1', 'seed');"}});

    REQUIRE(migrator.bring_up_to_date(db) == 2);
    REQUIRE(read_user_version(db) == 2);
    REQUIRE(table_exists(db, "devices"));
    REQUIRE(table_exists(db, "messages"));

    // 幂等重跑：no-op 返回 0，user_version 不变。
    REQUIRE(migrator.bring_up_to_date(db) == 0);
    REQUIRE(read_user_version(db) == 2);

    // 种子行随 v2 一起提交（步骤内 sql 与 user_version 同事务）。
    Statement seed = db.prepare("SELECT count(*) FROM messages;");
    REQUIRE(seed.step());
    REQUIRE(seed.column_int64(0) == 1);
}

TEST_CASE("A failing migration rolls back and keeps user_version unchanged",
    "[unit][persistence][migration]") {
    Database db = Database::open(":memory:");
    Migrator migrator({MigrationStep{1, "ok", "CREATE TABLE t1 (a INTEGER);"},
        MigrationStep{2, "broken",
            "CREATE TABLE t2 (a INTEGER); THIS IS NOT VALID SQL;"}});
    REQUIRE_THROWS_AS(migrator.bring_up_to_date(db), SqliteError);

    REQUIRE(read_user_version(db) == 1);       // user_version 不前进
    REQUIRE(table_exists(db, "t1"));           // v1 已提交，不受影响
    REQUIRE_FALSE(table_exists(db, "t2"));     // v2 回滚干净

    // 修复后重跑：从 v1 续跑（部分前进）。
    Migrator fixed({MigrationStep{1, "ok", "CREATE TABLE t1 (a INTEGER);"},
        MigrationStep{2, "fixed", "CREATE TABLE t2 (a INTEGER);"}});
    REQUIRE(fixed.bring_up_to_date(db) == 1);
    REQUIRE(read_user_version(db) == 2);
    REQUIRE(table_exists(db, "t2"));
}

TEST_CASE("A database newer than known migrations fails cleanly",
    "[unit][persistence][migration]") {
    Database db = Database::open(":memory:");
    REQUIRE(Migrator({MigrationStep{1, "a", "CREATE TABLE a (x);"},
        MigrationStep{2, "b", "CREATE TABLE b (x);"}})
                .bring_up_to_date(db)
        == 2);

    Migrator older({MigrationStep{1, "a", "CREATE TABLE a (x);"}});
    REQUIRE_THROWS_AS(older.bring_up_to_date(db), std::runtime_error);
    REQUIRE(read_user_version(db) == 2);
}

TEST_CASE("Invalid migration lists are rejected at construction",
    "[unit][persistence][migration]") {
    auto make = [](std::vector<MigrationStep> steps) { return Migrator(std::move(steps)); };

    REQUIRE_THROWS_AS(
        make({MigrationStep{2, "out-of-order", "SELECT 1;"}}), std::invalid_argument);
    REQUIRE_THROWS_AS(
        make({MigrationStep{1, "a", "SELECT 1;"}, MigrationStep{1, "dup", "SELECT 2;"}}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        make({MigrationStep{1, "a", "SELECT 1;"}, MigrationStep{3, "gap", "SELECT 2;"}}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        make({MigrationStep{0, "zero", "SELECT 1;"}}), std::invalid_argument);
    REQUIRE_THROWS_AS(
        make({MigrationStep{1, "", "SELECT 1;"}}), std::invalid_argument);
    REQUIRE_THROWS_AS(
        make({MigrationStep{1, "no-sql", ""}}), std::invalid_argument);
}
