# DEC-009：AppStateOwner 接受后处理器（持久化写路径正式落点）与 UpsertMessage 会话归属

> 状态：Accepted
> 日期：2026-09-24
> 负责人：Linductor
> 冻结里程碑：M3（M3-02 设计先行固化，实现随 M3-03+ 跟进）
> 替代/被替代：取代 [M2 里程碑](../plans/m2-local-persistence.md) M2-07 验证记录中
> 「宿主快照权威值镜像」偏差段的临时形态（M2 历史记录保持原样，不回改）；不替代
> [DEC-004](DEC-004-local-persistence-sqlite.md)/[DEC-006](DEC-006-heyaki-api-contract.md)/[DEC-008](DEC-008-manager-routing-and-executor-tasks.md)

## 背景与问题

第 11.1 节 ①（M2-01 固化）要求 DB 作业由 typed 更新驱动、在更新被状态 owner 接受
后由 owner 单写者上下文按接受顺序入队。M2-07 实现受「app/state 公开契约不改」的
任务约束，采用宿主组合根的「快照权威值镜像」过渡形态（每步 quiesce 后以快照中
已接受实体镜像入队），并在其验证记录中声明两项偏差：① 幂等 no-op 接受在快照无
变化时不产生作业（§11.1 ① 原句「接受包含幂等 no-op 同样入队」被跳过）；② 正式
落点（接受后回调或 owner 侧 tap）待 M3 真实事件源接入时评估。M3-05 要求消息历史
实时持久化——Manager 驱动的真实事件流没有宿主 quiesce 时点可用，镜像形态不再
成立，必须在动代码前固化正式契约（M1-08 纪律：先文档后代码）。

同批固化的阻塞子决策：`UpsertMessage` 的会话归属缺口。MESSAGE 行需要
conversation_id（FK，`MESSAGE 1-* CONVERSATION`），但第 6 节 `Message` 模型与
`UpsertMessage` 载荷均无会话归属字段（`MessageRepository::upsert` 由调用方提供，
`repositories.hpp:74`）；M2-07 由宿主簿记补齐（console 宿主的
`mirror.conversation`，`main.cpp`）。M3-05 落消息行前必须确定归属的权威来源。

## 决策

**① 持久化写路径正式落点 = `AppStateOwner` 构造注入的接受后处理器**（设计第
10.1 节/第 11.1 节 ① 已同步）：

- 契约形态：`using PostAcceptHandler = std::function<void(const AppStateUpdate&)>`，
  作为 `AppStateOwner` 构造入参注入（与初始快照同级的显式入参，默认空——M1/M2
  形态兼容）；对齐 `ManagerPump` 的 Handler 先例（`app/application/
  manager_runtime.hpp:115`，`DEC-008`）。
- 调用时序：`drain_updates` 在 `apply()` 返回 true（更新被接受，计入
  `updates_applied`）后，于 owner 单写者上下文按接受顺序同步调用处理器恰好一次；
  被拒绝的更新不调用。处理器把第 11.1 节 ① 映射的 DB 作业入队 `DatabaseWorker`
  有界通道（组合根实现复用 M2-07 `PersistenceMirror` 的 jobs_for 映射与
  `persistence/repository/update_jobs.hpp` 工厂；`SetPresence` /
  `SetConnectionPath` 不持久化、无作业）。入队不新增执行上下文（第 10.1 节硬
  约束 1 的延伸），同一实体的作业顺序即接受顺序。
- **幂等 no-op 同样入队**（补回 §11.1 ① 原句）：对已终态传输重复
  `CompleteTransfer(Completed)` 等幂等接受同样经处理器入队，由作业侧幂等语义
  吸收（M2-06 作业组跳过条件），不在处理器侧去重——owner 不新增簿记状态。
- 异常策略：处理器**不得抛出异常**（实现纪律）；owner 侧仍全捕获并计入新增的
  `post_accept_failures` 统计（可观测，`RULE-09`）——异常不上浮、不中断本批
  drain 的后续更新与事件处理（handler 挂掉不能拖垮状态机）。
- 容量预算（`RULE-09`）：owner drain 批 64 × 每接受更新至多 2 个作业
  （`CompleteTransfer` 的 `Failed`/`Cancelled` 分支 = 终态列更新 + `.part` 删除
  两个串行作业）= 128 ≤ `DatabaseWorker` 通道容量 256。通道满/已关闭/未注册的
  入队拒绝经处理器侧计数与 `DatabaseWorkerControl::rejected_count` 双可见，
  不静默重试、不回滚已接受的内存更新（第 10.1 节状态边界单向）；内存态与持久化
  的分歧经计数暴露，由上层策略处理。
- 装配时序（第 8.3 节/第 11.1 节 ② 已同步）：组合根顺序固化为
  `ExecutorOwner.initialize() → 启动恢复 → DatabaseWorkerControl（先于 owner
  构造，锚定恢复移交的单一连接）→ AppStateOwner(初始快照 + 处理器) → 四
  Manager → start_blocking_worker + mark_registered → RouterSink`。「worker 在
  播种完成后才注册」指注册时点而非 control 对象构造时点；未注册窗口内入队明确
  拒绝且计数可见，标准装配中该窗口无更新流、预期计数 0。

**② `UpsertMessage` 扩展会话归属字段，第 6 节 `Message` 模型保持不变**：

- `UpsertMessage` 载荷增加 `ConversationId conversation` 字段（扩展更新载荷，
  第 10.1 节 typed 指令的归属补充；与 `MessageDeliveredEvent` 自带 conversation
  的先例一致，`app/state/app_events.hpp:33-36`）。owner 校验扩展：载荷的
  conversation 必须已在 `ConversationStore`（FK 语义在状态边界前置校验，未知
  会话拒绝并可观测）。
- 第 6 节 `Message`（领域/展示模型）不加会话归属字段：消息的会话归属是持久化
  关联元数据而非消息本体语义——会话内消息列表可由 MESSAGE 行 FK 查询/恢复
  重建，无需在内存模型冗余；改动 §6 会牵动协议表示与 UI 模型，收益为零。
- `MessageRepository::upsert(message, conversation_id)` 既有签名不变（调用方
  从更新载荷取归属，不再依赖宿主簿记）。

## 备选方案

- **owner 侧 tap（第二入队准入点）**：在 owner 更新入口（submit 侧）tap 入队。
  否决——tap 点在 accept 校验之前，会把被拒绝的更新也产生作业（或需要重放
  校验）；且与 drain 批内消费形成两个并发写 `DatabaseWorker` 通道的上下文，
  跨通道保序复杂，有 `RULE-09` 风险。
- **维持 M2-07 宿主快照镜像**：否决——镜像依赖「每步 quiesce 到静止后读快照」
  的宿主节奏，M3-05 真实事件流由 Manager 泵驱动、无宿主 quiesce 时点；幂等
  no-op 不产生作业的偏差也无法补回。保留为 console 宿主过渡形态（M3-03+
  切换，切换前镜像与处理器不并存）。
- **9 类事件面驱动持久化**：否决——§11.1 ① 明文「不由 9 类事件驱动（事件是
  通知面）」；事件可丢失（Topic best-effort）且不承载 owner 校验结论。
- （子决策备选）**第 6 节 `Message` 模型加 conversation_id 字段**：否决——
  理由见决策 ②；领域模型保持与协议/展示对齐，归属归 typed 更新载荷。

## 影响与风险

- **公开 API 变更**（工程规范 6.2 同步矩阵「公开 API/事件 schema」行）：
  `AppStateOwner` 构造入参新增 `PostAcceptHandler`（默认空，源兼容）；
  `AppStateOwnerStats` 新增 `post_accept_failures` 计数；`UpsertMessage` 载荷
  新增 `conversation` 字段（聚合初始化处需补字段——源不兼容点，随实现批次全仓
  修正）。事件 schema 不变。
- **实现迁移**：M3-03+ 批次切换 console 宿主与相关测试到新装配序（§8.3 固化
  序）；切换前 M2-07 镜像形态继续工作（两者不并存于同一进程）。`test_app_state`
  需补：接受才调用/拒绝不调用/顺序/异常全捕获计数/通道满双计数五类断言。
- **处理器内入队失败 ≠ 更新失败**：accepted 内存态与持久化的分歧只经计数暴露
  （既有 §11.1 ① 失败语义不变）；上层策略（日志/退出）归宿主。
- **容量复核触发条件**：若未来单更新作业数上限提高或 drain 批扩大，须重算
  64 × n ≤ 通道容量并同步 §11.1 ①。
- 与 `EXEC-02` 无冲突：处理器在 owner 上下文执行，不在 Adapter 回调线程；
  与 `EXEC-04` 一致：作业仍由 DatabaseWorker 串行消费。

## 验证方式

契约文档先行（本记录 + 设计第 10.1/11.1/8.3/8.1 节同步，M3-02）；实现验证归
M3-03+ 批次：接受后处理器仅在 accept 后调用且顺序正确；幂等 no-op 产生作业并被
作业侧幂等吸收；处理器异常被全捕获且 `post_accept_failures` 可见、drain 继续；
通道满时处理器侧与 `rejected_count` 双计数一致；M3-05 消息行以载荷归属实时落库、
重启恢复一致；切换后全量 ctest 零回归（含 M2 既有 19+1 项）。

## 关联文档和工作项

- [Aki 设计方案](../design/aki_design.md)第 10.1 节（owner 契约）、第 11.1 节
  ①②（写路径正式落点/装配时序）、第 8.3 节（装配顺序固化）、第 6 节（Message
  模型保持不变）、第 8.1 节（记录型来源触发语义，M3-02 同批细化）
- [DEC-004](DEC-004-local-persistence-sqlite.md)（写路径映射与作业承载不变）、
  [DEC-006](DEC-006-heyaki-api-contract.md)（第 8.1 节映射权威）、
  [DEC-008](DEC-008-manager-routing-and-executor-tasks.md)（Handler 先例与回调
  路由）
- [M2 里程碑](../plans/m2-local-persistence.md)（M2-07 偏差段——被本记录取代的
  临时形态；M2 历史记录保持原样）
- [M3 里程碑](../plans/m3-heyaki-integration.md)（`M3-02` 本记录、`M3-05` 实时
  持久化实现、`M3-08` 宿主切换）
