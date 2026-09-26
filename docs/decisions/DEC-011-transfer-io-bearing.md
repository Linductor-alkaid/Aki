# DEC-011：发送侧分块文件 IO 承载形态与传输会话循环收口

> 状态：Accepted
> 日期：2026-09-26
> 负责人：Linductor
> 冻结里程碑：M4（`M4-04` 实现前冻结；本记录即冻结，调研依据见文末）
> 替代/被替代：无（对设计 §7.1①③ 的 M4-01 文本作显式修订；不改变
> [DEC-006](DEC-006-heyaki-api-contract.md) 映射 7 的 wire 语义）

## 背景与问题

M4-04 交付发送侧真实数据链路（`start_file_transfer` → `push_file` → `.part`
分块归档 → `Completed` 终态组回写），开工前必须定案三件事：

1. **分块 IO 由谁执行**：设计 §7.1③（M4-01）留下两份待 reconcil 文本——硬结论
   「分块 IO 不经 DatabaseWorker 通道」与「大文件长作业仅占用 blocking worker
   执行时长」并存，但现行实现（M1 骨架与 M4-02 组件）的分块循环都是**池上
   `submit_cancellable` 会话任务内 `sleep_for` 轮询**：每个活跃传输会话停占一个
   池 worker。2 vCPU 设备自适应池恰 2 worker（executor `config.hpp`：
   `min_threads=0` 哨兵→`hw_concurrency` 且下限 2），≥2 并发传输即停占全部池
   worker、Manager 排空任务永无调度——CI 已实测 ~916s 停占级联（M4-03 验证
   记录观察③，测试夹具以固定 4 worker 规避并把生产侧 2 核饥饿登记给 M4-04）。
   不定案承载形态，真实数据链路无法在既定 EXEC-04 约束内落地。
2. **会话循环形态**：真实发送侧 wire 路径由 heyaki `push_file` 自读源文件并经
   `set_file_event_observer` 在 executor 上下文回报（DEC-006 映射 7），Aki 侧
   仅剩文件 IO（发送前流式 SHA-256 + 归档拷贝 `source → files/tmp/<id>.part`）
   ——「会话长任务」是否还需要存在、以什么形态存在须冻结。
3. **组件收口**：`transfer/manager/transfer_session_manager.hpp`（M4-02，未接入
   app 栈）与 `app/application/transfer_manager.hpp`（M1 骨架）双组件并存，
   职责重叠（各有会话表/泵/状态推进），M4-04 重构时必须一并收口，否则留下
   两套漂移的会话语义。

## 决策

**① 分块 IO 承载（§7.1③ 重写）**：发送侧固化为「**事件驱动会话状态机（无池上
会话长任务）+ Aki 侧分块文件 IO 走新增专用 blocking worker（名
`aki.transfer-io`，DatabaseWorker 同款 `IBlockingIoWorker` + 有界
`MpscChannel` 作业通道形态）上的逐块续接**」：

- **事件驱动会话状态机**：传输会话没有 `submit_cancellable` 池任务——wire 侧
  状态由 heyaki 文件事件（经 `set_file_event_observer` → RouterSink → TM 泵，
  §8.3 既有 EXEC-02 路径）驱动；Aki 侧归档（hash/copy 相位）由 IO 作业完成
  事件驱动，泵上下文推进（会话状态只在排空上下文访问，DEC-008 不变量保持）。
  取消经「worker StopToken（全局）+ 会话控制位（`closing`/`paused`，块间检查）
  + 续接抑制」表达，不经 `request_task_cancel`（无池任务句柄）。
- **专用 transfer IO worker**：与 `aki.db-worker` 同款形态（`IBlockingIoWorker`
  的 `run(StopToken)`/`wakeup()` + 有界作业通道 + 关闭侧存量按取消结算/清理），
  名 `aki.transfer-io`（worker 名唯一，进程内与 `aki.db-worker`、
  `heyaki-asio`/`heyaki-asio-file-io` 并存，已部署先例能力内）。作业为
  **offset 基础的无状态分块**（对齐断点续传：恢复后从持久化 `transferred`
  续传），**每会话单飞**（per-session single-flight：每会话至多一个在飞分块
  作业，完成事件回到泵后续接下一块）——「顺序写 `.part`」不变量由单飞保持，
  通道有界满即拒绝可见（RULE-09）。通道等待为 `try_receive` + 短睡眠轮询
  （pinned v0.5.0-7 `receive_for` 上游缺陷的既定备选，M2-05 注记），停止响应
  上界 = 轮询间隔。
- **进度聚合**：进度列以 wire 事件（heyaki `bytes_done`）为准；泵侧每会话
  最新进度槽 + 单飞 dirty 工作项——每次排空至多一个 `UpdateTransferProgress`
  （不可逐分块入箱：heyaki 逐分块事件率可达每秒数千，TM 收件箱仅 256）。
- **终态闸门**：`CompleteTransfer(Completed)` 仅在归档 `.part` 写完后放行入队
  ——归档未完成时持有 wire 终态事件（M2-06 终态作业组对不完整 `.part` 按契约
  明确失败，risk ③ 顺序敏感点）；`Failed`/`Cancelled` 终态不等待归档（`.part`
  幂等删除，在飞块结束后清理）。
- **hash-first 消息排序**（§7.1④ 落地）：发送方 SHA-256 以分块作业流先行，
  消息发送（`stored_sha256` 随 `FileMetadata` 携带，DEC-010 冻结字段号 5）
  等 hash 完成后在泵上下文经注入延续触发；`push_file` 不等 hash（wire 完整性
  是 heyaki BLAKE3 的职责）——大文件的消息可见延迟 = hash 单遍时长（如实登记，
  与单遍 copy+hash 互斥：消息先于拷贝完成发出）。归档 hash 失败时延续以空
  hash 触发、调用方不发消息，wire 侧自行终结。
- **DB 通道预算不变**：分块 IO 不经 DatabaseWorker 通道（M4-01 结论维持且
  更干净——DB drain 不再等待任何分块）；批上限维持 64×2≤256，终态作业组
  （流式 SHA-256 + 原子改名 + 回写）仍经 DatabaseWorker 通道（§11.1④/M2-06），
  由真实 `.part` 供源。

**② 会话循环与组件收口**：M1 骨架的 `session_loop`（`sleep_for` 轮询停占）与
M4-02 组件的 `session_loop`/`IoChunkHook` 一并废除——app
`TransferManager`（`app/application/transfer_manager.hpp`）重构为唯一传输会话
所有者：单飞排空泵（DEC-008 模式不变）+ 事件驱动会话表（hash/copy 相位、
单飞位、终态闸门、进度槽）+ 经 `transfer/storage/transfer_io.hpp` 承载面消费
IO worker。`transfer/manager/transfer_session_manager.hpp`（M4-02 组件，未接入
app 栈）删除，其 9 用例单测（DOD-02 六项 + 七状态机/RULE-08）由真实 IO 路径
测试沿新形态重写覆盖（状态机合法边在领域状态机与 owner 层已有独立覆盖）。
设计 §14 的 `transfer/manager/` 目录自本决策起由 app 层 TransferManager 承载
（目录留空，后续如需拆分再启用）。

**③ 承载面与层向**：IO 承载接口 `TransferIo`（事件 + start/advance/cancel +
idle/拒绝计数）声明于 `transfer/storage/transfer_io.hpp`（域层，仅 aki/std
类型，RULE-10）；实现 `TransferIoWorker`（`persistence/storage/`，FileStore +
流式 Sha256 + blocking worker 接线）——persistence 依赖域（既有方向），app
经接口消费（app→persistence 为既有允许方向，测试面仅依赖接口头）。事件投递
面：worker 线程经注入 sink 把完成事件有界投递回 TM 泵收件箱（MpscChannel
线程安全；投递满时 worker 侧有界重试，仍失败计拒绝可见——泵持续排空下不可达
饱和）；**TM 必须先于其 IO 事件回调终结**（EXEC-07 形态）：flush() 在泵静止
后等待 IO 在飞归零（有界预算），析构兜底 `request_stop` + 有界等待——组合根
关闭序为「TM flush（含 IO 归零）→ … → owner 步骤 2/3 回收 worker（join）→
TM 析构」。

**④ TransferId 生成入口定案**（DEC-010 风险⑦ 的 M4-04 登记项）：新生成
TransferId 一律以规范形式生成——16 随机字节（`std::random_device`，全零重抽）
→ `heyaki::to_string(TransferId)`（`hyt1_` + 26 base32，按构造规范）；生成入口
`NodeSession::new_transfer_id()`（aki/std 公开面）。ad-hoc 串（如测试用
`t-1`）在真实 Adapter `push_file` 转换与 codec/DEC-010 谓词处被拒（Fake 沿
参数化接受不受影响，仅约束真实链路）。

## 备选方案

（调研否决项，全文见冻结调研记录；摘要）

- **§7.1③ 字面形态**（池上会话长任务内联写 + sleep 切片）＝现行骨架——池
  worker 停占，M4-03 观察③ CI 实证否决（916s 级联）。
- **逐块委托 + 池上阻塞等待**（session 任务委托 worker 后原地等 future）——
  等待期间仍占池 worker；executor 明示 worker 上等待可耗尽线程池挂死
  shutdown。
- **分块作业进 DatabaseWorker 通道**——M4-01 已否决（长 IO 占位通道、破坏
  64×2≤256 与 2s drain 预算），维持否决。
- **分块作为池上有限任务自链续接**——MiB 级阻塞文件 IO 跑池 worker 违反
  EXEC-04 明文与 AGENTS 规则 5。
- **整文件单长作业**——GB 级分钟占位、暂停/取消粒度不可表达、head-of-line。
- **进度自驱周期采样**（`submit_periodic`/`TimerHandle`）——违反 M4 计划风险
  条款「优先以 on_transfer_progress 事件驱动，不引入自驱周期负载」。
- **复用 heyaki 的 `heyaki-asio-file-io` worker**——heyaki 内部 worker 非公开
  承载面（DEC-006 契约权威仅公开头文件），不可注入。
- **保留 `submit_cancellable` 会话任务但纯协调不睡眠**——无事可等（进度已
  事件驱动入泵），空转句柄无消费者价值。

## 影响与风险

- **公开契约修订**（M1-08 纪律先行落地）：设计 §7.1①③ 重写（本决策）、§8.3
  （传输会话承载 + transfer IO worker 关闭纪律一句）、§11.1③（worker 复用
  DatabaseWorker 关闭纪律一句）、总计划 `EXEC-05` 对传输的适用面收窄（重连
  循环仍适用；传输取消经 worker StopToken + 会话控制位）。
- **M4-02 组件删除**：`transfer/manager/transfer_session_manager.hpp` 与其测试
  二进制移除（未接入 app 栈、语义被收口组件取代）；M4-02 验证记录保持历史
  原样。`app/application/transfer_manager.hpp` 公开观测面变化：会话不再有
  executor 任务/future（`active_session_count` 语义改为会话表规模，
  executor 取消计数断言不再适用——相关测试断言随新语义更新）。
- **`FileMetadata.stored_sha256` 为 wire+内存字段**：随消息载荷（DEC-010
  字段 5）与内存态携带；message 表无对应列——消息行重启重建时该字段为空
  （传输行 `stored_*` 回写列是持久权威），M4-06 回环断言须按此口径；如需
  持久化消息侧哈希须另立 schema 变更（登记，不在本决策内）。
- **实现时必须验证**：分块边界（空文件/非整块尾部/多块）；进度节流（每次
  排空至多一个 UpdateTransferProgress）；终态闸门顺序（归档先于/后于 wire
  committed 两序 + 归档失败释放已持终态的已知边角——DB 作业组对不完整
  `.part` 明确失败可见）；DOD-02 六项沿真实 IO 路径；2 worker 小池夹具验证
  Manager 泵恒可调度（M4-03 观察③ 闭环）；重启后传输行与文件本体一致
  （完整 DB 组合）；hash-first 大文件启动延迟如实登记（本机实测数量级）。
- **回调生命周期**：TM 先于 IO 回调终结的纪律由 flush/idle 等待 + 析构兜底
  request_stop 承载；违反关闭序的组合根属装配缺陷（同 M4-02 会话任务纪律）。
- 本调研为纯静态证据（未编译未运行任何构建/测试）；动态验证归 M4-04 实现。

## 验证方式

本记录依据 2026-09-26 冻结调研（负责人 Linductor；pinned executor
`blocking_io.hpp`/`executor.hpp`/`config.hpp`/集成指南 blocking-io 卡、heyaki
`node.hpp`/`file.hpp` 公开头、仓库内 DatabaseWorker/FileStore/file_jobs/
main.cpp/test_app_managers 逐文件静态核实）。动态验证归 M4-04：网络无关单测
（分块边界/节流/终态闸门/DOD-02 六项/2-worker 停占夹具/重启一致性）+
debug/release 全量 ctest 零回归 + ASAN/UBSAN/TSAN 随 PR CI；回环沿 M3/M4-03
降级纪律。证据记录于 [M4 里程碑文档](../plans/m4-image-file-transfer.md)
M4-04 验证记录。

## 关联文档和工作项

- [Aki 设计方案](../design/aki_design.md)§7.1①③④⑤（本决策的应用侧集成契约
  权威落点）、§8.3（Manager 承载与关闭序）、§11.1③④（worker 关闭纪律/
  终态作业组）、§6.1②（发送侧闸门——hash-first 排序补入）
- [DEC-004](DEC-004-local-persistence-sqlite.md)（.part 布局/终态作业组/
  FileStore）、[DEC-006](DEC-006-heyaki-api-contract.md)（映射 7：push_file
  root 注入与事件面——wire 语义不变）、[DEC-008](DEC-008-manager-routing-and-executor-tasks.md)
  （单飞泵/单飞投影/取消纪律）、[DEC-009](DEC-009-appstate-write-path.md)
  （DB 通道预算——批上限维持）、[DEC-010](DEC-010-image-message-contract.md)
  （字段 5 stored_sha256 + TransferId 生成入口定案）
- [Aki 实施总计划](../plans/aki-implementation-plan.md)（`SCOPE-08`、`EXEC-04`/
  `EXEC-05` 适用面修订、M4）
- [M4：图片消息与文件传输](../plans/m4-image-file-transfer.md)（`M4-04` 实现、
  M4-03 验证记录观察③ 收口）
