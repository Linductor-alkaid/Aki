// M2-03：RULE-10 公开边界锁定用例。
//
// 本消费编译单元只链接 aki_persistence（其 sqlite3 链接为 PRIVATE，不向消费者
// 传播 include 路径或库）：任一 persistence 公开头引入 <sqlite3.h> 或暴露
// sqlite3 类型，本目标立即编译失败。用例本身对公开 API 做一次最小真实使用
// （:memory: 库上迁移 + 写读），证明公开面自洽可用。
#include "persistence/database/database.hpp"
#include "persistence/migration/migration.hpp"

#include <cstdio>
#include <string>
#include <vector>

int main() {
    aki::persistence::Database db =
        aki::persistence::Database::open(":memory:");
    aki::persistence::Migrator migrator({aki::persistence::MigrationStep{
        1, "public-surface", "CREATE TABLE probe (k TEXT PRIMARY KEY, v TEXT);"}});
    if (migrator.bring_up_to_date(db) != 1) {
        std::puts("[FAIL] migration should apply one step");
        return 1;
    }

    aki::persistence::Statement insert =
        db.prepare("INSERT INTO probe (k, v) VALUES (?1, ?2);");
    insert.bind(1, std::string("k"));
    insert.bind(2, std::string("v"));
    if (insert.step()) {
        std::puts("[FAIL] insert should not return a row");
        return 1;
    }

    aki::persistence::Statement select =
        db.prepare("SELECT v FROM probe WHERE k = ?1;");
    select.bind(1, std::string("k"));
    if (!select.step() || select.column_text(0) != "v") {
        std::puts("[FAIL] round-trip mismatch");
        return 1;
    }

    std::puts("[ok] persistence public surface compiles and works without "
              "sqlite include path");
    return 0;
}
