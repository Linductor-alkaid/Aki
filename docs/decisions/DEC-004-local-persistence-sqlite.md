# DEC-004：本地持久化采用 SQLite

> 状态：Proposed
> 日期：2026-09-21
> 负责人：Linductor
> 冻结里程碑：M2 开始前
> 替代/被替代：无

## 背景与问题

设计第 11 节要求本地持久化保存设备身份、受信设备、Conversation metadata、消息历史、
Transfer history 与应用设置；大文件本体保存在文件系统，数据库记录 metadata。需要确定
存储引擎与访问方式。

## 决策（暂定默认值）

- 采用 SQLite 作为本地持久化存储，对应设计第 11 节的 ER 模型
  （DEVICE / CONVERSATION / MESSAGE / TRANSFER）。
- 访问方式暂定 SQLite C API + 薄封装（RAII 语句/事务守卫），不引入 ORM；数据库访问
  属于阻塞 I/O，运行期经 Executor blocking worker 承载（`EXEC-04`）。
- 文件本体（图片、视频、大文件）存文件系统，数据库只存路径、hash、大小、MIME type 等
  metadata；文件布局在 M2 详细设计时确定。
- schema 变更走 `persistence/migration` 版本化迁移，不直接改历史 schema。

## 备选方案

- 每设备 JSON/二进制文件：查询、迁移与并发写能力弱，无法支撑历史检索，否决。
- 嵌入式 KV 存储（LMDB 等）：消息关联查询需自建索引，收益不明确，暂不采用。

## 影响与风险

- SQLite 第三方引入方式（系统库 / 源码并入 / 包管理）随 `DEC-003` 的锁定机制一并确定。
- 数据库文件损坏与版本迁移路径需要在 M2 测试矩阵中覆盖。

## 验证方式

M2 退出条件：重启后恢复设备、会话、消息与传输历史；迁移与损坏恢复测试通过。

## 关联文档和工作项

- [Aki 设计方案](../design/aki_design.md)第 11 节
- [Aki 实施总计划](../plans/aki-implementation-plan.md)（`SCOPE-09`、M2）
