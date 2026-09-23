# M3：Heyaki 真实接入与文本消息

> 状态：Planned
> 负责人：Linductor
> 所属计划：[Aki 实施总计划](aki-implementation-plan.md)
> 前置：M1、M2（均已关闭：SPI/状态边界/Manager 骨架与本地持久化就绪）；
> [DEC-006](../decisions/DEC-006-heyaki-api-contract.md)（Heyaki API 契约版本与
> 目标级集成方式，2026-09-23 已冻结为 `Accepted`，本里程碑直接依据其执行）
> 建议发布点：v0.3.0
> 更新日期：2026-09-23

## 目标

以 pinned `third_party/heyaki`（v1.0.1-38 @ `e114508a`，[DEC-003](../decisions/DEC-003-dependency-locking.md)
锁定）替换 `FakeHeyakiAdapter` 作为宿主运行路径，交付真实设备链路：两台设备各自建立
Heyaki 密码学身份，在局域网中发现对方，经公钥指纹确认建立信任，组成一对一
Conversation 并收发文本消息（含送达回报），消息历史实时持久化；网络中断恢复后原
会话继续可用（`SCOPE-01/02/03/05/06/10/11` 的真实实现；`SCOPE-04` 的状态面随本
里程碑具备，展示归 M5）。本里程碑首次接触真实网络，`heyaki/events` 与
`heyaki/session` 目录（设计第 14 节）自本里程碑落地。

## 范围与非目标

### 范围

- `DEC-006` 冻结与 heyaki 目标级构建接入：对照 pinned 版本公开头文件与
  `third_party/heyaki/docs/api.md`、`docs/client-library.md`，固化
  `heyaki::client` / `heyaki::services` 接入方式与设计第 8.1 节 SPI 的 API 映射
  （身份/配置、LAN 发现、配对信任、消息与事件收发、会话重建），创建正式决策记录。
- 本地设备身份真实化：首次启动创建 Heyaki 长期身份（Ed25519），后续启动加载；
  `DeviceId` 与长期公钥稳定绑定（设计第 3 节），身份行经 M2 仓储持久化与恢复一致。
- 设备发现与信任真实化：LAN 发现（signed multicast）映射为统一
  `DiscoveredDevice`；配对（pairing）与公钥指纹确认驱动信任状态机
  `Unknown -> Pending -> Trusted / Rejected`、`Trusted -> Revoked`（设计第 4 节）；
  已知设备记录随启动恢复加载并支持再连接。
- Conversation 真实建链与文本消息：会话经 Heyaki Session 建立，路径无关
  （`RULE-06`）；`send_text_message` 真实实现（`MessageId` 应用侧生成并稳定，
  admission 拒绝可见）；`on_message_received` / `on_message_delivered` 驱动
  `DeliveryState` 推进与消息历史实时持久化（设计第 11.1 节 ① 写路径正式落点）。
- Presence 与连接路径：`on_device_connected` / `on_device_disconnected` 维护
  `PresenceState` 与 `ConversationState`（`Active <-> Disconnected`）；
  `on_connection_path_changed` 提供 LAN / P2P / Relay 展示数据（`SCOPE-10`）。
- 断线恢复：网络中断恢复后原 Conversation 继续可用、消息历史不变、不新建会话
  （`SCOPE-11`、`RULE-06`）。
- 宿主组合根切换：根 `main.cpp` 以真实 Adapter 替换 `FakeHeyakiAdapter`
  （Fake 保留供单测与回归）；EXEC-01 关闭顺序覆盖真实投递停止与 heyaki 库生命周期。
- 发现来源分期：以 LAN 发现与已知设备记录为主路径；Relay 发现、邀请链接与手动
  输入（`SCOPE-02` 其余来源）的接入方式随 `DEC-006` 调研结论细化，若延期至后续
  里程碑必须在退出证据中如实记录范围与补做条件。

### 非目标

- 图片消息与文件传输的真实数据链路（M4，`SCOPE-07`/`SCOPE-08`）；本里程碑
  Adapter 传输四接口仅保持 M1 签名语义。
- EUI-NEO UI 与设备列表展示（M5，`SCOPE-04` 展示面/`SCOPE-12`）。
- 群组/多设备会话、Agent、远程能力（`SCOPE-13`~`SCOPE-17`、`POST-NN`）。
- heyaki relay 服务器的部署与运营（heyaki 自身范围；Aki 仅在验证 Relay 路径展示
  时使用测试 relay 环境，条件见风险节）。

## 设计与决策依据

- [Aki 设计方案](../design/aki_design.md)第 3~7 节（领域模型与状态机）、第 8.1 节
  （Heyaki Adapter SPI——"记录型来源的接入在 M3 细化"）、第 8.2/8.3 节（owner 与
  Manager 路由）、第 10/10.1 节（9 类事件与单写者）、第 11.1 节（持久化集成契约）、
  第 14 节（`heyaki/adapter`、`heyaki/events`、`heyaki/session` 落点）、第 15 节。
- [DEC-002](../decisions/DEC-002-layering-and-state-boundary.md)（分层与状态边界、
  真实 Heyaki 在 M3 接入）、[DEC-003](../decisions/DEC-003-dependency-locking.md)
  （pinned 锁定与目标级接入点）、[DEC-008](../decisions/DEC-008-manager-routing-and-executor-tasks.md)
  （Manager 路由与任务承载；多线程回调下的序语义重评条款）。
- `DEC-006`（已冻结，2026-09-23）：Heyaki API 契约版本与目标级集成方式——
  pinned v1.0.1-38 公开面为契约基线（wire {1,3}）、单一构建图接入
  `heyaki::client`、executor target 由 heyaki 子目录提供（双侧同 pin
  `74a94198`）、`Runtime::create_borrowed` 注入、SPI↔API 逐项映射与冻结常量
  （`application_id="org.aki.app"`、`"aki.text"`、`message.send` scope、ID hex
  双射）、LanPresence 元数据缺口记录；含 DEC-003 executor 接入条款修订。
- M3 开工前两项调研结论（2026-09-23，已并入本节与风险节）：
  - **executor 生命周期协调（EXEC-01）**：采用借用注入方案——Aki
    `ExecutorOwner` 仍是进程内唯一 executor 实例与唯一 owner，宿主以
    `heyaki::Runtime::create_borrowed(executor_owner.executor(), cfg)` 创建
    借用型 Runtime 并经 `NodeConfig.runtime` 注入 `Node::create`（pinned
    `runtime.hpp:205`；borrowed 分支不碰宿主 executor 生命周期、不设库内
    failure callback，监控权留在 owner；其 AsioWorker/FileIoWorker 经
    `start_worker(BlockingWorkerSpec)` 挂在同一 executor，worker 名
    `heyaki-asio` / `heyaki-asio-file-io`，并发对 Executor 完全可见——非能力
    缺口，不进 9.4 台账）。关闭：`Node::shutdown()`（对外部 runtime 只
    request_stop）+ 宿主显式 `Runtime::shutdown()`（回收挂在 Aki executor 上
    的 heyaki worker）整体编入 EXEC-01 步骤 1 钩子，早于 owner 步骤 2/3/5；
    断言 `RuntimeShutdownReport.executor_shutdown_performed == false` 与 owner
    `fully_stopped()`（heyaki 上游测试有「borrowed 不关宿主 executor」门禁
    先例）。否决备选：owned/`runtime=nullptr`（第二 executor 实例，违反唯一
    owner）、全局单例（隐藏全局生命周期）。初始化顺序约束：`create_borrowed`
    前置宿主 executor 已 Running（`borrowed_executor_not_running` 拒绝路径须
    可见）；borrowed 模式下 RuntimeConfig 的 executor 线程参数不生效，合并
    负载 sizing 只能在 ExecutorOwner 配置做。
  - **第 11.1 节 ① 写路径正式落点**：选「AppStateOwner 接受后回调」——在
    AppStateOwner 契约新增构造注入的接受后处理器（对齐 ManagerPump 的
    Handler 先例），在 drain_updates 的 apply() 返回 true 后于 owner 单写者
    上下文按接受顺序同步调用，组合根实现该处理器（复用 PersistenceMirror
    的 jobs_for 映射 + update_jobs 工厂）将 DbJob 入队 DatabaseWorker 有界
    通道；幂等 no-op 接受同样入队（补回 §11.1① 承诺、M2-07 偏差跳过的
    行为）。否决备选：owner 侧 tap（第二有界准入点 + 跨通道保序复杂度，违
    RULE-09 风险）、维持宿主快照镜像（M3 Manager 驱动事件流无写路径）、
    9 类事件面驱动（§11.1① 明文禁止）。**阻塞子决策**：UpsertMessage 的
    conversation_id 归属缺口（Message 无 ConversationId，M2-07 靠宿主簿记）
    必须与写路径落点同批固化（倾向扩展 UpsertMessage 并同步评估第 6 节
    Message 模型会话归属），否则 M3-05 消息行无法落库。落地按 M1-08 纪律
    先更新设计 §10.1/§11.1①（含 §8.3 装配顺序与 §11.1②「DatabaseWorker 在
    播种完成后才注册」措辞对齐）再动代码，`M3-02` 执行。
- M1 已知偏差"`heyaki/events` 延至 M3"（[M1 里程碑](m1-domain-state.md) M1-08
  记录）；M2 遗留"Manager 更新拦截正式落点待 M3 真实事件源接入时按第 11.1 节 ①
  评估，如需改 `AppStateOwner` 契约先更新第 10.1/11.1 节"（[M2 里程碑](m2-local-persistence.md)
  M2-07 验证记录）。
- AGENTS.md Executor 强制规则与工程规范第 9 节；heyaki 并发自身运行于 pinned
  executor（其 README 声明），与 Aki `ExecutorOwner` 的协调方式是本里程碑首要
  确认项。

## 工作项

- [ ] `M3-01` 按 `DEC-006` 完成 heyaki 目标级构建接入与实测验证（决策与调研
  已于 2026-09-23 冻结，本项为执行与取证）：单一构建图
  `add_subdirectory(third_party/heyaki)` 只链 `heyaki::client`
  （`HEYAKI_BUILD_APPS=OFF`、`HEYAKI_AUTO_INSTALL=OFF`），Aki 移除自己的
  executor add、SYSTEM include/告警豁免转移到 heyaki 提供的 executor target，
  configure 校验扩展为 Aki lock / heyaki lock / checkout 三方 executor commit
  一致；实测验证清单（调研均为静态证据，未编译未运行，如实复验）：三套工具链
  configure/build（MSVC 含 OpenSSL/DLL 部署实测、MinGW configure-only、CI Linux
  三档）、最终二进制单份 sqlite3 符号与有效版本（dumpbin/nm）、tsan preset
  联动 `HEYAKI_SANITIZER=thread` 的全图插桩；Aki 侧依赖边界
  （`RULE-01`/`RULE-10`：heyaki 类型封死在 `heyaki/` 层）。
- [ ] `M3-02` 设计先行契约固化（先文档后代码，M1-08 纪律）：按调研已选方向
  固化第 11.1 节 ① 写路径正式落点——`AppStateOwner` 构造注入接受后处理器
  （owner 单写者上下文按接受顺序同步调用、幂等 no-op 同样入队），更新第
  10.1/11.1① 与 §8.3 装配顺序、§11.1② 注册时序措辞；同批固化阻塞子决策：
  `UpsertMessage` 的 conversation_id 归属（倾向扩展更新载荷并同步评估第 6 节
  Message 模型会话归属）；处理器异常策略与容量预算入契约。细化设计第 8.1 节
  记录型来源接入与"发现 → 进入信任确认"的触发语义（DEC-006 映射权威下的
  Adapter 观察管道语义）。
- [ ] `M3-03` 本地设备身份真实化（`SCOPE-01`）：首次启动创建 Heyaki 长期身份、
  后续启动加载；`DeviceId` 与公钥稳定绑定；身份行持久化，重启恢复后一致。
- [ ] `M3-04` 设备发现与信任真实化（`SCOPE-02`/`SCOPE-03`）：LAN 发现映射
  `DiscoveredDevice`；配对与公钥指纹确认驱动信任状态机全部合法转移；已知设备
  记录随启动恢复加载并支持再连接；`Rejected`/`Revoked` 终态幂等（`RULE-08`）。
- [ ] `M3-05` Conversation 建链与文本消息真实化（`SCOPE-05`/`SCOPE-06`）：会话
  经 Heyaki Session 建立；`send_text_message` 真实实现（admission 拒绝可见，
  `RULE-09`）；接收与送达回报驱动 `DeliveryState` 正向链与终态；消息历史经
  `M3-02` 固化的写路径实时持久化，重启恢复一致。
- [ ] `M3-06` Presence 与连接路径（`SCOPE-10`）：连接/断开事件维护
  `PresenceState` 与 `ConversationState`；连接路径变化（LAN / P2P / Relay）经
  第 10.1 节 LatestMailbox 语义发布，路径切换不新建会话（`RULE-06`）。
- [ ] `M3-07` 断线恢复（`SCOPE-11`）：网络中断恢复后原 Conversation 继续可用、
  历史不变；迟到事件不复活终态；重连路径的取消与超时语义闭合
  （`EXEC-05`，长任务可解除阻塞）。
- [ ] `M3-08` 双端真实链路验证与宿主切换：组合根切换真实 Adapter（Fake 保留
  用于单测）；两进程回环（及可行时的 LAN 双端）集成测试覆盖发现 → 信任 →
  文本收发 → 送达 → 断线恢复全闭环；DOD-02 六项沿真实事件路径（Adapter 回调、
  Manager 排空、连接事件处理）覆盖。
- [ ] `M3-09` 收口审计与退出证据归集（沿用 M1-08 / M2-08 纪律）：实现与设计
  第 3~8/10/11.1/14 节及 `DEC-006` 逐项校对，退出-1~5 证据与可复现命令归档，
  发现来源分期（若有）如实记录范围与补做条件。

## 风险与阻塞

- **单一 executor 副本（最高风险，M3-01 实测）**：heyaki `CMakeLists.txt:129`
  无条件 `add_subdirectory(third_party/executor)`，executor 目标名全局为
  `executor`——Aki 必须移除自己的 executor add（DEC-006 已定），并验证单图内
  只编译一份 executor（双侧 pin 同为 `74a94198`，已核实）；SYSTEM include 与
  告警豁免设置须转移到 heyaki 提供的 executor target，configure 校验扩展为
  Aki lock / heyaki lock / checkout 三方一致（DEC-003 修订项，任一侧升级即破坏
  注入前提）。
- **双 SQLite 符号（M3-01 实测）**：Aki vendored sqlite 3.53.4（persistence）与
  heyaki `heyaki::sqlite` 3.50.4（profile PRIVATE）同图静态链接各含全套
  sqlite3_* 符号——须用 dumpbin/nm 验证最终二进制仅一份符号并记录有效版本；
  出现行为冲突再立统一决策，不得静默。
- **首次配置网络与 bash**：heyaki 依赖树经 `scripts/fetch_third_party.sh` 按
  ref+commit 双校验拉取（当前 `third_party/heyaki/third_party` 仅锁文件未拉取，
  已核实）；Windows 需 bash（Git for Windows）；CI Linux 需网络 + bash +
  libssl-dev，建议 actions/cache 缓存。与 DEC-003「submodule + configure 校验」
  的偏差已在 DEC-006 声明锁链。
- **MSVC OpenSSL/DLL 部署**：heyaki 依赖 OpenSSL 3（`find_package REQUIRED`），
  本地与 windows CI 需 `OPENSSL_ROOT_DIR` 并部署 libssl-3-x64.dll /
  libcrypto-3-x64.dll；构建树内链 LibDataChannelStatic 预期无需 datachannel.dll，
  需实测确认。MinGW 维持 configure-only 并如实记录限制（pinned executor 本体
  在 w64devkit 不可构建，M1-02 限制 1，heyaki 强依赖 executor 故同样受限）。
  CI Linux 三档（debug/asan/ubsan/tsan）为全量构建主路径。
- **TSAN 联动（M3-01 实测）**：heyaki 将 `EXECUTOR_ENABLE_TSAN` 强制 OFF，
  `HEYAKI_SANITIZER=thread` 时对 executor target 直接补 `-fsanitize=thread`——
  Aki tsan preset 必须联动设置，并以构建日志/ExecutorSnapshot 验证全图插桩
  （`DOD-03`）。
- **executor 协调实现期验证**：borrowed 注入方案已定（见设计与决策依据），关闭
  顺序 `Node::shutdown` + `Runtime::shutdown` 编入 EXEC-01 步骤 1 钩子进 DOD-02
  shutdown 项与 M3-08 双端验证（含 `executor_shutdown_performed==false` 断言）；
  容量预算合并复核（heyaki async 负载 + 2~3 个 blocking worker 并入后
  ExecutorConfig 定容）；EXEC-06 对账可用 heyaki RuntimeSnapshot 的 executor_*
  计数（读自同一实例）。
- **写路径正式落点的阻塞子决策**：UpsertMessage 的 conversation_id 归属缺口
  必须与落点同批固化（见设计与决策依据），否则 M3-05 消息行无法落库；处理器
  异常策略（handler 不得抛或全捕获 + 计数）与容量预算（owner drain 批 64 ×
  每更新至多 2 作业 = 128 ≤ DB 通道 256，通道满拒绝双计数可见）写入 M3-02
  契约。
- **回调线程模型**：Node 回调在 executor 上下文触发，Adapter 侧保持"有界校验 +
  投递"（`EXEC-02`），业务 handler 一律在 Manager 执行上下文；多线程回调下的
  序语义按 DEC-008 重评条款在 M3-02/M3-05 验证。
- **真实网络测试环境**：CI 无真实 LAN 双端——以两进程回环为主要证据路径，LAN
  双端手动验证记录补跑条件；Relay 路径展示验证依赖测试 relay 环境，缺失时
  `SCOPE-10` 的 Relay 项记录限制与补跑条件，不冒充已验证。
- pinned heyaki 升级不属本里程碑：锁定 commit 不变，API 差异经 `DEC-006` 记录
  后按需另立变更。

## 测试与退出条件

- [ ] 退出-1：双端真实闭环——两台设备（两进程回环或 LAN 双端）完成身份建立 →
  发现 → 信任（指纹确认）→ Conversation → 文本消息收发（含送达回报）→ 历史
  持久化与重启恢复，逐域断言一致（`SCOPE-01/02/03/05/06`）。
- [ ] 退出-2：DOD-02 六项沿真实事件路径通过——正常完成、任务异常、提交拒绝、
  执行中取消、超时、shutdown（真实投递停止并入 EXEC-01 顺序）。
- [ ] 退出-3：断线恢复——中断恢复后原 Conversation 继续可用、消息历史不变、
  不新建会话；迟到事件不复活终态（`SCOPE-11`/`RULE-06`/`RULE-08`）。
- [ ] 退出-4：debug/release 构建与全量测试通过；ASAN/UBSAN 随 CI 门禁；真实
  网络路径跨上下文按 `DOD-03` 评估 TSAN；不适用工具链记录限制与补跑条件。
- [ ] 退出-5：设计（第 8.1/10.1/11.1 节细化）、`DEC-006`、总计划与里程碑状态
  同步；验证记录含可复现命令与环境（回环/LAN 拓扑说明）；发现来源分期（若有）
  已记录范围与补做条件。

## 验证记录

（尚无记录；自 `M3-01` 起按工程规范 6.1/6.3 追加。）
