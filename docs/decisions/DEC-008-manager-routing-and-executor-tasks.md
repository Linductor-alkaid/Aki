# DEC-008：Manager 职责切分、事件路由与 Executor 任务承载（M1-05）

> 状态：Accepted
> 日期：2026-09-22
> 负责人：Linductor
> 冻结里程碑：M1
> 替代/被替代：无

## 背景与问题

M1-05 要在 Application 层提供 Device / Conversation / Message / Transfer 四个
Manager 骨架（设计第 8.3/14 节）。落地前需冻结三类契约：① 四个 Manager 之间的
职责与 Store 写权限切分；② 9 类 `HeyakiAdapterSink` 事件到 Manager 与状态更新的
路由表；③ Manager 任务在 pinned executor（v0.5.0-7 @ `74a9419`）上的承载与取消
语义（`EXEC-04`/`EXEC-05`/`EXEC-07`）。若不先行固化，`BridgeSink` 测试形态
（业务处理留在 Adapter 回调线程）可能被延续为正式架构，违反 `EXEC-02` 与
AGENTS 规则 11。

## 决策

- **职责与所有权**：四个 Manager 落位 `app/application/`，每个 Manager 恰好只写
  自己领域的 Store（DM→devices、CM→conversations、MM→messages、TM→transfers），
  写入以类型化更新指令经 `MpscChannel` 汇聚到状态 owner（`DEC-002` / `RULE-02`）。
  出站操作按域切分（发现启停→DM、`send_text`→MM、传输四接口→TM）；CM 显式提供
  `ensure_conversation(local, remote)`，不从事件隐式建会话，其自建会话记录仅用于
  断连推导（创建记录，不复制 owner 权威状态）。
- **事件路由**：`app/application` 内单一 `RouterSink` 实现 `HeyakiAdapterSink`，
  回调线程只做有界校验并投递到各 Manager 私有有界 `MpscChannel` 收件箱
  （`EXEC-02`）；9 类事件逐一对应固定路由与状态更新（设计第 8.3 节路由表）；
  `connected` / `disconnected` 是仅有的双 Manager 扇出事件，主路径事件各只投递
  一次（由 DM 发布）。Sink 返回值为各路 admission 的合取。
- **任务承载**：每个 Manager 用"单飞有界排空"泵——工作项入收件箱 → CAS 抢占 →
  `submit_auto` 排空任务（有界批量、保留并消费 future、释放后复查收件箱防丢失
  唤醒）；长任务（传输会话类）用 `submit_cancellable` + `StopToken`，Manager 按
  业务稳定 ID 持有 `TaskHandle` + future 作为成员，取消一律经
  `executor().request_task_cancel(handle)` 发起（运行期协作轮询、排队期以
  `TaskCancelled(Explicit)` 结算），不经 StopSource 直发，业务代码不得抛
  `TaskCancelled` 做控制流。
- **观测通道**：取消经 `get_cancellation_status()` / `ExecutorSnapshot.cancellation`
  （独立计数）；超时经 failure 体系 `timeout_count`（`task_timeout_ms` 为排队软
  超时，config 级，不打断运行中任务）。任务健康以 Executor 监控设施为事实源
  （AGENTS 规则 9），Manager 自身仅保留域名义计数。
- **边界**：M1 不启用 blocking worker（`FakeHeyakiAdapter` 无真实 I/O、持久化
  M2、文件数据 M4）；M1 不引入 `TimerHandle` / 周期任务（presence 与进度均以
  事件到达，无自驱周期负载）。二者为延迟启用而非能力缺口，不进工程规范 9.4
  反馈台账（已对照 pinned 公开头文件与集成指南核实无缺口）。

## 备选方案

- 以 `BridgeSink` 直投 owner 作为正式架构：业务处理留在 Adapter 回调线程，违反
  `EXEC-02`/AGENTS 规则 11，且 M1-05 的目的就是把业务移入 Manager 上下文，否决。
- 每 Manager 一个 `SerialExecutionContext` + `submit_on`：其构造即自建
  `std::thread`、不入 `ExecutorSnapshot`，`wait_for_completion_ex` 不覆盖已发布
  未执行的串行回调（`EXEC-01` 步骤 4 盲区），违反 AGENTS 规则 1/9，否决。
- 无收件箱的 task-per-event：池内并发执行不保序，M1 退出-1 的消息顺序断言与
  "进度不先于 started"被破坏，否决（M3 真实适配器若引入多线程回调，在该决策下
  重评序语义）。
- 事件消息总线 / 单一 EventDispatcher 替代四 Manager：`DEC-002` 已否决消息总线
  （生命周期与取消路径不可观测），否决。
- Manager 自管 `std::stop_source` / 原子取消标志 + `submit_auto`：取消对 Executor
  生命周期视图不可见（无 CancellationStatus、无 in-flight Cancelled），违反
  `EXEC-05` 与 AGENTS 规则 1/6，否决。
- 自建 `submit_delayed` 超时器兜底长任务：Executor 已有排队软超时并入 failure
  统计，自建即平行监控（`EXEC-06` 禁止），否决。

## 影响与风险

- 三个类型化更新（`SetPresence` / `SetDeliveryState` / `CompleteTransfer`）属
  公开契约扩展（设计第 10.1 节已先行同步）；owner 校验纪律（幂等 no-op、终态
  不复活、未知 id 拒绝）与既有更新一致。
- Manager 内部状态只在排空上下文访问；TaskHandle/future 由 Manager 显式持有
  （`EXEC-07`），宿主关闭钩子必须先取消并消费在途 future 再进入 `EXEC-01`
  步骤 2~5，否则 Manager 先于任务终结会悬垂。
- 排队软超时击杀的排空任务不会自复位单飞标志，泵必须以下一次入队/flush 消费
  就绪 future 并自愈重排；该路径需测试覆盖（DOD-02 超时项）。

## 验证方式

M1-05 单测（`tests/unit/test_app_managers.cpp`）按 DOD-02 六项覆盖 Manager 任务
路径：正常完成（事件 → Manager 排空 → 快照/主路径可见，含并发入队不丢）、任务
异常（Adapter 抛出经 future 与 `task_exception_count` 可见且泵自愈）、提交拒绝
（`max_in_flight_tasks` 耗尽 → future 即时异常 + `capacity_exhausted_count`；
收件箱满 → 入队明确拒绝）、执行中取消（`request_task_cancel` + StopToken 轮询
退出 + `running_request_count`）、超时（独立 owner 小 `task_timeout_ms` 配置 →
`timeout_count` 且存量不丢）、shutdown（第 8.3 节关闭钩子顺序 →
`fully_stopped()`）；另覆盖迟到事件不复活终态（`RULE-08`）与 9 类事件路由/扇出。
MSVC debug/release ctest 与 GCC `-Wall -Wextra -Wpedantic -Werror` 语法检查；
ASAN/UBSAN 随 PR 的 Linux CI 门禁提供。

## 关联文档和工作项

- [Aki 设计方案](../design/aki_design.md)第 8.1/8.2/8.3/10/10.1/14 节
- [DEC-002](DEC-002-layering-and-state-boundary.md)（分层与状态边界）、
  [DEC-003](DEC-003-dependency-locking.md)（依赖锁定）
- [Aki 实施总计划](../plans/aki-implementation-plan.md)（`RULE-02`/`RULE-07`~`RULE-09`、
  `EXEC-02`~`EXEC-07`、`DOD-02`）
- [M1：领域模型与状态边界](../plans/m1-domain-state.md) 工作项 `M1-05`
