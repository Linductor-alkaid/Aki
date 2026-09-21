# M1：领域模型与状态边界

> 状态：In Progress
> 负责人：Linductor
> 所属计划：[Aki 实施总计划](aki-implementation-plan.md)
> 前置：M0（依赖已就绪：executor 已 pin 并通过校验，见
> [DEC-003](../decisions/DEC-003-dependency-locking.md)；M0 关闭后启动本里程碑）
> 建议发布点：v0.1.0
> 更新日期：2026-09-22

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
- ~~Application State 的快照/订阅语义（`DoubleBuffer` 与 `LatestMailbox` 的具体落点）需在
  `M1-02` 设计时对照 pinned executor 文档确认，必要时补充设计小节。~~ 已解除
  （2026-09-22，`M1-02`）：落点对照 pinned 版本头文件与集成指南确认并固化为
  [设计第 10.1 节](../design/aki_design.md)——`DoubleBuffer<AppState>`（SWMR，更新经
  `MpscChannel` 汇聚到单一状态 owner）、`LatestMailbox<ConnectionPath>`（单值最新状态）、
  `Topic<AppEventPtr>`（可容忍丢失的观察者广播）；9 类必达事件主路径维持
  `MpscChannel<AppEvent>`（`EXEC-02`）。

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
