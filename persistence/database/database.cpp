// persistence 薄 RAII 封装实现（M2-03）。sqlite3* 仅出现在本编译单元（RULE-10）。
#include "persistence/database/database.hpp"

#include <sqlite3.h>

#include <utility>

namespace aki::persistence {
namespace {

// 统一把 sqlite3 返回值转成 SqliteError（携带 sqlite3_errmsg 语义）。
[[noreturn]] void throw_sqlite_error(int code, sqlite3* db) {
    const char* message = db != nullptr ? sqlite3_errmsg(db) : "unknown sqlite error";
    throw SqliteError(code, message != nullptr ? message : "unknown sqlite error");
}

void check_rc(int rc, sqlite3* db) {
    if (rc != SQLITE_OK) {
        throw_sqlite_error(rc, db);
    }
}

constexpr int kFirstBindIndex = 1;  // SQLite 绑定/列索引为 1-based。

}  // namespace

// ---- SqliteError ----

SqliteError::SqliteError(int code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

// ---- Statement ----

struct Statement::Impl {
    sqlite3* db = nullptr;
    sqlite3_stmt* stmt = nullptr;
};

Statement::Statement(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

Statement::Statement(Statement&&) noexcept = default;
Statement& Statement::operator=(Statement&&) noexcept = default;

Statement::~Statement() {
    if (impl_ != nullptr && impl_->stmt != nullptr) {
        // 确定性 finalize（RAII 契约）；错误码无有意义的恢复路径，忽略。
        sqlite3_finalize(impl_->stmt);
        impl_->stmt = nullptr;
    }
}

Statement::operator bool() const noexcept { return impl_ != nullptr; }

void Statement::require_valid() const {
    if (impl_ == nullptr) {
        throw std::logic_error(
            "aki::persistence::Statement is empty (moved-from or default)");
    }
}

void Statement::bind(int index, int value) {
    require_valid();
    check_rc(sqlite3_bind_int64(impl_->stmt, index, value), impl_->db);
}

void Statement::bind(int index, std::int64_t value) {
    require_valid();
    check_rc(sqlite3_bind_int64(impl_->stmt, index, value), impl_->db);
}

void Statement::bind(int index, double value) {
    require_valid();
    check_rc(sqlite3_bind_double(impl_->stmt, index, value), impl_->db);
}

void Statement::bind(int index, const std::string& value) {
    require_valid();
    check_rc(sqlite3_bind_text(impl_->stmt, index, value.c_str(),
                 static_cast<int>(value.size()), SQLITE_TRANSIENT),
        impl_->db);
}

void Statement::bind(int index, std::span<const std::byte> value) {
    require_valid();
    const void* data = value.empty() ? nullptr : value.data();
    check_rc(sqlite3_bind_blob(impl_->stmt, index, data,
                 static_cast<int>(value.size()), SQLITE_TRANSIENT),
        impl_->db);
}

void Statement::bind_null(int index) {
    require_valid();
    check_rc(sqlite3_bind_null(impl_->stmt, index), impl_->db);
}

bool Statement::step() {
    require_valid();
    const int rc = sqlite3_step(impl_->stmt);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    throw_sqlite_error(rc, impl_->db);
}

void Statement::reset() {
    require_valid();
    check_rc(sqlite3_reset(impl_->stmt), impl_->db);
    check_rc(sqlite3_clear_bindings(impl_->stmt), impl_->db);
}

int Statement::column_count() const {
    require_valid();
    return sqlite3_column_count(impl_->stmt);
}

std::int64_t Statement::column_int64(int column) const {
    require_valid();
    return sqlite3_column_int64(impl_->stmt, column);
}

double Statement::column_double(int column) const {
    require_valid();
    return sqlite3_column_double(impl_->stmt, column);
}

std::string Statement::column_text(int column) const {
    require_valid();
    const auto* text = sqlite3_column_text(impl_->stmt, column);
    const int size = sqlite3_column_bytes(impl_->stmt, column);
    if (text == nullptr || size <= 0) {
        return {};
    }
    return {reinterpret_cast<const char*>(text), static_cast<std::size_t>(size)};
}

std::vector<std::byte> Statement::column_blob(int column) const {
    require_valid();
    const auto* data = sqlite3_column_blob(impl_->stmt, column);
    const int size = sqlite3_column_bytes(impl_->stmt, column);
    if (data == nullptr || size <= 0) {
        return {};
    }
    const auto* bytes = static_cast<const std::byte*>(data);
    return {bytes, bytes + static_cast<std::size_t>(size)};
}

bool Statement::column_is_null(int column) const {
    require_valid();
    return sqlite3_column_type(impl_->stmt, column) == SQLITE_NULL;
}

// ---- Database ----

struct Database::Impl {
    sqlite3* db = nullptr;
};

Database::Database(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

Database::Database(Database&&) noexcept = default;
Database& Database::operator=(Database&&) noexcept = default;

Database::~Database() {
    if (impl_ != nullptr && impl_->db != nullptr) {
        // 兜底路径（close 未显式调用）：close_v2 在仍有未 finalize 语句时延迟
        // 释放，不产生 UB；正常路径走 close()（严格语义，暴露编程错误）。
        sqlite3_close_v2(impl_->db);
        impl_->db = nullptr;
    }
}

Database Database::open(const std::string& path, Options options) {
    sqlite3* raw = nullptr;
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    const int rc = sqlite3_open_v2(path.c_str(), &raw, flags, nullptr);
    if (rc != SQLITE_OK) {
        SqliteError error(rc, raw != nullptr
                ? sqlite3_errmsg(raw)
                : "cannot open database: " + path);
        if (raw != nullptr) {
            sqlite3_close(raw);
        }
        throw error;
    }

    Database db{std::unique_ptr<Impl>{new Impl{raw}}};
    // open 期 pragma（DEC-004 清单；默认值见 Options）。任一失败（含损坏 DB
    // 的 SQLITE_NOTADB）→ 异常展开时析构关闭连接，不静默（设计第 11.1 节 ②）。
    db.execute("PRAGMA journal_mode = WAL;");
    db.execute("PRAGMA synchronous = NORMAL;");
    db.execute("PRAGMA foreign_keys = ON;");
    db.execute("PRAGMA busy_timeout = "
        + std::to_string(options.busy_timeout.count()) + ";");
    return db;
}

bool Database::is_open() const noexcept { return impl_ != nullptr; }

void Database::close() {
    if (impl_ == nullptr || impl_->db == nullptr) {
        return;  // 幂等（头文件契约）：已关闭/空壳为 no-op。
    }
    const int rc = sqlite3_close(impl_->db);
    if (rc != SQLITE_OK) {
        // SQLITE_BUSY：仍有未 finalize 的 Statement——生命周期契约被破坏，
        // 显式暴露编程错误（不静默吞掉；连接暂未关闭）。
        throw std::logic_error(
            "aki::persistence::Database::close() called while statements are "
            "still alive; destroy all Statement objects before close()");
    }
    impl_->db = nullptr;
    impl_.reset();
}

void Database::require_open() const {
    if (impl_ == nullptr) {
        throw std::logic_error(
            "aki::persistence::Database is empty (moved-from, closed or "
            "default-constructed)");
    }
}

Statement Database::prepare(const std::string& sql) {
    require_open();
    sqlite3_stmt* stmt = nullptr;
    check_rc(sqlite3_prepare_v2(impl_->db, sql.c_str(),
                 static_cast<int>(sql.size()), &stmt, nullptr),
        impl_->db);
    return Statement{std::unique_ptr<Statement::Impl>{new Statement::Impl{impl_->db, stmt}}};
}

void Database::execute(const std::string& sql) {
    require_open();
    char* errmsg = nullptr;
    const int rc = sqlite3_exec(impl_->db, sql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        SqliteError error(rc, errmsg != nullptr
                ? std::string(errmsg)
                : std::string(sqlite3_errmsg(impl_->db)));
        sqlite3_free(errmsg);
        throw error;
    }
}

std::int64_t Database::last_insert_rowid() const {
    require_open();
    return sqlite3_last_insert_rowid(impl_->db);
}

int Database::changes() const {
    require_open();
    return sqlite3_changes(impl_->db);
}

// ---- Transaction ----

Transaction::Transaction(Database& database)
    : database_(&database), state_(State::active) {
    database_->execute("BEGIN;");
}

Transaction::~Transaction() noexcept {
    if (state_ == State::active) {
        // 异常路径自动回滚；析构不得抛出——回滚失败只能吞掉（理论上来不到：
        // 连接仍有效时 ROLLBACK 不失败）。
        try {
            database_->execute("ROLLBACK;");
        } catch (...) {
        }
    }
}

void Transaction::commit() {
    database_->execute("COMMIT;");
    state_ = State::committed;
}

void Transaction::rollback() {
    database_->execute("ROLLBACK;");
    state_ = State::rolled_back;
}

}  // namespace aki::persistence
