# M4：图片消息与文件传输

> 状态：Planned
> 负责人：Linductor
> 所属计划：[Aki 实施总计划](aki-implementation-plan.md)
> 前置：M3（`M3-01`~`M3-09` 工作项已完成并经 #29 收口审计；退出-1/退出-3 为
> 「部分验证 + 如实降级声明」，待防火墙放行入站 TCP / LAN 双端环境补跑后关闭
> ——环境补跑不阻塞本里程碑文档与设计先行工作项，M4 实现工作项开工时复核
> M3 状态；真实 Adapter、NodeSession、发现/消息/presence/重连管道均已就绪）
> 建议发布点：v0.4.0
> 更新日期：2026-09-24

## 目标

交付真实数据链路端的图片消息与文件传输能力（`SCOPE-07`/`SCOPE-08` 的非 UI
部分）：图片以 typed 消息承载 metadata + `TransferId`（`RULE-05`），文件本体经
独立 Transfer Session 传输——发送侧分块写入 `files/tmp/<transfer_id>.part`、
进度采样、终态 `Completed` 流式 SHA-256 + 原子改名 + 回写（M2-06 作业组真实
接线）；接收侧落盘与完成合并；暂停/恢复/取消全状态机推进；传输历史实时持久化
并重启恢复一致。`transfer/manager/` 目录自本里程碑落地（M1-08 遗留项）。本
里程碑不引入 UI（Transfers 页面归 M5/`SCOPE-12`），不改变 M3 文本消息语义。

## 范围与非目标

### 范围

- `M4-01` 设计先行：第 7 节传输集成契约小节（M1-08 纪律，先文档后代码）——
  TransferManager 职责切分（`DEC-008` 模式扩展）、传输 typed 更新→DB 作业映射
  （`UpsertTransfer`/`UpdateTransferProgress`/`CompleteTransfer` 既有 + 发起/
  暂停/恢复/取消更新补充）、heyaki BLAKE3 wire 校验与 `DEC-004` SHA-256 存储
  哈希的关系澄清、`.part` 写入在 blocking worker 的承载与 `DatabaseWorker`
  通道容量预算复核（`DEC-009` 复核触发条款）、DEC-006 映射 7 的实现级细化。
- `M4-02` `transfer/manager/` TransferManager 落地：传输会话七状态机
  （`Queued`/`Negotiating`/`Transferring`/`Paused`/`Completed`/`Failed`/
  `Cancelled`，设计第 7 节）全覆盖、任务经 `submit_cancellable` + StopToken
  承载（`EXEC-05`）、有界收件箱 + 单飞排空泵（`DEC-008` 模式）、`TaskHandle`
  按业务稳定 ID 显式持有（`EXEC-07`）。
- `M4-03` 图片消息（`SCOPE-07`）：`MessageType::Image` typed 消息发送/接收——
  消息面仅 `FileMetadata` + `TransferId`，图片本体经传输链路（`RULE-05`）；
  收发两侧 `DeliveryState`/传输状态联动。
- `M4-04` 发送侧文件传输真实数据链路（`SCOPE-08`）：发起（`push_file`）→
  `Negotiating`→`Transferring`→`.part` 分块写入（blocking worker，`EXEC-04`）→
  `Completed` 流式 SHA-256 + 原子改名 + hash/size/relative_path 回写（M2-06
  终态作业组从测试内字节源切换为真实文件源）；进度经
  `UpdateTransferProgress` 实时持久化。
- `M4-05` 接收侧与暂停/恢复/取消（`SCOPE-08`）：接收侧 `.part` 落盘与完成
  合并；`pause_transfer`/`resume_transfer`/`cancel_transfer` 驱动状态机推进、
  `Failed`/`Cancelled` 幂等删除 `.part`、终态幂等（`RULE-08`）；`on_transfer_*`
  事件路由（`DEC-008`）。
- `M4-06` 双端传输回环验证：回环（环境受限沿 M3 降级纪律）发现 → 信任 →
  图片消息 → 文件传输（进度、暂停/恢复/取消）→ 终态文件本体 SHA-256 一致 →
  历史与传输行重启恢复一致；DOD-02 六项沿传输并发路径覆盖。
- `M4-07` 收口审计与退出证据归集（沿用 M1-08/M2-08/M3-09 纪律）：实现与设计
  第 6/7/11.1/14 节及 `DEC-004`/`DEC-006` 逐项校对，退出-1~5 证据与可复现
  命令归档，环境受限项如实降级声明并登记补跑条件。

### 非目标

- EUI-NEO UI 与 Transfers 页面、文件卡片展示（M5，`SCOPE-12`）。
- 语音/视频、远程能力、Agent（`SCOPE-13`~`SCOPE-17`、`POST-NN`）。
- 内容寻址去重（`DEC-004` 延后项，触发条件不变）。
- 断点续传语义超出 heyaki 传输会话既有能力范围的部分（以 pinned 版本公开
  API 为准，`DEC-006`；能力缺口按工程规范 9.4 处置）。

## 设计与决策依据

- [Aki 设计方案](../design/aki_design.md)第 6 节（消息模型与 `Image` 类型）、
  第 7 节（Transfer Session 与七状态机）、第 8.1 节（传输四接口「M4 前仅签名
  与 TransferId 语义」）、第 8.3 节（Manager 装配与关闭钩子）、第 10.1 节
  （typed 更新）、第 11.1 节①（写路径映射：`UpsertTransfer`/进度/终态回写）、
  第 14 节（`transfer/transfer`、`transfer/manager`、`transfer/storage` 落点）。
- [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)（文件本体布局、
  `.part` 生命周期、SHA-256、终态作业组——M2-06 已实现待真实接线）、
  [DEC-006](../decisions/DEC-006-heyaki-api-contract.md)（映射 7：`push_file`/
  `pause`/`resume`/`cancel` + `set_file_event_observer` → `on_transfer_*`，M4
  预留条款）、[DEC-008](../decisions/DEC-008-manager-routing-and-executor-tasks.md)
  （Manager 模式与取消）、[DEC-009](../decisions/DEC-009-appstate-write-path.md)
  （写路径正式落点与容量预算复核触发条款）。
- M1 遗留「`transfer/manager/` 留 M4」（[M1 里程碑](m1-domain-state.md)
  M1-08 记录）；M2-06 文件存储「真实传输数据链路在 M4」（[M2 里程碑](m2-local-persistence.md)）。
- AGENTS.md Executor 强制规则与工程规范第 9 节；文件 I/O 属 blocking worker
  承载（`EXEC-04`，M2-05 DatabaseWorker 通道与 M2-06 作业工厂复用）。

## 工作项

- [ ] `M4-01` 设计先行：第 7 节传输集成契约小节固化（TransferManager 职责、
  传输 typed 更新→作业映射、BLAKE3/SHA-256 关系澄清、`.part` 写入 blocking
  worker 承载与通道预算复核、DEC-006 映射 7 实现级细化）；偏差先更新设计/
  决策再合代码（M1-08 纪律）。
- [ ] `M4-02` `transfer/manager/` TransferManager：传输会话七状态机全覆盖
  （合法/非法转移、终态幂等）、`submit_cancellable` + StopToken 任务承载、
  有界收件箱 + 排空泵、按业务稳定 ID 持有句柄。
- [ ] `M4-03` 图片消息（`SCOPE-07`）：`Image` typed 消息收发，消息面仅
  metadata + `TransferId`，本体经传输链路（`RULE-05`）。
- [ ] `M4-04` 发送侧真实传输链路（`SCOPE-08`）：`push_file` 发起 → 分块写入
  `.part`（blocking worker）→ 进度实时持久化 → `Completed` SHA-256 + 原子
  改名 + 回写（M2-06 作业组真实接线）。
- [ ] `M4-05` 接收侧与暂停/恢复/取消（`SCOPE-08`）：接收落盘与完成合并；
  pause/resume/cancel 状态机推进与 `.part` 幂等删除；`on_transfer_*` 路由；
  终态幂等。
- [ ] `M4-06` 双端传输回环验证：全链路（图片 + 文件 + 进度 + 暂停/恢复/取消 +
  重启恢复一致）；环境受限沿 M3 降级纪律（网络无关断言拆分、[skip] 显式 +
  补跑条件）。
- [ ] `M4-07` 收口审计与退出证据归集（沿用 M1-08/M2-08/M3-09 纪律）。

## 风险与阻塞

- **防火墙/LAN 双端环境限制**：M3 已登记的补跑条件（防火墙放行入站 TCP /
  专用测试网络 / LAN 双端真机）同样约束 M4 的双端回环全链路验证——沿 M3-04~
  09 降级纪律处置（网络无关断言拆分 unit、[skip] 显式 + 补跑条件），不冒充
  已验证；补跑由负责人执行。
- **BLAKE3/SHA-256 关系**：heyaki 传输 wire 校验为 BLAKE3，`DEC-004` 存储哈希
  为 SHA-256——两者用途不同（wire 完整性 vs 存储哈希），预期无冲突；
  `M4-01` 设计先行时澄清并固化，如出现实际行为冲突再立统一决策，不得静默。
- **blocking worker 通道预算**：文件分块写入作业与 DB 作业共享 `DatabaseWorker`
  通道——`M4-01` 按第 11.1 节① 容量预算模型复核（`DEC-009` 复核触发条款：
  单更新作业数上限变化须重算并同步设计）；大文件传输时长对 drain 预算的影响
  需评估（传输作业长期占用 worker 的排队影响）。
- **进度采样周期任务**：如进度采样需要周期任务（EXEC-04 timer），`TimerHandle`
  由 TransferManager 显式持有（`EXEC-07`）；优先以 `on_transfer_progress` 事件
  驱动，不引入自驱周期负载（`DEC-008` 边界条款）。
- **测试资源**：大文件用例控制体积与数量（CI 时长预算），分块边界（空文件、
  非整块尾部、多块）显式覆盖。
- pinned heyaki 升级不属本里程碑；`DEC-005`（EUI-NEO 集成）干 M5 开始前冻结，
  不阻塞本里程碑。

## 测试与退出条件

- [ ] 退出-1：双端回环图片 + 文件传输全链路——发起 → 进度 → 终态文件本体
  SHA-256 一致 + 暂停/恢复/取消语义 + 历史/传输行重启恢复一致
  （`SCOPE-07`/`SCOPE-08`）；环境受限时按 M3-09 先例「部分验证 + 如实降级
  声明」处置并登记补跑条件。
- [ ] 退出-2：DOD-02 六项沿传输并发路径通过——正常完成、任务异常、提交拒绝、
  执行中取消（重连/传输长任务 StopToken）、超时、shutdown（含在途文件作业
  drain 语义）。
- [ ] 退出-3：传输状态机七状态合法/非法转移全覆盖、终态幂等、暂停/恢复语义、
  取消删除 `.part`（`RULE-08`/`RULE-09`）。
- [ ] 退出-4：debug/release 构建与全量测试通过；ASAN/UBSAN/TSAN 随 CI 门禁
  （抑制表仅上游条目纪律不放松）；不适用工具链记录限制与补跑条件。
- [ ] 退出-5：设计（第 7 节集成契约）、决策（`DEC-004`/`DEC-006` 如有回填）、
  总计划与里程碑状态同步；验证记录含可复现命令；环境受限项降级声明完整。

## 验证记录

（尚无记录；自 `M4-01` 起按工程规范 6.1/6.3 追加。）
