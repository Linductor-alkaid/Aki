// 启动恢复组合（设计第 11.1 节 ②；M2-07）。
//
// 组合根在 ExecutorOwner.initialize() 之后、Manager/Adapter 启动之前，于主线程
// 同步执行本组件——此时尚无并发事件源，不经 blocking worker，DatabaseWorker 在
// 恢复完成后才注册（EXEC-02 启动段纪律）：
//   解析数据根（组合根注入路径字符串，§11.1 ④）→ open（DB 损坏即 SqliteError
//   干净失败，不静默）→ PRAGMA user_version 迁移 → 四仓储 load_all() 逐域加载
//   DEVICE/CONVERSATION/MESSAGE/TRANSFER → sweep_tmp_orphans 按加载到的活动
//   Transfer 行清扫 files/tmp/ 残留（DEC-004 崩溃恢复纪律）。
//
// 产出：加载结果供宿主以 AppStateOwner 构造入参播种初始快照（恢复期 owner 尚未
// 运行、无并发写者，恢复数据即首个权威快照）。
//
// 单一连接纪律（§11.1 ③）：Repositories 锚定唯一 Database，恢复期由主线程独占
// 使用；恢复完成后整体移交 DatabaseWorker（同一实例）——连接不并发。FileStore
// 以 shared_ptr 返回，供终态文件作业组捕获（M2-06 契约）。
//
// RULE-10：公开面仅领域类型与 std 类型；本组件为同步组合，不建线程、不依赖
// executor（RULE-07）。
#pragma once

#include "persistence/database/database_worker.hpp"
#include "persistence/storage/file_store.hpp"

#include <memory>
#include <string>
#include <vector>

namespace aki::persistence {

// 恢复诊断（宿主输出与测试断言用；RULE-09：清扫/迁移结果可见）。
struct RecoveryDiagnostics {
    std::string data_root;
    std::string db_path;
    std::size_t migrations_applied = 0;   // 本次恢复实际应用的迁移步数（幂等重开为 0）
    std::size_t tmp_orphans_removed = 0;  // files/tmp/ 残留清扫数
};

// 逐域加载结果（领域类型；宿主据此构造 AppState 播种）。
struct RecoveredData {
    std::vector<aki::device::DeviceIdentity> devices;              // presence 恢复为 Offline（易失）
    std::vector<aki::conversation::Conversation> conversations;
    std::vector<aki::conversation::Message> messages;
    std::vector<aki::transfer::Transfer> transfers;
};

struct RecoveryResult {
    std::unique_ptr<Repositories> repositories;  // 单一连接，移交 DatabaseWorker
    std::shared_ptr<FileStore> store;            // 文件本体（终态作业共享捕获）
    RecoveredData state;                         // 播种数据
    RecoveryDiagnostics diagnostics;
};

// DEC-004 布局：<data_root>/db/aki.db3 与 files/ 并置。
inline constexpr const char* kDatabaseFileName = "aki.db3";

// 主线程同步执行启动恢复。失败抛 SqliteError（open/迁移/加载）或
// std::runtime_error / std::invalid_argument（目录创建、参数）——组合根捕获后
// 输出原因并退出（§11.1 ② 干净失败，不静默）。
[[nodiscard]] RecoveryResult perform_startup_recovery(const std::string& data_root,
    const DatabaseWorkerOptions& options = {});

}  // namespace aki::persistence
