# M2：本地持久化

> 状态：Planned
> 负责人：Linductor
> 所属计划：[Aki 实施总计划](aki-implementation-plan.md)
> 前置：M1（已关闭，`app/lifecycle`/`app/state`/`app/application` 骨架就绪；
> [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md) 已于 2026-09-22
> 提前冻结，本里程碑直接依据其执行）
> 建议发布点：v0.2.0
> 更新日期：2026-09-23

## 目标

交付 `SCOPE-09` 的本地持久化能力：按 [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)
以 SQLite（官方 amalgamation，vendored 引入）保存设备身份、受信设备、Conversation
metadata、消息历史与 Transfer history，DB 访问经 Executor blocking worker 承载
（`EXEC-04`，[设计第 8.2 节](../design/aki_design.md)标注的首次启用里程碑）；
落实文件本体存储布局与生命周期纪律；实现重启后恢复。本里程碑不接触真实网络。

## 范围与非目标

### 范围

- vendored SQLite 接入：amalgamation（锁定 3.53.4）并入 `third_party/sqlite`，
  锁文件 `class=vendored` + SHA-256 登记，configure 校验分支（`DEC-004`、`DEC-003`）。
- persistence 薄 RAII 封装（`Database` / `Statement` / `Transaction` 守卫 +
  `SqliteError`）与 `user_version` 版本化迁移框架；`sqlite3*` 封死在 persistence
  层（`RULE-10`）。
- 设计第 11 节 ER 模型 4 张表（DEVICE / CONVERSATION / MESSAGE / TRANSFER）schema、
  领域枚举 `INTEGER + CHECK` 映射与仓储层。
- `DatabaseWorker`：单一 blocking worker 独占连接、有界工作通道串行消费 DB 作业
  （`EXEC-04`），取消经 StopToken 在语句间检查，admission 拒绝与失败经 Executor
  监控设施可观察（`EXEC-06`/`RULE-09`），shutdown 按 `EXEC-01` drain 后 join。
- 文件本体布局与生命周期：`files/tmp/<transfer_id>.part` 写入、终态 `Completed`
  流式 SHA-256 + 原子改名、`Failed` / `Cancelled` 幂等删除、启动清扫残留
  （`TransferId` 字符集约束与路径净化，`DEC-004`）。本里程碑以测试内字节源驱动
  该路径（真实传输数据链路在 M4）。
- 重启恢复：写入 → 关闭 → 重开，设备/信任/会话/消息/传输历史一致；DB 损坏时
  open 干净失败。

### 非目标

- 真实 Heyaki 接入与网络断线恢复（`SCOPE-11` 归 M3/M4；本里程碑的"恢复"指进程
  重启后的持久化恢复）。
- 图片/文件传输的真实数据链路与传输页面（M4 / `SCOPE-07`/`SCOPE-08`），本里程碑
  仅落实存储布局与传输历史记录的持久化语义。
- EUI-NEO UI（M5）；设计第 11 节提及的应用设置存储（`SCOPE-09` 未含，出现真实
  需求时按 `DEC-004` 流程扩展 schema 并补充决策）。
- 内容寻址去重（`DEC-004` 已记为带触发条件的延后项）。

## 设计与决策依据

- [Aki 设计方案](../design/aki_design.md)第 8.2 节（blocking worker 自 M2 起按负载
  启用）、第 8.3 节（首次启用预期为 M2 历史读写 I/O）、第 11 节（存储划分与 ER
  模型）、第 14 节（`persistence/database`、`persistence/repository`、
  `persistence/migration` 目录）。
- [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)：引擎与访问方式、
  vendored 引入与 SHA-256 校验、`DatabaseWorker` 承载、文件本体布局、迁移纪律、
  M2 验证方式（本里程碑退出条件的直接依据）。
- [DEC-003](../decisions/DEC-003-dependency-locking.md)：锁定 + configure 校验、
  漂移即失败纪律；[DEC-001](../decisions/DEC-001-cpp-baseline.md)：预设矩阵与
  测试标签；[DEC-007](../decisions/DEC-007-test-framework.md)：`unit` /
  `integration` 标签。
- AGENTS.md Executor 强制规则与工程规范第 9 节；blocking worker 首次启用前按
  pinned executor 集成指南加载对应 capability card（blocking I/O 路由）。

## 工作项

- [ ] `M2-01` 补充设计第 11 节持久化集成契约小节：DB 写路径由哪些 Manager 动作
  触发（对应第 10.1 节 typed 更新）、启动恢复流程与 Application State 的关系、
  `DatabaseWorker` 在 `EXEC-01` 关闭顺序中的落点；偏差先更新设计再合代码
  （M1-08 纪律）。
- [ ] `M2-02` 落地 vendored SQLite 接入：官方 amalgamation 并入
  `third_party/sqlite`（仅 `sqlite3.c` / `sqlite3.h`），锁文件登记
  `class=vendored` + 版本 + SHA-256，`cmake/Dependencies.cmake` 增加
  `file(SHA256)` 校验分支（不匹配即 `FATAL_ERROR` + 修复提示），编译宏
  `SQLITE_DQS=0` / `SQLITE_OMIT_LOAD_EXTENSION=1`，debug 构建断言
  `sqlite3_sourceid` 与锁文件版本一致。
- [ ] `M2-03` 提供 persistence 薄 RAII 封装与迁移框架：`Database` / `Statement` /
  `Transaction` 守卫 + `SqliteError`；open 处统一设置 `journal_mode=WAL`、
  `synchronous=NORMAL`、`foreign_keys=ON`、`busy_timeout`；`persistence/migration`
  按 `PRAGMA user_version` 版本化迁移（前进成功与失败回滚路径）；`sqlite3*` 不出
  现在 persistence 层外公开头文件的编译级验证（`RULE-10`）。
- [ ] `M2-04` 提供第 11 节 ER 模型 4 张表 schema（迁移 M1 起步版本）与仓储层：
  领域枚举（`TrustState` / `MessageType` / `DeliveryState` / `TransferState`）
  `INTEGER + CHECK` 映射，DEVICE / CONVERSATION / MESSAGE / TRANSFER 的 CRUD，
  prepared-statement 缓存容量上限（`RULE-09`）。
- [ ] `M2-05` 提供 `DatabaseWorker`：经 `ExecutorOwner` 注册 blocking worker
  （首次启用，设计第 8.2 节落点），单一连接 + 有界工作通道串行消费、StopToken
  在语句间协作取消、通道满与执行失败经 Executor 监控设施可见（`EXEC-04`/
  `EXEC-06`/`RULE-09`），关闭顺序并入 `EXEC-01`（drain 在途 DB 作业后 join）。
- [ ] `M2-06` 落实文件本体存储布局与生命周期：数据根目录（Platform Adapter 解析，
  Windows `%APPDATA%` / Linux XDG，Core 不见平台类型）下 `db/aki.db3` 与 `files/`
  并置；`files/tmp/<transfer_id>.part` 写入、`Completed` 终态流式 SHA-256 后原子
  改名到 `files/<transfer_id>/<净化文件名>`、`Failed` / `Cancelled` 幂等删除
  `.part`、启动清扫无活动 Transfer 行的残留；`TransferId` 字符集约束
  `[A-Za-z0-9_-]{1,64}` 固化于生成处，远端原始文件名只存 DB 不拼路径
  （`DEC-004`；本项以测试内字节源驱动，真实链路 M4）。
- [ ] `M2-07` 实现重启恢复并写入宿主/测试路径：设备、信任状态、会话、消息与
  传输历史写入 → 受控关闭 → 重新 open 后逐域断言一致（`SCOPE-09` 验收）；
  DB 文件损坏时 open 干净失败不静默。
- [ ] `M2-08` 收口审计与退出证据归集（沿用 M1-08 纪律）：实现与设计第 11 节/
  `DEC-004` 逐项校对，退出-1~5 证据与可复现命令归档，供应链登记
  （zip SHA3-256 + 文件 SHA-256、public domain 许可证结论入
  `docs/supply-chain/`）。

## 风险与阻塞

- `sqlite3.c` 纳入 asan/ubsan preset 编译；若 UBSan 对 SQLite 内部报点，仅在
  sqlite target 上做编译旗标豁免并记录，不放松全局（`DEC-004` 影响节）。
- 本地 MinGW 默认生成器仍受 pinned executor 构建缺陷阻塞（M1 验证记录限制 1），
  本地验证以 MSVC 生成器等价执行；`Dependencies.cmake` 的 vendored 校验分支属
  configure 逻辑，需在 MinGW/MSVC/CI Linux 三套工具链的 configure 均验证。
- WAL 约束：db 与 files 必须位于本地盘；网络文件系统场景在测试中显式记录限制
  （`DEC-004`）。

## 测试与退出条件

- [ ] 退出-1：重启恢复——写入 → 关闭 → 重开，设备/信任/会话/消息/传输历史逐域
  一致；损坏 DB open 干净失败。
- [ ] 退出-2：并发基线六项沿 `DatabaseWorker` 路径通过——正常完成、任务异常、
  提交拒绝（通道满/关闭后）、执行中取消（语句间 StopToken）、超时、shutdown
  （drain 后 join）。
- [ ] 退出-3：迁移与约束测试通过——`user_version` 前进成功、失败路径不破坏既有
  数据；枚举 `CHECK` 拒绝非法值；重复迁移幂等。
- [ ] 退出-4：debug/release 构建与全量测试通过；ASAN/UBSAN 通过（随 CI 门禁），
  不适用工具链记录限制与补跑条件；DB worker 与 Manager 跨上下文交互按 `DOD-03`
  评估 TSAN/故障注入。
- [ ] 退出-5：设计（第 11 节集成契约）、决策（`DEC-004` 验证方式回填）、总计划
  状态同步；验证记录含可复现命令与结果；供应链文档已登记。

## 验证记录

（按日期追加；本里程碑尚未开工。）
