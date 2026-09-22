# M1：领域模型与状态边界

> 状态：Completed（2026-09-23 收口审计通过，见 M1-08 验证记录；v0.1.0 发布点就绪，
> tag 创建待用户授权）
> 负责人：Linductor
> 所属计划：[Aki 实施总计划](aki-implementation-plan.md)
> 前置：M0（依赖已就绪：executor 已 pin 并通过校验，见
> [DEC-003](../decisions/DEC-003-dependency-locking.md)；M0 关闭后启动本里程碑）
> 建议发布点：v0.1.0
> 更新日期：2026-09-23

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
- [x] `M1-02` 提供 Application State 单写者边界与设计第 10 节事件模型实现，跨上下文
  通信落点对应 `executor::comm` 组件选型（`EXEC-03`）。（2026-09-22：
  `app/state/app_state.hpp`（四 Store + 容量预算）、`app/state/app_events.hpp`
  （9 类类型化事件 + owner 单写者序列号）、`app/state/app_state_updates.hpp`（经
  MpscChannel 汇聚的更新指令）、`app/state/app_state_owner.hpp`（单写者 owner：
  DoubleBuffer 快照 / LatestMailbox 连接路径 / Topic 观察者 / 必达事件主路径）；
  pinned executor 库经根 `CMakeLists.txt` 完成目标级接入；设计第 10.1 节补充 comm
  语义映射；单测 `test_app_state`（13 test case / 168 断言）覆盖验收 ①②④，
  详见验证记录。）
- [x] `M1-03` 提供 Heyaki Adapter SPI 抽象接口与 `FakeHeyakiAdapter`，支持注入发现、
  连接路径变化与消息事件。（2026-09-22：`heyaki/adapter/heyaki_adapter.hpp`
  （`HeyakiAdapter` 出站 + `HeyakiAdapterSink` 入站，9 个 Sink 方法与设计第 10 节
  9 类事件一一对应，仅依赖第 3~7 节领域类型，`RULE-01`/`RULE-10`）、
  `heyaki/adapter/fake_heyaki_adapter.hpp`（`inject_*` 编程式注入，`EXEC-02`
  有界校验 + 投递，结果经返回值可见）；设计第 8.1 节先行固化 SPI 契约；
  单测 `test_heyaki_adapter`（11 test case / 116 断言）覆盖验收 ①② 与 DOD-02
  六项；`heyaki/events` 未引入——Heyaki 原生事件类型随 M3 `DEC-006` 落地。）
- [x] `M1-04` 提供 `app/lifecycle` 的 Executor 初始化/关闭 owner 实现并满足 `EXEC-01`
  关闭顺序，附 shutdown 测试。（2026-09-22：`app/lifecycle/executor_owner.hpp`
  ——`ExecutorOwner` 独立实例持有 pinned executor `Executor` facade（非单例），
  `EXEC-01` 五步逐一注释对应（停生产者钩子 → blocking worker `request_stop` →
  `stop` 回收 → `wait_for_completion_ex(owner 预算)` → 非 worker 线程
  `shutdown(true)`），`ExecutorShutdownReport` 以 `Completed` +
  `lifecycle==Stopped` + `wait_timeout_count==0` 作为可观察关闭证据；blocking
  worker 句柄由 owner 持有（M1-05/M2 预留）；设计第 8.2 节先行固化契约与唯一
  owner 纪律落点；单测 `test_executor_lifecycle`（8 test case / 58 断言）覆盖
  五类 shutdown 断言与 DOD-02 六项。）
- [x] `M1-05` 提供 Device / Conversation / Message / Transfer Manager 骨架，任务全部经
  Executor 承载并具备协作式取消路径（`EXEC-04`、`EXEC-05`）。（2026-09-22：
  `app/application/`——`manager_runtime.hpp`（单飞有界排空泵：`MpscChannel` 收件箱 +
  CAS 单飞 + `submit_auto` 排空任务，保留并消费 future，释放后复查收件箱防丢失
  唤醒；排队软超时/提交即拒经消费 future 自愈重排）、四 Manager（各只写本域
  Store，事件与宿主命令经收件箱在 Manager 执行上下文串行处理）、`router_sink.hpp`
  （9 类事件按 `DEC-008` 路由表路由，connected/disconnected 双 Manager 扇出、主路径
  事件各投递一次）、`transfer_manager.hpp` 会话任务 `submit_cancellable` + StopToken
  （TaskHandle 按 TransferId 由 Manager 持有，取消经 `request_task_cancel`，M1 无
  TimerHandle/周期任务）；`app/state/app_state_updates.hpp` 新增 `SetPresence`/
  `SetDeliveryState`/`CompleteTransfer`（owner 状态机校验、终态幂等、未知 id 拒绝）；
  设计第 8.3 节先行固化 Manager 契约 + 第 10.1 节更新指令清单 + 第 8.2 节 blocking
  worker/TimerHandle 措辞修正；`DEC-008` 冻结 Accepted；单测 `test_app_managers`
  （9 test case / 295 断言）覆盖 DOD-02 六项（Manager 任务路径）与迟到事件不复活
  终态（RULE-08）；计数以 PR #8 合入版为准：10 test case / 314 断言（评审中
  补充重复 TransferId 防线用例，原记录 9/295，M1-08 审计按工程规范 6.1 修正）。
  详见验证记录。）
- [x] `M1-06` 提供 console 冒烟宿主：两台假设备完成“发现 -> 信任 -> 文本消息 -> 断开 ->
  重连”演示，作为 v0.1.0 验收载体。（2026-09-22：根 `main.cpp` 按设计第 8.3 节
  装配顺序实现组合根——`ExecutorOwner.initialize()` → `AppStateOwner` → 四
  Manager（构造注入 executor/owner/adapter 与容量预算）→ `RouterSink` 经
  `FakeHeyakiAdapter::set_sink` 注册；进程内 owner 自本项起为正式
  `ExecutorOwner`；设备信任确认为用户流程（设计第 4 节），M1 无 Trust Manager/UI，
  由宿主经 `UpsertDevice` 更新指令模拟（第 8.3 节先行补充该语义，M1-08 纪律）；
  每步 flush + owner drain 推进到静止后经 DoubleBuffer 快照与序列号排序的主路径
  事件逐步断言；受控关闭按 §8.3 钩子顺序（request_cancel_all → flush →
  set_sink(nullptr)+stop_discovery → close()）以 `fully_stopped` 证据收尾；
  ctest 注册 `smoke.device_lifecycle`（`integration` 标签自本项启用，覆盖
  退出-1），`skeleton.app_runs` 断言保持兼容（宿主保留 `aki 0.1.0` 横幅）。
  详见验证记录。）
- [x] `M1-07` 落地 `DEC-007` 测试框架与 `unit`/`integration` 标签，CI 可按标签选择
  执行集。（2026-09-21：[DEC-007](../decisions/DEC-007-test-framework.md) 冻结为
  Accepted，Catch2 v3.9.1 经 FetchContent 锁 commit 接入；`unit`/`smoke` 标签生效，
  `integration` 标签随 `M1-06` 冒烟宿主启用。CI 已于 2026-09-22 建立（PR #1），全量
  ctest 进入门禁。）
- [x] `M1-08` 校对实现与设计偏差：SPI、事件命名或状态集若与设计不一致，先更新设计或
  新增决策，再合入代码。（2026-09-23：M1 收口审计完成——逐项校对矩阵覆盖设计
  第 3~7/8.1/8.2/8.3/10/10.1/14 节对 `device/`、`conversation/`、`transfer/`、
  `heyaki/adapter/`、`app/lifecycle/`、`app/application/`、`app/state/` 与根
  `main.cpp`，结论全部一致；四项已知偏差（设计 3~6 节枚举补充、`heyaki/events`
  延至 M3、`transfer/manager/` 留 M4 与 blocking worker/TimerHandle 措辞、冒烟
  宿主信任流经 `UpsertDevice` 模拟）均有设计小节/工作项/决策记录锚点，未记录
  偏差数为 0；RULE-01/07/10 边界抽查 grep 证据通过；一项事实修正：PR #8 合入
  版本在评审中补充了重复 TransferId 防线用例，`test_app_managers` 实为
  10 test case / 314 断言（记录原为 9/295，按工程规范 6.1 修正）。详见验证记录。）

## 风险与阻塞

- ~~`RISK-2026-001`：executor 来源未解锁~~ 已解除（2026-09-21）：executor 已 pin 于
  `74a9419` 并通过 configure 校验，`M1-01`~`M1-06` 可正常启动。
- ~~Application State 的快照/订阅语义（`DoubleBuffer` 与 `LatestMailbox` 的具体落点）需在
  `M1-02` 设计时对照 pinned executor 文档确认，必要时补充设计小节。~~ 已解除
  （2026-09-22，`M1-02`）：落点对照 pinned 版本头文件与集成指南确认并固化为
  [设计第 10.1 节](../design/aki_design.md)——`DoubleBuffer<AppState>`（SWMR，更新经
  `MpscChannel` 汇聚到单一状态 owner）、`LatestMailbox<ConnectionPath>`（单值最新状态）、
  `Topic<AppEventPtr>`（可容忍丢失的观察者广播）；9 类必达事件主路径维持
  `MpscChannel<AppEvent>`（`EXEC-02`）。

## 测试与退出条件

- [x] 退出-1：`integration` 冒烟——两台假设备经 FakeHeyakiAdapter 完成发现、信任、文本
  消息收发、断开与重连，断言消息顺序与状态转换。（2026-09-22：`M1-06`
  `smoke.device_lifecycle` 通过，验证记录含覆盖映射与可复现命令。）
- [x] 退出-2：并发基线六项测试通过——正常完成、任务异常、提交拒绝、执行中取消、超时、
  shutdown（AGENTS.md 工程约束）。（2026-09-23：六项沿四条并发路径覆盖——comm 通道
  （`test_app_state`，7 个 dod02 用例）、Adapter 注入管线（`test_heyaki_adapter`，6）、
  owner 生命周期（`test_executor_lifecycle`，5）、Manager 任务路径（
  `test_app_managers`，8）；四个可执行文件 debug 全部通过，命令与映射见 M1-08
  验证记录。）
- [x] 退出-3：状态机单测覆盖合法/非法转换与终态幂等；迟到事件不得复活已取消任务。
  （2026-09-23：四个状态机单测通过（trust 64 / delivery 38 / conversation 18 /
  transfer 73 断言，含非法转移表、终态幂等与迟到事件用例）；Manager 路径的迟到
  事件拒绝见 `test_app_managers` 两条 RULE-08 用例与 `test_app_state` /
  `test_heyaki_adapter` 的 late-events 用例。）
- [x] 退出-4：debug 构建与全量测试通过；ASAN/UBSAN 通过（声明支持的平台），不适用的
  工具链记录限制与补跑条件。（2026-09-23：本地 MSVC debug/release 全量 ctest
  11/11 通过（本审计执行）；ASAN/UBSAN 由 Linux CI 提供——PR #8 与 PR #9 的
  `Linux / asan`、`Linux / ubsan`、`Linux / debug`、`Windows / debug (MSVC)`
  四项检查均 SUCCESS（gh 核实），覆盖合入 master 的全部 M1 代码；MinGW 默认
  生成器限制沿用 `M1-02` 验证记录限制 1。M1-08 本 PR 的 CI 复跑同一矩阵作为
  收口门禁。）
- [x] 退出-5：设计、决策与总计划状态同步；验证记录含可复现命令与结果。（2026-09-23：
  M1-08 审计矩阵逐项复核设计第 3~14 节与实现的同步；生效决策 DEC-001~004、
  DEC-007、DEC-008 与设计/计划交叉引用一致；各里程碑工作项验证记录均含可复现
  命令与结果。）

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

- 2026-09-22（`M1-02`，Windows 11 / MSVC 2022 BuildTools 14.44.35207 / CMake 4.1.0，
  工作树状态见 git 历史）：
  - 范围：`app/state/` 四个头文件（四 Store、9 类事件、更新指令、单写者 owner）、根
    `CMakeLists.txt` 接入 pinned executor 库（`EXCLUDE_FROM_ALL`，关闭其 tests/examples
    与 GPU 探测；executor include 目录标 SYSTEM，MSVC `/wd4324` 与 executor 自身豁免
    策略一致）、`tests/unit/test_app_state.cpp`（进程内唯一 Executor owner 为测试
    `main`，AGENTS 规则 7；正式 owner 由 `M1-04` 提供）。
  - 依据：[设计第 10/10.1 节](../design/aki_design.md)（10.1 为本次先行补充）、
    [DEC-002](../decisions/DEC-002-layering-and-state-boundary.md)、总计划
    `RULE-02`/`EXEC-02`/`EXEC-03`、AGENTS.md Executor 强制规则 4/7 与
    executor-integration communication 卡片。
  - 验证（`CMakePresets.json` 不锁定生成器，本机默认生成器为 MinGW Makefiles，因下述
    限制 1 以 MSVC 生成器等价执行，与 CI Windows job 同工具链）：
    - `cmake --preset debug -G "Visual Studio 17 2022" -A x64 &&
      cmake --build --preset debug --config Debug && ctest --preset debug -C Debug`
      → 7/7 通过（新 `test_app_state`：13 test case / 168 断言；同进程重复运行
      15/15 稳定）。
    - `cmake --preset release -G "Visual Studio 17 2022" -A x64 &&
      cmake --build --preset release --config Release && ctest --preset release -C Release`
      → 7/7 通过。
    - 覆盖映射（DOD-02，以 comm 关闭/排空为主）：正常完成（executor 任务投递 → drain →
      快照可见）；任务异常（future 异常 + `task_exception_count` 可见，已入队更新不丢）；
      提交拒绝（满通道 `dropped_count`、关闭后 `closed_send_count`）；执行中取消
      （`close()` 解除阻塞中的 `receive_for`，返回 `Closed`）；超时（空 outbox 与满
      inbox 的 `receive_for`/`send_for` 返回 `Timeout` 且计数可见）；shutdown
      （close 顺序排空存量、拒绝新工作、末次快照可读、订阅句柄已关闭、重复 close 幂等）。
      另覆盖验收 ④：迟到的进度/投递/信任/会话事件在状态机应用层被拒绝
      （`updates_rejected`），终态不复活。
  - 限制 1：pinned executor 库本体无法在 w64devkit MinGW GCC 15.2 构建——
    `cmake --preset debug && cmake --build --preset debug` 报
    `third_party/executor/src/executor/blocking_io_executor.cpp:157: error: invalid
    'static_cast' from type 'HANDLE' to type 'std::thread::native_handle_type'`。
    归因：pinned 版本在 MinGW 上的构建兼容缺陷（其 README 声明支持 Windows 但 CI 未
    覆盖 MinGW；非 Aki 分层/选型问题，非能力缺口，不进反馈台账；未经授权不修改
    `third_party/executor`）。影响范围：本地 MinGW 默认生成器构建；CI（Linux GCC +
    Windows MSVC）不受影响。负责人：Linductor。补跑条件：上游修复或经授权的最小
    兼容补丁后，以 MinGW 生成器重跑 debug/release 预设并记录证据。
  - 限制 2（沿用）：本机工具链无 sanitizer 运行时；`M1-02` 的 ASAN/UBSAN 证据随
    下次 PR 的 Linux CI 门禁提供。DOD-03 对跨上下文状态变更建议的 TSAN/故障注入
    尚无 CI job（预设已存在），补跑条件：CI 矩阵增加 tsan preset。
  - 同步：设计第 10.1 节、[DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)
    冻结 Accepted、总计划决策表与当前状态、本里程碑工作项与风险节。

- 2026-09-22（`M1-03`，Windows 11 / MSVC 2022 BuildTools 14.44.35207 / CMake 4.1.0）：
  - 范围：`heyaki/adapter/heyaki_adapter.hpp`（SPI 纯虚接口）、
    `heyaki/adapter/fake_heyaki_adapter.hpp`（假实现）、设计第 8.1 节（先行固化）、
    `tests/unit/test_heyaki_adapter.cpp`（自带 main 的 Executor owner，
    AGENTS 规则 7）、`tests/CMakeLists.txt` 接入；`heyaki/events` 不引入。
  - 依据：[设计第 3~8.1/10/10.1/14 节](../design/aki_design.md)（8.1 为本次先行
    补充）、[DEC-002](../decisions/DEC-002-layering-and-state-boundary.md)、
    [DEC-003](../decisions/DEC-003-dependency-locking.md)、总计划
    `RULE-01`/`RULE-03`/`RULE-08`/`RULE-09`/`RULE-10`/`EXEC-02`、
    executor-integration communication 卡片（本会话已加载）。
  - 验证（生成器说明同 `M1-02` 记录：MinGW 默认生成器受限制 1 阻塞，以 MSVC
    生成器等价执行）：
    - `cmake --preset debug -G "Visual Studio 17 2022" -A x64 &&
      cmake --build --preset debug --config Debug && ctest --preset debug -C Debug`
      → 8/8 通过（新 `test_heyaki_adapter`：11 test case / 116 断言；同进程重复
      运行 15/15 稳定）。
    - `cmake --preset release -G "Visual Studio 17 2022" -A x64 &&
      cmake --build --preset release --config Release && ctest --preset release -C Release`
      → 8/8 通过。
    - GCC 语法检查（CI Linux 告警姿势）：
      `g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -fsyntax-only`（含
      Catch2/executor include）通过——过程中修正两处 `uint64_t`/`int` 比较的
      sign-compare（MSVC /W4 未报，CI Linux 会 -Werror）。
    - RULE-10 证据（验收 ②）：`grep -n "#include" heyaki/adapter/*.hpp` 仅列出
      第 3~7 节领域头与 `<cstdint>`/`<string_view>`/标准库容器；
      `grep -rln "executor/" heyaki/adapter/` 无输出。
    - 覆盖映射：注入发现 → 快照出现新设备 + 主路径事件（正常完成）；注入连接路径
      变化 → `LatestMailbox` 更新、快照不发布（设计 10.1 单值摘要语义）；注入消息 →
      主路径 FIFO；终态/取消后注入迟到进度/信任 → 状态机应用层拒绝
      （`updates_rejected`，RULE-08）；DOD-02 六项沿注入管线——正常完成、任务异常
      （future 异常 + `task_exception_count`，已入队事件不丢）、提交拒绝（无 sink /
      空载荷 / 非终态 completed / inbox 满 / TransferId 重复启动与未知名控制）、
      执行中取消（close 解除阻塞消费）、超时（空 outbox `Timeout`）、shutdown
      （close 排空注入存量 → `Closed`，关闭后注入明确拒绝）。
  - 限制：MinGW 默认生成器仍受 `M1-02` 验证记录限制 1（pinned executor 构建缺陷）
    阻塞，未变化；ASAN/UBSAN 证据随本 PR 的 Linux CI 门禁提供；TSAN 沿用限制 2。
  - 同步：设计第 8.1 节、本里程碑工作项、总计划当前状态。

- 2026-09-22（`M1-04`，Windows 11 / MSVC 2022 BuildTools 14.44.35207 / CMake 4.1.0）：
  - 范围：`app/lifecycle/executor_owner.hpp`（`ExecutorOwner` + `ExecutorShutdownReport`）、
    设计第 8.2 节（先行固化：EXEC-01 五步映射、关闭证据、唯一 owner 纪律落点）、
    `tests/unit/test_executor_lifecycle.cpp`、`tests/CMakeLists.txt` 接入；
    M1-02/M1-03 测试 main 增加 owner 纪律落点说明（临时测试形态 → 正式
    `ExecutorOwner`，M1-06 起切换）。
  - 依据：总计划 `EXEC-01`/`EXEC-06`/`EXEC-07`、`RULE-07`/`RULE-08`、AGENTS.md
    规则 1/2/7/9/10；executor-integration SKILL 路由 → quick-start 与
    tasks-and-lifecycle 卡（本会话加载）；pinned executor 公开头文件核对
    （blocking_io.hpp `WorkerHandle::request_stop/stop`、types.hpp
    `WaitResult`/`ExecutorSnapshot.lifecycle`、config.hpp `enable_monitoring`、
    executor.hpp `wait_for_completion_ex`/`get_snapshot`/`start_worker`）。
    调研结论：EXEC-01 五步与库官方关闭协议一致，无能力缺口，不进 9.4 台账。
  - 验证（生成器说明同 `M1-02` 记录）：
    - `cmake --preset debug -G "Visual Studio 17 2022" -A x64 &&
      cmake --build --preset debug --config Debug && ctest --preset debug -C Debug`
      → 9/9 通过（新 `test_executor_lifecycle`：8 test case / 58 断言）。
    - `cmake --preset release -G "Visual Studio 17 2022" -A x64 &&
      cmake --build --preset release --config Release && ctest --preset release -C Release`
      → 9/9 通过。
    - GCC 语法检查（CI Linux 告警姿势）对 `test_executor_lifecycle.cpp` 通过
      （-Wall -Wextra -Wpedantic -Werror）。
    - 稳定性：`test_executor_lifecycle` 连续 100 次运行全部通过（修复两处偶发
      后：①任务失败计数相对 future 结算异步记账，测试改为有界轮询；
      ②`shutdown(true)` 返回后 lifecycle/失败计数快照异步收敛，owner 在 1s 预算
      内轮询至 `Stopped`，读不到即如实记录）。
    - 断言覆盖（验收 ③）：正常关闭排空存量（五步证据 `fully_stopped`）；关闭后
      提交明确失败（实测消息 "Async executor not initialized. Call initialize()
      first."，不静默）；重复 shutdown 幂等（返回同一报告）；owner 不可重复
      初始化、关闭后不可重建；blocking worker 注册 → `request_stop`+`wakeup`
      解除阻塞（耗时远小于 10s 等待上限，`wakeup_count>=1`）→ join；DOD-02：
      正常完成、任务异常（计数可见）、提交拒绝（关闭后）、执行中取消（阻塞
      等待被解除）、超时（owner 30ms 预算耗尽如实记录 `timed_out` 且
      `fully_stopped()==false`，`shutdown(true)` 仍完成）、shutdown（五步主体）。
  - 限制：MinGW 默认生成器仍受 `M1-02` 记录限制 1 阻塞；ASAN/UBSAN 随本 PR 的
    Linux CI 门禁提供；TSAN 沿用限制 2。
  - 同步：设计第 8.2 节、本里程碑工作项、总计划当前状态、M1-02/03 测试注释。

- 2026-09-22（`M1-05`，Windows 11 / MSVC 2022 BuildTools 14.44.35207 / CMake 4.1.0）：
  - 范围：`app/application/manager_runtime.hpp`（`ManagerPump` 单飞有界排空泵）、
    `device_manager.hpp` / `conversation_manager.hpp` / `message_manager.hpp` /
    `transfer_manager.hpp` / `router_sink.hpp`、`app/state/app_state_updates.hpp`
    （+`SetPresence`/`SetDeliveryState`/`CompleteTransfer`）、
    `app/state/app_state_owner.hpp`（+3 个 apply 分支：SetPresence 仅改 presence 不
    触发信任状态机、SetDeliveryState/CompleteTransfer 经状态机校验且未知 id 拒绝）、
    `tests/unit/test_app_managers.cpp` 与 `tests/CMakeLists.txt` 接入；设计第 8.3 节
    （先行固化 Manager 职责/路由/装配契约）、第 10.1 节（typed 更新指令清单）、
    第 8.2 节（blocking worker 改为"M1 不启用，M2 起按负载启用"、TimerHandle 子句
    标注 M1 为空操作）；[DEC-008](../decisions/DEC-008-manager-routing-and-executor-tasks.md)
    冻结 Accepted 并登记总计划决策表。`transfer/manager/` 目录按调研结论保留给
    M4 传输引擎内部，TransferManager 落位 `app/application/`。
  - 依据：[设计第 8/10/14 节](../design/aki_design.md)；[DEC-002](../decisions/DEC-002-layering-and-state-boundary.md)、
    [DEC-008](../decisions/DEC-008-manager-routing-and-executor-tasks.md)；总计划
    `RULE-02`/`RULE-07`~`RULE-09`、`EXEC-02`~`EXEC-07`、`DOD-02`/`DOD-03`；
    AGENTS.md Executor 规则 3/5/6/7/8；executor-integration 集成指南
    tasks-and-lifecycle / communication / observability / scheduling 卡（本会话按
    SKILL 路由加载）；pinned API 核对（executor.hpp `submit_cancellable`/
    `request_task_cancel`/`get_cancellation_status`、task_cancellation.hpp
    `TaskCancellationResponse`、types.hpp `TaskSubmission`/`TimedOutException`/
    `CapacityExhaustedException`/`CancellationStatus`、config.hpp
    `task_timeout_ms`/`max_in_flight_tasks`）。调研结论：取消/超时/句柄语义与
    EXEC-04~07 逐条吻合，无 Executor 能力缺口，不进工程规范 9.4 台账（M1 不启用
    blocking worker 与 TimerHandle 属延迟启用而非缺口）。
  - 验证（生成器说明同 `M1-02` 记录：MinGW 默认生成器受限制 1 阻塞，以 MSVC
    生成器等价执行）：
    - `cmake --preset debug -G "Visual Studio 17 2022" -A x64 &&
      cmake --build --preset debug --config Debug && ctest --preset debug -C Debug`
      → 10/10 通过（新 `test_app_managers`：本项交付时为 9 test case / 295 断言；
      PR #8 评审中补充重复 TransferId 防线用例，合入版为 10 test case / 314 断言，
      M1-08 审计实测）。
    - `cmake --preset release -G "Visual Studio 17 2022" -A x64 &&
      cmake --build --preset release --config Release && ctest --preset release -C Release`
      → 10/10 通过。
    - 稳定性：`test_app_managers` debug/release 各连续 100 次运行全部通过（修复一处
      偶发：flush 在"任务已释放单飞、promise 尚未结算"的窗口把已就绪 future 误判为
      未静止而多派一个空转排空任务——改为退让等待结算后消费，不新增排空；
      另修复用例自身两处缺陷：异常用例需先 flush 消费异常再解除注入标志；迟到送达
      用例快照断言前需 drain owner）。
    - GCC 语法检查（CI Linux 告警姿势）：
      `g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -fsyntax-only`（含
      Catch2/executor include）对 `tests/unit/test_app_managers.cpp`（包含全部新增
      头文件）通过——过程中修正两处 GCC 特有问题：`ManagerPump` 成员初始化顺序
      （-Werror=reorder，MSVC /W4 未报）；嵌套 Options 的默认实参 `= {}`（按仓库
      既有纪律上提为命名空间作用域 `ConversationManagerOptions`/
      `MessageManagerOptions`/`TransferManagerOptions`，类内保留 `using Options`
      别名）。
    - 覆盖映射（验收 ①，DOD-02 六项沿 Manager 任务路径）：正常完成（9 类事件路由
      → 排空 → 快照/主路径 FIFO 且 `events_dropped==0`；4×25 并发 `send_text` +
      主线程交错入队不丢、`rejected==0`）；任务异常（StubAdapter 抛出穿透排空任务 →
      future + `task_exception_count` 可见，`drain_failures==1`，泵自愈重排
      `spawn_count==2`、`processed==1`）；提交拒绝（`max_in_flight_tasks=1` 饱和 →
      排空任务提交以 `CapacityExhaustedException` 即时就绪，`task_submit_rejections
      ==1` + `capacity_exhausted_count>=1`，存量不丢；收件箱容量 1 → 第三次入队
      明确拒绝并经 Manager/Sink 返回值可见）；执行中取消（`start_transfer` →
      `submit_cancellable` 会话任务，`request_task_cancel` → StopToken 轮询退出，
      `running_request_count==1`、`cancelled_session_count==1`，Adapter 侧取消命令
      可见，重复取消幂等不重复计数）；超时（独立 owner：`max_threads=1` +
      `task_timeout_ms=40` → 排空任务排队软超时击杀，`timeout_count+1`、
      `drain_timeouts==1`，被击杀排空的存量经自愈重排全部完成）；shutdown（在飞
      会话 + 未排空发送存量，经设计第 8.3 节钩子顺序：`request_cancel_all` →
      flush → `set_sink(nullptr)`+`stop_discovery` → `AppStateOwner.close()` →
      `fully_stopped()`，关闭后注入明确拒绝，末次快照可读）。
      覆盖映射（验收 ②，RULE-08/退出-3）：`Cancelled` 传输的迟到进度与迟到
      `Completed` 宣告、`Failed` 消息的迟到送达回报，均 Sink admission 成功、状态机
      应用层拒绝（`updates_rejected==2` / `==1`），快照终态不变。
  - 限制：MinGW 默认生成器仍受 `M1-02` 验证记录限制 1 阻塞（pinned executor 构建
    缺陷），未变化；ASAN/UBSAN 证据随本 PR 的 Linux CI 门禁提供；TSAN 沿用
    限制 2。MR 闭环（分支/PR/CI/Squash）由后续环节按仓库流程执行，本记录不含
    commit/CI 证据。
  - 同步：设计第 8.2/8.3/10.1 节、[DEC-008](../decisions/DEC-008-manager-routing-and-executor-tasks.md)、
    总计划决策表与当前状态、本里程碑工作项与验证记录。

- 2026-09-22（`M1-06`，Windows 11 / MSVC 2022 BuildTools 14.44.35207 / CMake 4.1.0）：
  - 范围：根 `main.cpp`（M0 骨架打印替换为 M1 console 冒烟宿主：设计第 8.3 节
    组合根 + 六步演示脚本 + 受控关闭；输出以 `aki 0.1.0` 横幅开头、以
    `smoke: PASS` / `smoke: FAIL (N check(s) failed)` 收尾供 ctest 正则断言）、
    `tests/CMakeLists.txt`（新增 `smoke.device_lifecycle`，`integration` 标签自本项
    启用；`skeleton.app_runs` 断言保持兼容）、设计第 8.3 节（补冒烟宿主信任流
    语义：宿主经 `UpsertDevice` 模拟用户信任确认，UI 于 M5 接入——M1-08 纪律，
    先改设计再合代码）。
  - 依据：[设计第 8.1/8.2/8.3/10/10.1/14 节](../design/aki_design.md)；
    [DEC-002](../decisions/DEC-002-layering-and-state-boundary.md)（验证方式即该
    闭环）、[DEC-007](../decisions/DEC-007-test-framework.md)（integration 标签）、
    [DEC-008](../decisions/DEC-008-manager-routing-and-executor-tasks.md)（Manager
    路由与任务承载）；总计划 `RULE-01`/`RULE-02`/`RULE-06`~`RULE-09`、
    `EXEC-01`~`EXEC-07`、`DOD-01`~`DOD-06`；AGENTS.md Executor 规则 1/7/8；
    executor-integration 集成指南 tasks-and-lifecycle / communication /
    observability / scheduling 卡（本会话已加载）。
  - 验证（生成器说明同 `M1-02` 记录：MinGW 默认生成器受限制 1 阻塞，以 MSVC
    生成器等价执行）：
    - `cmake --preset debug -G "Visual Studio 17 2022" -A x64 &&
      cmake --build --preset debug --config Debug && ctest --preset debug -C Debug`
      → 11/11 通过（既有 10 + 新 `smoke.device_lifecycle`，标签
      integration=1 / smoke=1 / unit=9）。
    - `cmake --build --preset release --config Release && ctest --preset release
      -C Release` → 11/11 通过。
    - 确定性（验收 ③）：`aki.exe` debug 连续 50 次运行输出逐字节一致
      （`cmp` 比对基准输出，无任何差异）且退出码全 0；release 连续 50 次全部
      `smoke: PASS`。
    - GCC 语法检查（CI Linux 告警姿势）：
      `g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -fsyntax-only -I.
      -Ithird_party/executor/include main.cpp` 通过。
    - 覆盖映射（验收 ①，退出-1 全链路）：发现（start_discovery →
      inject_device_discovered/connected → 设备入 Store、presence Online、主路径
      事件 1~2）→ 信任（宿主经 UpsertDevice 模拟用户确认：Unknown -> Pending ->
      Trusted 均为信任状态机合法边，快照终值 Trusted）→ 会话与文本消息
      （ensure_conversation → Active；send_text(m-1) 经 Manager 排空任务出站 →
      本地 Sent；inject_message_delivered → SetDeliveryState Sent -> Delivered；
      inject_message_received(m-2) 收到即记录 Delivered；主路径序列号 1~6 严格
      FIFO，delivered 先于 received）→ 断开（presence Offline、会话
      Disconnected、消息历史保持）→ 重连（P2P，同一会话 id 回到 Active、不新建
      会话、历史不变，RULE-06）。
      覆盖映射（验收 ②③）：进程内 owner 为正式 `ExecutorOwner`（设计第 8.2 节
      落点说明自本项生效），演示中全部任务（四 Manager 排空泵）均经其 executor
      承载，无 `std::thread`/`std::async`/自建线程（RULE-07）；受控关闭按第 8.3
      节钩子顺序（request_cancel_all → flush 四 Manager 至泵静止并消费 future →
      `set_sink(nullptr)`+`stop_discovery` → `AppStateOwner.close()`）进入
      EXEC-01 步骤 2~5，以 `fully_stopped()`（Completed + lifecycle Stopped +
      wait_timeout_count==0）收尾，close 后注入明确拒绝——DOD-02 shutdown 项在
      集成层可见。
  - 限制：MinGW 默认生成器仍受 `M1-02` 验证记录限制 1 阻塞（pinned executor
    构建缺陷），未变化；ASAN/UBSAN 证据随本 PR 的 Linux CI 门禁提供；TSAN
    沿用限制 2。MR 闭环（分支/PR/CI/Squash）由后续环节按仓库流程执行，本记录
    不含 commit/CI 证据。
  - 同步：设计第 8.3 节、本里程碑工作项与退出-1、总计划当前状态、
    `tests/CMakeLists.txt` integration 标签。

- 2026-09-23（`M1-08` 收口审计，Windows 11 / MSVC 2022 BuildTools 14.44.35207 /
  CMake 4.1.0 / GCC 15.2.0（语法检查与边界 grep）；基线 `master`@`129dd0d`
  （PR #9 合入）+ 本审计文档改动）：
  - 范围：纯审计与文档同步，无代码变更。逐项校对矩阵结论、边界抽查证据、
    退出-2~5 证据归集如下；一项事实修正（`test_app_managers` 计数 9/295 →
    合入版 10/314，工程规范 6.1）。
  - 依据：本里程碑工作项 `M1-08` 与退出条件；设计第 3~10.1/14 节；
    [DEC-002](../decisions/DEC-002-layering-and-state-boundary.md)（验证方式复核）、
    [DEC-007](../decisions/DEC-007-test-framework.md)、
    [DEC-008](../decisions/DEC-008-manager-routing-and-executor-tasks.md)；
    总计划 `DOD-01`~`DOD-06`、`RULE-01`/`RULE-02`/`RULE-10`、`EXEC-01`~`EXEC-07`；
    工程规范第 4 节（状态与勾选规则）与 4.5（里程碑关闭）。executor-integration
    集成指南 tasks-and-lifecycle / communication / observability / scheduling 卡
    （本会话已加载）；审计无新增并发代码。
  - 校对矩阵（逐项结论，全部为"一致"；任何"偏差"均已按先文档后代码处置，见下节）：
    - 第 3 节设备身份 vs `device/device/device_types.hpp`：一致——
      DeviceIdentity/PresenceState/ConnectionPath/DeviceCapabilities 字段与枚举
      逐一对应（PresenceState 两值 :38-41）。
    - 第 4 节信任 vs `device/trust/trust_state.hpp`：一致——五态与合法边
      （:11-48）同 §4 转移规则，终态幂等语义同 §4 终态约束。
    - 第 5 节会话 vs `conversation/conversation/conversation_types.hpp`：一致——
      三态 + Active<->Disconnected + Archived 终态（:42-52），RULE-06 路径无关。
    - 第 6 节消息 vs `conversation/message/message_types.hpp`：一致——五类 typed
      payload（:29-35）、DeliveryState 正向链（:71-84）、收到即 Delivered 语义落
      MessageManager（第 8.3 节路由）。
    - 第 7 节传输 vs `transfer/transfer/transfer_types.hpp`：一致——七态（:34-42）、
      终态幂等（:57-82）、文件 metadata 与数据分离（RULE-05）。
    - 第 8.1 节 SPI vs `heyaki/adapter/heyaki_adapter.hpp` + fake：一致——出站
      2+1+4 方法、9 个 Sink 方法与 9 类事件一一对应、bool admission、
      final_state 仅终态；`grep -rln "executor/" heyaki/adapter/` 无输出（RULE-10）。
    - 第 8.2 节 owner vs `app/lifecycle/executor_owner.hpp`：一致——EXEC-01 五步
      逐一注释映射（shutdown :101-163）且经 test_executor_lifecycle 断言；唯一
      owner 纪律；blocking worker 由 owner 注册（:168）且 M1 不启用（§8.2:317 措辞）。
    - 第 8.3 节 Manager vs `app/application/` 五个头文件 + 根 `main.cpp`：一致——
      Store 所有权按 Manager 切分、9 类事件路由表与 router_sink.hpp 逐一对应、
      单飞有界排空泵（manager_runtime.hpp：消费 future/释放后复查收件箱/软超时
      自愈）、传输会话 submit_cancellable + request_task_cancel、装配与关闭钩子
      顺序与 main.cpp 六步一致；重复 TransferId 防线（transfer_manager.hpp:251-258，
      PR #8 评审补充）符合 §8.1 TransferId 语义与 RULE-09。
    - 第 10/10.1 节状态边界 vs `app/state/` 四个头文件：一致——9 类类型化事件 +
      owner 单写者序列号（app_events.hpp）、comm 四组件映射与三条硬约束
      （app_state_owner.hpp）、typed 更新清单与 §10.1 罗列完全一致（9 项变体）、
      owner 校验纪律（幂等 no-op/非法转移/未知 id 拒绝）与容量预算（RULE-09）。
    - 第 14 节目录 vs 实际布局：一致——蓝图目录全部存在，未启用子目录以 .gitkeep
      占位且延后项均有记录（heyaki/events→M3 DEC-006；transfer/manager、
      transfer/storage→M4；persistence→M2；ui→M5）；各域 skeleton.hpp 为 M0 构建
      脚手架，仍被 skeleton.includes 用例使用。
  - 已知偏差复核：四项均有锚点——①设计 3~6 节枚举补充（本文件 M1-01 工作项，
    :50）；②`heyaki/events` 延至 M3（M1-03 工作项 :68/:209 + 总计划 DEC-006 行）；
    ③`transfer/manager/` 留 M4 与 blocking worker/TimerHandle 措辞（M1-05 记录
    :292、设计 §8.2:317/§8.3、DEC-008「边界」节）；④冒烟宿主信任流经
    `UpsertDevice`（设计 §8.3:417、M1-06 记录）。扫描未发现其他漂移：
    未记录偏差数为 0。
  - 边界抽查（grep 证据，全部通过）：
    - RULE-10：`grep -rn "#include" device/ conversation/ transfer/ heyaki/` 仅
      领域头 + `<cstdint>/<string>/<string_view>/<vector>/<set>/<utility>/
      <variant>/<chrono>`；`grep -rin "executor|heyaki|sqlite|eui|windows|android"`
      在 Core 命中 2 处均为注释（device_types.hpp:13、discovery_types.hpp:31，
      说明取值来源，无类型暴露）。
    - RULE-01：`grep -rn "app/|ui/" heyaki/` 无输出（Adapter 无反向依赖）。
    - RULE-07：`grep -rn "std::thread|std::jthread|std::async|detach()"`（排除
      this_thread）仅命中 main.cpp 注释行；并发一律经 Executor 提交面。
    - EXEC-04 M1 边界：`grep -rn "submit_periodic|submit_delayed|TimerHandle|
      submit_realtime"` 在 app/ 与 main.cpp 无调用（仅 transfer_manager.hpp 注释与
      owner 的 start_blocking_worker 注册口，后者调用点仅 test_executor_lifecycle:200）。
    - executor 依赖限定：`grep -rln "executor" app/ main.cpp` 仅 app 层与宿主，
      领域层与 heyaki 层零引用。
  - 退出条件证据（可复现命令与结果）：
    - 退出-2（六项映射 + 命令）：
      `ctest --preset debug -C Debug -R "test_app_state|test_heyaki_adapter|
      test_executor_lifecycle|test_app_managers"` → 4/4 通过。六项 × 四条路径：
      comm 通道（test_app_state，7 个 dod02 用例）、Adapter 注入管线
      （test_heyaki_adapter，6）、owner 生命周期（test_executor_lifecycle，5）、
      Manager 任务路径（test_app_managers，8）——`grep -c "dod02"` 计数如前；
      正常完成/任务异常/提交拒绝/执行中取消/超时/shutdown 均有名点用例
      （test_app_state.cpp:283-436 六个 DOD-02 TEST_CASE）。
    - 退出-3（命令）：
      `ctest --preset debug -C Debug -R "test_trust_state|test_delivery_state|
      test_conversation_state|test_transfer_state"` → 4/4 通过；实测断言：
      trust 64（含 10 条非法转移表与终态幂等/迟到事件 3 个 SECTION）、
      delivery 38、conversation 18、transfer 73（含 late progress 不复活）；
      Manager 路径 RULE-08 用例 test_app_managers:929/:966 与
      test_app_state:437、test_heyaki_adapter late-events 用例。
    - 退出-4：本地 `ctest --preset debug -C Debug` → 11/11、
      `ctest --preset release -C Release` → 11/11（本审计执行）；ASAN/UBSAN：
      `gh pr view 8/9 --json statusCheckRollup` 核实两 PR 的
      `Linux / debug`、`Linux / asan`、`Linux / ubsan`、`Windows / debug (MSVC)`
      均 SUCCESS（覆盖合入 master 的全部 M1 代码，含 asan/ubsan 对
      test_app_managers 等 11 个可执行文件的常规运行）；MinGW 限制沿用
      `M1-02` 记录限制 1；TSAN 沿用限制 2（预设已存在，CI 矩阵未含 tsan）。
      本收口 PR 的 CI 复跑同一矩阵作为最终门禁（标准闭环合并前置条件）。
    - 退出-5：设计第 3~14 节与实现同步（校对矩阵）；生效决策
      DEC-001~004、DEC-007、DEC-008 与设计/计划交叉引用一致；
      总计划里程碑索引与当前状态随本审计更新。
  - 结论与状态：M1 全部 8 个工作项完成、退出-1~5 全部通过——按工程规范 4.5 将
    M1 置为 `Completed`；总计划里程碑索引置为 `Done`。v0.1.0 发布点就绪：
    建议于收口 PR 合入后由用户授权创建 tag `v0.1.0`（tag 创建不在本项范围）。
  - 限制：MinGW 默认生成器仍受 `M1-02` 记录限制 1 阻塞；TSAN 证据仍待 CI 矩阵
    扩展（限制 2）；本收口 PR 的 CI 结果由标准闭环核验（本记录不含其证据）。
  - 同步：本里程碑工作项 `M1-08` 与退出-2~5、里程碑状态 Completed、总计划
    里程碑索引与当前状态、M1-05 计数事实修正（9/295 → 合入版 10/314）。
