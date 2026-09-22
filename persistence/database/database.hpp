// persistence 薄 RAII 封装（DEC-004；设计第 11.1 节 ②；M2-03）。
//
// RULE-10 边界：本公开头文件不出现任何 sqlite3 类型，也不包含 <sqlite3.h>——
// sqlite3* 只存在于 persistence/database/database.cpp 编译单元内（pimpl），
// aki_persistence 以 PRIVATE 链接 sqlite3（消费者拿不到 sqlite include 路径）。
// 公开 API 只暴露 std 标准类型（std::string / std::int64_t / std::byte span）。
//
// 并发定位：同步封装，内部无锁、不建线程、不依赖 executor（RULE-07）——
// 启动恢复段由主线程直用（设计第 11.1 节 ②），运行期由 DatabaseWorker
// （blocking worker，M2-05）在单一 worker 内串行消费。
//
// 生命周期契约：
//   - Statement/Transaction 不得越过其 Database（close 前必须已析构全部
//     Statement；违者 Database::close() 抛 std::logic_error 显式暴露，
//     析构路径经 sqlite3_close_v2 兜底不产生 UB）。
//   - move 后的 Database/Statement 为空壳：is_open()/operator bool() 为假，
//     其余方法抛 std::logic_error。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace aki::persistence {

// SQLite 错误（携带主错误码 + sqlite3_errmsg 语义，标准类型，RULE-10）。
class SqliteError : public std::runtime_error {
public:
    SqliteError(int code, const std::string& message);
    [[nodiscard]] int code() const noexcept { return code_; }

private:
    int code_;
};

// 预编译 SQL 语句：prepare/bind/step/列提取 RAII，析构确定性 finalize。
// 绑定与列索引均为 1-based（SQLite 契约）。move 后为空壳。
class Statement {
public:
    Statement(Statement&&) noexcept;
    Statement& operator=(Statement&&) noexcept;
    ~Statement();

    explicit operator bool() const noexcept;

    void bind(int index, int value);  // 代理到 int64，避免 int64/double 二义
    void bind(int index, std::int64_t value);
    void bind(int index, double value);
    void bind(int index, const std::string& value);           // TEXT
    void bind(int index, std::span<const std::byte> value);   // BLOB
    void bind_null(int index);

    // true = 有可用行；false = 完成（DONE）；错误抛 SqliteError。
    [[nodiscard]] bool step();

    void reset();  // reset + clear bindings

    [[nodiscard]] int column_count() const;
    [[nodiscard]] std::int64_t column_int64(int column) const;
    [[nodiscard]] double column_double(int column) const;
    [[nodiscard]] std::string column_text(int column) const;  // NULL → 空串
    [[nodiscard]] std::vector<std::byte> column_blob(int column) const;
    [[nodiscard]] bool column_is_null(int column) const;

    struct Impl;

private:
    friend class Database;
    explicit Statement(std::unique_ptr<Impl> impl) noexcept;
    void require_valid() const;

    std::unique_ptr<Impl> impl_;
};

// 连接 RAII：open 成功处统一设置 open 期 pragma（DEC-004 清单，默认值本项确定）：
//   journal_mode=WAL、synchronous=NORMAL、foreign_keys=ON、
//   busy_timeout=Options::busy_timeout（默认 5000ms）。
// 任一 pragma 失败（含损坏 DB 文件的 SQLITE_NOTADB）→ 关闭并抛 SqliteError，
// 不静默（设计第 11.1 节 ②）。
// 构造选项置于命名空间作用域（GCC 对嵌套 Options 默认实参 `= {}` 的已知限制，
// 同 AppStateOwnerOptions 处理）。
struct DatabaseOptions {
    std::chrono::milliseconds busy_timeout{5000};
};

class Database {
public:
    using Options = DatabaseOptions;

    Database() = default;
    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;
    ~Database();

    // 打开（READWRITE|CREATE）并完成 open 期 pragma；失败抛 SqliteError。
    [[nodiscard]] static Database open(const std::string& path,
        DatabaseOptions options = {});

    [[nodiscard]] bool is_open() const noexcept;
    // 幂等；调用前必须已析构全部 Statement（否则抛 std::logic_error）。
    void close();

    // 预编译单条语句；prepare 失败（语法/缺表）抛 SqliteError。
    [[nodiscard]] Statement prepare(const std::string& sql);
    // 执行多语句脚本（无结果行，如迁移 DDL/PRAGMA）；失败抛 SqliteError。
    void execute(const std::string& sql);

    [[nodiscard]] std::int64_t last_insert_rowid() const;
    // 最近一条语句改变的行数（UPDATE ... WHERE 未命中检测，RULE-09）。
    [[nodiscard]] int changes() const;

    struct Impl;

private:
    friend class Transaction;
    explicit Database(std::unique_ptr<Impl> impl) noexcept;
    void require_open() const;

    std::unique_ptr<Impl> impl_;
};

// 事务守卫：构造 BEGIN，commit()/rollback() 显式结束；析构时仍未结束
// （含异常路径）自动 ROLLBACK。非拷贝非移动（守卫语义）。
class Transaction {
public:
    explicit Transaction(Database& database);
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction() noexcept;

    void commit();    // COMMIT；之后析构为 no-op；失败抛 SqliteError
    void rollback();  // ROLLBACK；之后析构为 no-op；失败抛 SqliteError

private:
    enum class State { active, committed, rolled_back };

    Database* database_;
    State state_;
};

}  // namespace aki::persistence
