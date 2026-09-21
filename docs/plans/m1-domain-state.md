# M1：领域模型与状态边界

> 状态：In Progress
> 负责人：Linductor
> 所属计划：[Aki 实施总计划](aki-implementation-plan.md)
> 前置：M0（依赖已就绪：executor 已 pin 并通过校验，见
> [DEC-003](../decisions/DEC-003-dependency-locking.md)；M0 关闭后启动本里程碑）
> 建议发布点：v0.1.0
> 更新日期：2026-09-21

## 目标

建立可执行、可测试的领域骨架：固定 Device / Conversation / Message / Transfer 的领域
类型与状态机、Application State 状态边界、Heyaki Adapter SPI 与事件模型，并以
FakeHeyakiAdapter 打通“发现 -> 信任 -> 文本消息 -> 断开 -> 重连”的进程内闭环。遵循
“先契约后实现、先假实现后真实依赖”：本里程碑不接触真实网络与持久化。

## 范围与非目标

### 范围

- 领域类型与显式状态机（`TrustState`、`PresenceState`、`ConversationState`、
  `DeliveryState`、`TransferState`，设计第 3~7 节），含非法转换拒绝与终态幂等。
- Application State 单写者边界与设计第 10 节事件模型的 C++ 表示。
- Heyaki Adapter SPI（C++ 抽象接口）与 `FakeHeyakiAdapter`（可注入发现/连接/消息事件）。
- Executor 生命周期 owner（`app/lifecycle`）与关闭顺序实现（`EXEC-01`）。
- Device / Conversation / Message / Transfer Manager 骨架与协作取消路径。
- 无 UI 冒烟宿主（console demo）作为本里程碑的可验收交付。
- 测试框架选型落地（`DEC-007`）与测试标签（`unit` / `integration`）。

### 非目标

- 真实 Heyaki 接入（M3）、持久化实现（M2）、EUI-NEO UI（M5）。
- 图片消息与文件传输的真实数据链路（M4），仅保留 `TransferState` 状态机骨架。

## 设计与决策依据

- [Aki 设计方案](../design/aki_design.md)第 3~7、10、14 节。
- [DEC-001](../decisions/DEC-001-cpp-baseline.md)：构建与测试基线。
- [DEC-002](../decisions/DEC-002-layering-and-state-boundary.md)：分层、状态边界与 SPI 先行。
- AGENTS.md Executor 强制规则与工程规范第 9 节；实现前按 pinned executor 的集成指南
  加载对应 capability card。

## 工作项

- [x] `M1-01` 提供设计第 3~7 节领域类型与状态机的 C++ 实现，覆盖非法状态转换拒绝与
  终态幂等测试。（2026-09-21：`device/trust/trust_state.hpp`、
  `device/device/device_types.hpp`、`device/discovery/discovery_types.hpp`、
  `conversation/conversation/conversation_types.hpp`、`conversation/message/message_types.hpp`、
  `transfer/transfer/transfer_types.hpp` 及四个状态机单测；设计第 3~6 节先补充了
  PresenceState / 信任转移 / ConversationState / DeliveryState 枚举，见 `M1-08` 记录。）
- [ ] `M1-02` 提供 Application State 单写者边界与设计第 10 节事件模型实现，跨上下文
  通信落点对应 `executor::comm` 组件选型（`EXEC-03`）。
- [ ] `M1-03` 提供 Heyaki Adapter SPI 抽象接口与 `FakeHeyakiAdapter`，支持注入发现、
  连接路径变化与消息事件。
- [ ] `M1-04` 提供 `app/lifecycle` 的 Executor 初始化/关闭 owner 实现并满足 `EXEC-01`
  关闭顺序，附 shutdown 测试。
- [ ] `M1-05` 提供 Device / Conversation / Message / Transfer Manager 骨架，任务全部经
  Executor 承载并具备协作式取消路径（`EXEC-04`、`EXEC-05`）。
- [ ] `M1-06` 提供 console 冒烟宿主：两台假设备完成“发现 -> 信任 -> 文本消息 -> 断开 ->
  重连”演示，作为 v0.1.0 验收载体。
- [x] `M1-07` 落地 `DEC-007` 测试框架与 `unit`/`integration` 标签，CI 可按标签选择
  执行集。（2026-09-21：[DEC-007](../decisions/DEC-007-test-framework.md) 冻结为
  Accepted，Catch2 v3.9.1 经 FetchContent 锁 commit 接入；`unit`/`smoke` 标签生效，
  `integration` 标签随 `M1-06` 冒烟宿主启用。CI 已于 2026-09-22 建立（PR #1），全量
  ctest 进入门禁。）
- [ ] `M1-08` 校对实现与设计偏差：SPI、事件命名或状态集若与设计不一致，先更新设计或
  新增决策，再合入代码。

## 风险与阻塞

- ~~`RISK-2026-001`：executor 来源未解锁~~ 已解除（2026-09-21）：executor 已 pin 于
  `74a9419` 并通过 configure 校验，`M1-01`~`M1-06` 可正常启动。
- Application State 的快照/订阅语义（`DoubleBuffer` 与 `LatestMailbox` 的具体落点）需在
  `M1-02` 设计时对照 pinned executor 文档确认，必要时补充设计小节。

## 测试与退出条件

- [ ] 退出-1：`integration` 冒烟——两台假设备经 FakeHeyakiAdapter 完成发现、信任、文本
  消息收发、断开与重连，断言消息顺序与状态转换。
- [ ] 退出-2：并发基线六项测试通过——正常完成、任务异常、提交拒绝、执行中取消、超时、
  shutdown（AGENTS.md 工程约束）。
- [ ] 退出-3：状态机单测覆盖合法/非法转换与终态幂等；迟到事件不得复活已取消任务。
- [ ] 退出-4：debug 构建与全量测试通过；ASAN/UBSAN 通过（声明支持的平台），不适用的
  工具链记录限制与补跑条件。
- [ ] 退出-5：设计、决策与总计划状态同步；验证记录含可复现命令与结果。

## 验证记录

- 2026-09-21（`M1-01`、`M1-07`，Windows 11 / MinGW w64devkit GCC 15.2.0 / CMake 4.1.0，
  commit 见 git 历史）：
  - `cmake --preset debug && cmake --build --preset debug && ctest --preset debug`
    → 6/6 通过（4 个状态机单测共 17 个 test case / 193 断言 + 1 smoke）。
  - `cmake --preset release && cmake --build --preset release && ctest --preset release`
    → 6/6 通过。
  - asan preset：configure 失败。证据：w64devkit GCC 15.2 工具链未随附 sanitizer
    运行时（链接报 `cannot find -lasan`，库目录中无 libasan/libubsan/libtsan）。
    限制与补跑条件：ASAN/UBSAN/TSAN 证据待 Linux CI 门禁建立后补跑
    （对应总计划 `RISK-2026-003`，退出-4 在本机保持未勾选）。
  - 备注：测试期间修正一处测试自身缺陷（对 `Failed` 终态误断言 `Failed -> Failed`
    必须失败；该转移是终态幂等 no-op，应返回 true）。

- 2026-09-22（PR #1 CI 证据，M1-07 补验）：
  - CI 门禁建立：`.github/workflows/ci.yml`（Linux debug/asan/ubsan + Windows MSVC
    debug），PR #1 首轮 4/4 全绿，含此前本机缺失的 ASAN/UBSAN 证据（`RISK-2026-003`
    解除）。
  - MSVC 适配修复：`/utf-8`（本地 VS 2022 BuildTools Debug 构建 + ctest 6/6 通过）。
  - M1-07 的"CI 按标签执行集"已可验证：CI 运行全量 ctest，`unit`/`smoke` 标签生效。
  - PR #1 squash 合入 `master` @ `9ad1eb7`，工作分支已清理，本地 master 已同步。
