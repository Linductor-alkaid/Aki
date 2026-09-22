// M2-03：RULE-10 公开边界锁定用例。
//
// 本消费编译单元只链接 aki_persistence（其 sqlite3 链接为 PRIVATE，不向消费者
// 传播 include 路径或库）：任一 persistence 公开头引入 <sqlite3.h> 或暴露
// sqlite3 类型，本目标立即编译失败。用例本身对公开 API 做一次最小真实使用
// （:memory: 库上迁移 + 写读），证明公开面自洽可用。
#include "device/device/device_types.hpp"
#include "persistence/database/database.hpp"
#include "persistence/migration/migration.hpp"
#include "persistence/migration/schema_v1.hpp"
#include "persistence/repository/repositories.hpp"

#include <cstdio>
#include <string>
#include <vector>

int main() {
    aki::persistence::Database db =
        aki::persistence::Database::open(":memory:");
    // 迁移：v1 正式 schema（ER 四表）+ 探针表（两步，验证多步迁移）。
    std::vector<aki::persistence::MigrationStep> steps =
        aki::persistence::schema_v1_steps();
    steps.push_back(aki::persistence::MigrationStep{
        2, "public-surface-probe",
        "CREATE TABLE probe (k TEXT PRIMARY KEY, v TEXT);"});
    aki::persistence::Migrator migrator(steps);
    if (migrator.bring_up_to_date(db) != 2) {
        std::puts("[FAIL] migration should apply two steps");
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

    // 仓储层公开面（M2-04）：领域类型 + std 类型即可完成一次真实使用。
    aki::persistence::DeviceRepository devices(db);
    aki::device::DeviceIdentity identity;
    identity.id = aki::device::DeviceId{"local-1"};
    identity.display_name = "local";
    identity.trust_state = aki::device::TrustState::Trusted;
    devices.upsert(identity);
    if (!devices.find(identity.id).has_value()) {
        std::puts("[FAIL] device round-trip");
        return 1;
    }

    std::puts("[ok] persistence public surface compiles and works without "
              "sqlite include path");
    return 0;
}
