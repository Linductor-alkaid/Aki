// v1 正式 schema（设计第 11 节 ER 4 表 + DEC-004 列补齐；M2-04 注册为迁移框架
// 首个正式迁移，消费 M2-03 的 Migrator）。
//
// 枚举映射：领域枚举的底层 u8 值以 INTEGER + CHECK 存储，CHECK 显式列出全部
// 合法值——非法值被 DB 层拒绝并经 SqliteError 可见（RULE-09）：
//   TrustState      0=Unknown 1=Pending 2=Trusted 3=Rejected 4=Revoked
//   MessageType     0=Text 1=Image 2=Video 3=File 4=System
//   ConversationState 0=Active 1=Disconnected 2=Archived
//   DeliveryState   0=Queued 1=Sending 2=Sent 3=Delivered 4=Failed
//   TransferState   0=Queued 1=Negotiating 2=Transferring 3=Paused
//                   4=Completed 5=Failed 6=Cancelled
//   DeviceClass     0=Desktop 1=Tablet 2=Phone 3=Server 4=Robot 5=Other
//
// 关系（设计第 11 节 ER）：DEVICE 1-* CONVERSATION、CONVERSATION 1-* MESSAGE、
// MESSAGE 0..1 TRANSFER（TRANSFER.message_id 可空——M1 宿主直发传输可不经
// 消息行；外键由 open 处 foreign_keys=ON 强制，孤儿行被 DB 拒绝）。
//
// 列补齐（DEC-004 / 设计第 11.1 节 ①④）：transfer 含远端原始文件名
// file_name（仅展示用，禁止拼路径）、POSIX 相对路径 stored_relative_path、
// stored_sha256、stored_size_bytes（Completed 终态作业组的回写位，M2-06）。
// device 不存 presence（易失状态，恢复后默认 Offline，第 11.1 节 ①）。
#pragma once

#include "persistence/migration/migration.hpp"

#include <vector>

namespace aki::persistence {

// 返回 v1 迁移步骤（单步、原子），供宿主注册进 Migrator。
[[nodiscard]] std::vector<MigrationStep> schema_v1_steps();

}  // namespace aki::persistence
