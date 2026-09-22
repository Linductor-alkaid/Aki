// 版本化迁移框架（DEC-004；设计第 11.1 节 ②；M2-03）。
//
// 纪律：
//   - 步骤版本号必须为从 1 起严格连续递增（乱序/缺步/重复/空步骤在构造期
//     以 std::invalid_argument 拒绝——编程错误，非运行期 DB 错误）。
//   - 每个待执行步骤在独立事务内应用（步骤 sql + 前进 user_version）；
//     任一失败回滚当前步骤，user_version 不前进（既有数据不破坏）。
//   - bring_up_to_date 幂等：已是最新版本时为 no-op 返回 0。
//   - 数据库 user_version 新于已知最新步骤 → 干净失败（防旧二进制打开
//     新版本库，设计第 11.1 节 ②）。
// v1 正式 schema（第 11 节 ER 4 表）由 M2-04 注册；本框架以合成步骤测试全部路径。
#pragma once

#include "persistence/database/database.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace aki::persistence {

struct MigrationStep {
    int version = 0;  // 应用后目标 user_version（必须 = 数组下标 + 1）
    std::string name;
    std::string sql;  // 在独立事务内执行（可多语句）
};

class Migrator {
public:
    explicit Migrator(std::vector<MigrationStep> steps);

    [[nodiscard]] const std::vector<MigrationStep>& steps() const noexcept;

    // 应用所有待执行迁移，返回应用的步骤数（幂等 no-op 返回 0）。
    // 失败抛 SqliteError（步骤 sql 错误）或 std::runtime_error（库版本新于
    // 已知最新步骤）。
    [[nodiscard]] std::size_t bring_up_to_date(Database& database) const;

private:
    std::vector<MigrationStep> steps_;
};

}  // namespace aki::persistence
