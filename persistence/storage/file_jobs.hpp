// 文件终态作业工厂（设计第 11.1 节 ④；M2-06）。
//
// 经 M2-05 DatabaseWorker 串行执行（EXEC-04）：Completed 作业组 =
// TRANSFER 终态更新 + 流式 SHA-256 + 原子改名 + 回写位；Failed/Cancelled =
// .part 幂等删除。作业捕获 shared_ptr<FileStore>（生命周期安全），仓储由
// 作业参数注入（worker 的 Repositories）。
//
// 用法（宿主/M2-07）：创建共享 FileStore（数据根注入），终态事件到达时以
// `control.enqueue(make_transfer_complete_job(store, id))` 入队。
#pragma once

#include "persistence/database/database_worker.hpp"
#include "persistence/storage/file_store.hpp"

#include <memory>
#include <string>
#include <utility>

namespace aki::persistence {

// Completed 终态作业组（幂等；.part 缺失时作业以 runtime_error 失败）。
[[nodiscard]] DbJob make_transfer_complete_job(
    std::shared_ptr<FileStore> store, std::string transfer_id);

// Failed / Cancelled：.part 幂等删除作业。
[[nodiscard]] DbJob make_transfer_discard_job(
    std::shared_ptr<FileStore> store, std::string transfer_id);

}  // namespace aki::persistence
