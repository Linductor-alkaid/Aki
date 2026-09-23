// 启动恢复组合实现（设计第 11.1 节 ②；M2-07）。
#include "persistence/recovery/startup_recovery.hpp"

#include "persistence/migration/migration.hpp"
#include "persistence/migration/schema_v1.hpp"

#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace aki::persistence {

RecoveryResult perform_startup_recovery(const std::string& data_root,
    const DatabaseWorkerOptions& options) {
    if (data_root.empty()) {
        throw std::invalid_argument(
            "startup recovery: data root must not be empty");
    }

    // 布局（DEC-004）：<root>/db/aki.db3 与 <root>/files/ 并置；目录创建幂等。
    const std::string db_dir = data_root + "/db";
    std::error_code ec;
    std::filesystem::create_directories(db_dir, ec);
    if (ec) {
        throw std::runtime_error("startup recovery: cannot create '" + db_dir
            + "': " + ec.message());
    }
    // FileStore 创建 files/ 与 files/tmp（失败抛 runtime_error，不静默）。
    auto store = std::make_shared<FileStore>(data_root);

    // open（损坏 DB → SqliteError 干净失败）→ user_version 迁移。
    const std::string db_path = db_dir + "/" + kDatabaseFileName;
    Database database = Database::open(db_path);
    RecoveryDiagnostics diagnostics;
    diagnostics.data_root = data_root;
    diagnostics.db_path = db_path;
    diagnostics.migrations_applied =
        Migrator(schema_v1_steps()).bring_up_to_date(database);

    // 单一连接移交：Repositories 锚定该 Database（恢复期主线程独占，之后由
    // DatabaseWorker 串行消费，§11.1 ③）。
    auto repositories = std::make_unique<Repositories>(std::move(database),
        options.repository_cache_capacity);
    auto& repos = *repositories;

    RecoveryResult result;
    result.repositories = std::move(repositories);
    result.store = std::move(store);
    result.state.devices = repos.devices.load_all();
    result.state.conversations = repos.conversations.load_all();
    result.state.messages = repos.messages.load_all();
    result.state.transfers = repos.transfers.load_all();

    // 清扫按加载到的活动 Transfer 行判定（§11.1 ②：加载之后执行）。
    result.diagnostics = std::move(diagnostics);
    result.diagnostics.tmp_orphans_removed =
        result.store->sweep_tmp_orphans(result.state.transfers);
    return result;
}

}  // namespace aki::persistence
