# Aki 实施总计划

> 状态：Active
> 负责人：Linductor
> 更新日期：2026-09-21
> 设计依据：[Aki 设计方案](../design/aki_design.md)
> 协作约束：[AGENTS.md](../../AGENTS.md)、[项目管理与工程规范](../project/project-standards.md)

## 当前状态

- 2026-09-21：项目完成初始化（M0 工程骨架与协作基线）。M1 及后续里程碑均为
  `Planned`，尚未开始功能开发。
- 2026-09-21：executor、EUI-NEO、heyaki 三个依赖已完成 submodule + 锁文件登记与
  configure 校验，[DEC-003](../decisions/DEC-003-dependency-locking.md) 冻结为
  `Accepted`，`RISK-2026-001` 解除。M0 剩余：CI 基线（首次 push 后）与提交前
  `git status` 复核；M1 的依赖前置已就绪。

## 交付边界

### 包含（第一阶段 / MVP，对应设计第 15 节）

- [ ] `SCOPE-01` 设备身份：设备作为独立通信主体，持有 Heyaki 密码学身份，无用户账户层。
- [ ] `SCOPE-02` 设备发现：局域网发现、已知设备记录、Relay、邀请链接、手动输入统一为
  `DiscoveredDevice`。
- [ ] `SCOPE-03` 信任建立：`Unknown -> Pending -> Trusted / Rejected / Revoked` 流程与
  公钥指纹确认。
- [ ] `SCOPE-04` 设备列表：名称、类型、操作系统、连接方式与在线状态展示。
- [ ] `SCOPE-05` 一对一 Conversation：路径无关的会话模型与 Conversation List。
- [ ] `SCOPE-06` 一对一文本消息：发送、接收、送达状态。
- [ ] `SCOPE-07` 图片消息。
- [ ] `SCOPE-08` 文件传输：独立 Transfer Session、传输进度、暂停/取消，Transfers 页面。
- [ ] `SCOPE-09` 本地持久化：设备、信任、会话、消息与传输历史；重启后可恢复。
- [ ] `SCOPE-10` Presence 与连接路径展示（LAN / P2P / Relay）。
- [ ] `SCOPE-11` 断线恢复：网络中断恢复后原 Conversation 继续可用。
- [ ] `SCOPE-12` EUI-NEO 主窗口与基础主题：三栏布局，导航含 Conversations、Devices、
  Transfers、Settings。

### 明确不包含（第一阶段，已在 2026-09-21 初始化时依据设计第 15/16 节确认）

- [x] `SCOPE-13` 语音消息、语音通话、视频通话。
- [x] `SCOPE-14` 远程终端、屏幕控制。
- [x] `SCOPE-15` 设备命令（`Command` / `CommandResult`）与设备状态查询。
- [x] `SCOPE-16` Agent 消息与 Mira 接入（协议与 capability 预留，不交付）。
- [x] `SCOPE-17` 传统 User -> Device 账户模型与中心账户。

## 不可破坏的架构约束

- `RULE-01` 分层与依赖方向固定为 EUI-NEO UI -> Application -> Domain -> Heyaki Adapter ->
  Heyaki；Adapter 依赖 Core 抽象，禁止反向依赖；页面代码不得持有 transport 对象
  （设计第 8/14 节）。
- `RULE-02` 状态边界：Heyaki 异步事件只能进入对应 Manager 并更新 Application State；
  网络线程不得直接修改 UI，UI 只消费状态变化（设计第 10 节）。
- `RULE-03` Typed message：消息自第一版起使用类型化模型；新增消息类型必须先更新设计与
  协议说明（设计第 6 节）。
- `RULE-04` 信任与权限分离：`Trusted` 只表示身份信任；任何能力使用必须经过 capability
  声明与 permission 判定（设计第 4/12 节）。
- `RULE-05` 文件数据与消息分离：会话内只保存文件 metadata 与 TransferId，文件本体经
  独立 Transfer Session 传输与存储（设计第 7/11 节）。
- `RULE-06` Conversation 与网络路径解耦：LAN / Internet P2P / Relay 之间切换不创建新
  会话、不改变消息历史（设计第 5 节）。
- `RULE-07` 并发与生命周期：所有任务经 pinned `third_party/executor` 管理；禁止
  `std::thread`、`std::jthread`、`std::async`、自建线程池与 detached 任务（AGENTS.md）。
- `RULE-08` 显式状态机与幂等终态：Task/Session 有稳定 ID、取消上下文与生命周期所有者；
  迟到响应不得复活已取消或已完成的任务。
- `RULE-09` 有界资源：所有队列、缓存与并发 operation 有容量或预算上限；背压、拒绝、
  超时转化为明确结果与事件，不静默重试或吞掉失败。
- `RULE-10` 公开 API 不暴露平台与第三方类型（EUI-NEO、Heyaki、SQLite 类型不得出现在
  Core 公开头文件）。
- `RULE-11` 声明纪律：不宣称未通过目标平台或基准验证的实时性、性能或跨平台保证。

## Executor 并发边界

- `EXEC-01` 生命周期 owner：Executor 由 `app/lifecycle` 唯一初始化与关闭。关闭顺序：停止
  任务生产者 -> 发出取消/停止请求 -> 回收 blocking/实时 worker -> 等待需完成的有限任务 ->
  非 worker 线程执行 `shutdown(true)`。
- `EXEC-02` Heyaki 回调隔离：Heyaki/平台回调只在原线程做有界校验与投递（`MpscChannel`
  或 `Topic`），业务 handler 一律在 Manager 的执行上下文运行。
- `EXEC-03` 状态通信：Application State 为单写者（Manager 侧）；跨上下文最新状态用
  `LatestMailbox`，UI 消费的一致快照用 `DoubleBuffer`，事件广播用 `Topic`；禁止 ad-hoc
  队列与“共享可变状态 + mutex + 条件变量”。
- `EXEC-04` 任务承载：有限任务用 `submit_auto()` 并消费 future；文件 I/O 与历史读写用
  blocking worker 生命周期；presence 刷新、传输进度采样等允许抖动的周期任务用
  `submit_delayed`/`submit_periodic` + `TimerHandle`；第一阶段无固定周期/低延迟控制需求，
  不使用 realtime 能力，如出现先按工程规范 9.4 评估。
- `EXEC-05` 取消：传输、重连循环等长任务用 `submit_cancellable` + `StopToken` 协作取消；
  等待、网络与平台动作必须具备可解除阻塞路径；排队期与运行期取消对 Executor 可见。
- `EXEC-06` 可观测性：admission 拒绝、执行失败、超时/取消、背压与关闭状态经 Executor
  监控设施与 `executor::comm` 统计观察，不建平行任务监控。
- `EXEC-07` 句柄所有权：future、`TimerHandle`、worker 句柄由各 Manager 显式持有；依赖经
  构造参数或明确 context 传递，不隐藏全局 Executor 生命周期。

## 里程碑索引

| 里程碑 | 名称 | 状态 | 前置 | 建议发布点 | 文档 |
| --- | --- | --- | --- | --- | --- |
| M0 | 工程骨架与协作基线 | In Progress | 无 | 无（仓库基线） | [m0-project-skeleton.md](m0-project-skeleton.md) |
| M1 | 领域模型与状态边界 | Planned | M0（依赖来源解锁） | v0.1.0 | [m1-domain-state.md](m1-domain-state.md) |
| M2 | 本地持久化 | Planned | M1 | v0.2.0 | 待创建 |
| M3 | Heyaki 真实接入与文本消息 | Planned | M1、M2、DEC-006 | v0.3.0 | 待创建 |
| M4 | 图片消息与文件传输 | Planned | M3 | v0.4.0 | 待创建 |
| M5 | EUI-NEO UI 与 MVP 验收 | Planned | M2、M3、M4、DEC-005 | v0.5.0（MVP） | 待创建 |

依赖说明：M1 先以契约与假实现交付可运行的领域骨架（先契约后实现、先假实现后真实依赖）；
M3 引入真实 Heyaki；M5 整合 UI 并按设计第 15 节逐项验收 MVP。每个里程碑必须产生可独立
验收的能力增量，对应 tag 与里程碑文档“建议发布点”一一对应。

## 尚未冻结的决策（暂定默认值）

| 编号 | 主题 | 暂定默认值 | 负责人 | 最迟冻结里程碑 |
| --- | --- | --- | --- | --- |
| [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md) | 本地存储采用 SQLite | SQLite C API + 薄封装，不用 ORM | Linductor | M2 开始前 |
| `DEC-005` | EUI-NEO 集成方式 | 源码/子模块引入 + CMake target，不用 WebView | Linductor | M5 开始前 |
| `DEC-006` | Heyaki API 契约版本 | 以 M3 启动时 pinned 版本公开 API 为准 | Linductor | M3 开始前 |
| `DEC-007` | 单元测试框架 | M1 起引入（暂定 Catch2 v3），M0 用无框架 smoke | Linductor | M1 开始前 |

`DEC-005` 至 `DEC-007` 在冻结时创建正式决策记录文件；已生效决策见
[docs/decisions/](../decisions/)（含已冻结的 [DEC-003](../decisions/DEC-003-dependency-locking.md)）。

## 跨里程碑通用完成定义

- `DOD-01` 实现符合 `RULE-NN` 分层、所有权、取消、错误与 `EXEC-NN` Executor 生命周期约束。
- `DOD-02` 新增或变更行为有与风险相称的自动化测试；新增并发路径至少覆盖：正常完成、
  任务异常、提交拒绝、执行中取消、超时、shutdown。
- `DOD-03` 在声明支持的平台上，debug 构建与测试通过，ASAN/UBSAN 常规运行；涉及跨上下文
  状态或关闭的变更增加 TSAN/故障注入；不适用的平台记录限制与补跑条件。
- `DOD-04` 公开 API、事件 schema、错误语义变更已同步设计、决策、计划与示例。
- `DOD-05` 里程碑状态、工作项勾选与验证记录已更新；未执行验证保持未勾选，并记录原因、
  负责人与补跑条件。
- `DOD-06` Commit 与 MR 符合工程规范第 10 节；无被吞掉的失败、无主异步任务或未登记的
  Executor 能力绕行。

## 拆分与合并顺序

1. 先骨架后功能：M0 建立可构建、可测试的仓库骨架，再进入领域实现。
2. 先契约后实现：M1 先固定 Heyaki Adapter SPI、事件模型与状态机，再填实现。
3. 先假实现后真实依赖：M1 用 FakeHeyakiAdapter 打通闭环，M3 才接入真实 Heyaki。

## 延后项与触发条件

- `POST-01` 语音消息、语音通话、视频通话：MVP 稳定运行且 Heyaki 提供相应传输能力后立项
  （设计第 16 节）。
- `POST-02` 设备状态查询、Clipboard、Remote Shell、Screen、Remote Control：capability/
  permission 模型在 MVP 中得到验证且出现真实需求后立项。
- `POST-03` Agent 消息与 Mira 接入：typed message 与 capability 模型稳定、Agent 场景有
  明确需求后立项。
- `POST-04` 群组/多设备会话：一对一模型稳定且出现产品需求后立项。
- `POST-05` Android/移动端：桌面 MVP 验收且 Heyaki 提供对应平台支持矩阵后立项。

## 风险与阻塞

| 编号 | 状态 | 风险/阻塞 | 影响 | 负责人 | 解除条件 |
| --- | --- | --- | --- | --- | --- |
| `RISK-2026-001` | Resolved (2026-09-21) | executor / EUI-NEO / heyaki 来源与 pinned commit 未定 | 曾阻塞 M1 并发代码、M3、M5 | Linductor | 已解除：[DEC-003](../decisions/DEC-003-dependency-locking.md) 冻结为 Accepted，submodule + 锁文件校验通过 |
| `RISK-2026-002` | Open | EUI-NEO 组件能力与三栏布局匹配度未验证 | M5 范围可能调整 | Linductor | M5 开始前完成组件能力盘点 |
| `RISK-2026-003` | Open | 本机 Windows/MinGW 工具链对 sanitizer 支持有限 | TSAN/部分 ASAN 证据缺失 | Linductor | 建立 Linux CI 门禁（远程仓库已配置 GitHub，首次 push 后补 workflow） |
