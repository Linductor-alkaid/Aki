// 版本化迁移框架实现（M2-03）。
#include "persistence/migration/migration.hpp"

#include <stdexcept>
#include <utility>

namespace aki::persistence {
namespace {

int read_user_version(Database& database) {
    Statement statement = database.prepare("PRAGMA user_version;");
    if (!statement.step()) {
        throw SqliteError(-1, "PRAGMA user_version returned no row");
    }
    return static_cast<int>(statement.column_int64(0));
}

}  // namespace

Migrator::Migrator(std::vector<MigrationStep> steps)
    : steps_(std::move(steps)) {
    for (std::size_t i = 0; i < steps_.size(); ++i) {
        const MigrationStep& step = steps_[i];
        if (step.version != static_cast<int>(i) + 1) {
            throw std::invalid_argument(
                "migration steps must be consecutive ascending from 1: step at "
                "index " + std::to_string(i) + " has version "
                + std::to_string(step.version));
        }
        if (step.name.empty() || step.sql.empty()) {
            throw std::invalid_argument(
                "migration step " + std::to_string(step.version)
                + " has an empty name or sql");
        }
    }
}

const std::vector<MigrationStep>& Migrator::steps() const noexcept {
    return steps_;
}

std::size_t Migrator::bring_up_to_date(Database& database) const {
    const int current = read_user_version(database);
    const int latest = steps_.empty() ? 0 : steps_.back().version;
    if (current > latest) {
        throw std::runtime_error(
            "database schema version " + std::to_string(current)
            + " is newer than the supported version " + std::to_string(latest)
            + " (refusing to open with an older binary)");
    }

    std::size_t applied = 0;
    for (const MigrationStep& step : steps_) {
        if (step.version <= current) {
            continue;  // 已应用：幂等跳过。
        }
        // 每步独立事务：步骤 sql 与 user_version 前进同生共死（PRAGMA
        // user_version 是库头写，随事务回滚）。
        Transaction transaction(database);
        database.execute(step.sql);
        database.execute(
            "PRAGMA user_version = " + std::to_string(step.version) + ";");
        transaction.commit();
        ++applied;
    }
    return applied;
}

}  // namespace aki::persistence
