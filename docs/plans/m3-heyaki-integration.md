# M3：Heyaki 真实接入与文本消息

> 状态：In Progress
> 负责人：Linductor
> 所属计划：[Aki 实施总计划](aki-implementation-plan.md)
> 前置：M1、M2（均已关闭：SPI/状态边界/Manager 骨架与本地持久化就绪）；
> [DEC-006](../decisions/DEC-006-heyaki-api-contract.md)（Heyaki API 契约版本与
> 目标级集成方式，2026-09-23 已冻结为 `Accepted`，本里程碑直接依据其执行）
> 建议发布点：v0.3.0
> 更新日期：2026-09-24

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

- [x] `M3-01` 按 `DEC-006` 完成 heyaki 目标级构建接入与实测验证（决策与调研
  已于 2026-09-23 冻结，本项为执行与取证）：单一构建图
  `add_subdirectory(third_party/heyaki)` 只链 `heyaki::client`
  （`HEYAKI_BUILD_APPS=OFF`、`HEYAKI_AUTO_INSTALL=OFF`），Aki 移除自己的
  executor add、SYSTEM include/告警豁免转移到 heyaki 提供的 executor target，
  configure 校验扩展为 Aki lock / heyaki lock / checkout 三方 executor commit
  一致；实测验证清单（调研均为静态证据，未编译未运行，如实复验）：三套工具链
  configure/build（MSVC 含 OpenSSL/DLL 部署实测、MinGW configure-only、CI Linux
  三档）、最终二进制单份 sqlite3 符号与有效版本（dumpbin/nm）、tsan preset
  联动 `HEYAKI_SANITIZER=thread` 的全图插桩；Aki 侧依赖边界
  （`RULE-01`/`RULE-10`：heyaki 类型封死在 `heyaki/` 层）。（2026-09-24：
  依赖树 33 项 runtime + googletest/zstd 全部 verified（ref+commit 双校验，
  含 libdatachannel 递归子模块）；单图接入 + 开关冻结 + heyaki 测试不进构建面
  实测；三方 executor 校验 STATUS 实测；MSVC debug/release configure/build/
  ctest 全过（20/20，原 19 项零回归），OpenSSL DLL 部署集与 LibDataChannel
  Static 无需 datachannel.dll 实测确认；双 SQLite 实测出链接序敏感——aki_
  persistence 先于 heyaki::client 时 Aki vendored 3.53.4 确定性胜出（map
  取证 697 sqlite3_* 符号全部单源、heyaki_sqlite 零符号进入、二进制无
  3.50.4 字符串），无行为冲突无需改决策，运行期版本断言固化为回归护栏；
  MinGW configure-only 未达成（失败点前移至 heyaki vendored SQLite 生成，
  HeyakiVendoredRuntime.cmake:378）——限制如实记录；tsan 全图插桩与 CI 四档
  全绿随本 PR 门禁（本地不可执行）。详见验证记录。）
- [x] `M3-02` 设计先行契约固化（先文档后代码，M1-08 纪律）：按调研已选方向
  固化第 11.1 节 ① 写路径正式落点——`AppStateOwner` 构造注入接受后处理器
  （owner 单写者上下文按接受顺序同步调用、幂等 no-op 同样入队），更新第
  10.1/11.1① 与 §8.3 装配顺序、§11.1② 注册时序措辞；同批固化阻塞子决策：
  `UpsertMessage` 的 conversation_id 归属（倾向扩展更新载荷并同步评估第 6 节
  Message 模型会话归属）；处理器异常策略与容量预算入契约。细化设计第 8.1 节
  记录型来源接入与"发现 → 进入信任确认"的触发语义（DEC-006 映射权威下的
  Adapter 观察管道语义）。（2026-09-24：纯文档变更，无产品代码——新建
  [DEC-009](../decisions/DEC-009-appstate-write-path.md)（6.2 模板，Accepted，
  含 tap/维持镜像/事件面驱动三项否决与 Message 模型子决策）；设计第 10.1 节
  新增接受后处理器契约（PostAcceptHandler 构造入参、accept 后单写者上下文按
  接受顺序同步调用、不抛出 + owner 全捕获 `post_accept_failures`）、第 11.1 节
  标题与 ①（写入时机替换为正式落点 + 容量预算 64×2=128≤256 + 双计数失败语义）
  与 ②（control 先于 owner 构造的时序对齐，未注册窗口拒绝可见）、第 8.3 节
  装配顺序固化为七步（恢复→control→AppStateOwner(seed+handler)→Manager→
  注册→RouterSink）、第 8.1 节记录型来源与「发现→信任确认」触发语义四条；
  `UpsertMessage` 子决策=扩展载荷加 conversation 字段、§6 Message 模型不变。
  M2-07 镜像偏差被 DEC-009 取代（M2 历史记录保持原样）；实现随 M3-03+ 按新
  契约跟进，当前代码暂不变。详见验证记录。）
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

- 2026-09-24（`M3-01`，Windows 11 / MSVC 2022 BuildTools 14.44.35207 /
  CMake 4.1.0 / Git for Windows bash 5.2 / OpenSSL 3.5.8（开发安装
  `G:/OpenSSL-Win64`，`-DOPENSSL_ROOT_DIR` 注入）；执行与取证，构建接入
  变更不含 heyaki/ 层适配代码）：
  - 范围：根 `CMakeLists.txt`（heyaki 单图接入块（行 19 起）：
    `HEYAKI_BUILD_APPS=OFF` / `HEYAKI_AUTO_INSTALL=OFF` /
    `HEYAKI_FETCH_DEPENDENCIES=OFF`（configure 只做 --check 校验，不静默
    联网）/ `HEYAKI_VERIFY_DEPENDENCIES=ON`、`BUILD_TESTING` 局部 OFF、
    `add_subdirectory(third_party/heyaki)`、SYSTEM include 转移到 heyaki
    提供的 executor target（IDE 生成器门控保留，行为同 M1 记录）；Aki 自己
    的 executor add 块移除（`DEC-003` 条款按 `DEC-006` 修订）；新增
    `aki_deploy_openssl_dlls`（POST_BUILD 部署 libssl-3-x64.dll /
    libcrypto-3-x64.dll））、`cmake/Dependencies.cmake`（三方 executor
    commit 一致校验块）、`third_party/dependencies.lock.json`（executor
    used_by 修订为 DEC-006 措辞，diff 1 行）、`CMakePresets.json`（tsan
    预设增 `HEYAKI_SANITIZER: thread`）、`.github/workflows/ci.yml`
    （heyaki 依赖 fetch 步骤 + actions/cache 缓存 + Linux libssl-dev +
    Windows OPENSSL_ROOT_DIR）、`tests/unit/test_heyaki_client_surface.cpp`
    + `tests/CMakeLists.txt`（unit 标签边界锁定用例，链接序契约注释在案）。
    构建图内只消费 `heyaki::client`（`heyaki::services` 空伞 target 未链，
    `DEC-006`）。
  - 依据：[DEC-006](../decisions/DEC-006-heyaki-api-contract.md)（决策节全部
    + 影响与风险节：DEC-003 修订条款/双 SQLite/首次配置网络/三工具链/TSAN
    联动）、本里程碑 `M3-01` 工作项与风险节、设计第 8.1/8.2 节、
    [DEC-003](../decisions/DEC-003-dependency-locking.md)、
    [DEC-007](../decisions/DEC-007-test-framework.md)、总计划
    `RULE-01`/`RULE-07`/`RULE-10`、`EXEC-01`、`DOD-03`/`DOD-05`/`DOD-06`。
    executor-integration blocking-io 卡本会话按 SKILL 路由已加载（本项为
    构建接入，无新增并发路径，DOD-02 六项不适用——沿 M2-05 DatabaseWorker
    路径覆盖并随全量 ctest 复验）。
  - ① 依赖树首次拉取：`bash third_party/heyaki/scripts/fetch_third_party.sh`
    → runtime 33 项全部 `verified @ <commit>`（ref+commit 双校验；含
    libdatachannel v0.23.2 及其 5 个递归子模块、sqlite version-3.50.4 @
    `8ed5e7365e6f`、executor @ `74a9419`、protobuf v31.1（递归）、abseil、
    blake3、libsodium、FTXUI 等），exit 0。`--check` 离线复核 → `all
    selected dependencies are valid`。googletest v1.17.0 @ `52eb8108` 与
    zstd v1.5.7 @ `f8745da6` 按 lock 条目补拉——heyaki configure 的许可证
    清单（licenses.lock，40 项）无条件校验两者 LICENSE 文件存在（缺 zstd
    时实测 FATAL 于 `third_party/heyaki/CMakeLists.txt:554`，即 heyaki
    测试关建造就 License 残留依赖，如实记录）。
  - ② 单一构建图与测试隔离：`ctest --test-dir build/m3-01-debug -C Debug
    -N` → 20 项全部为 Aki 侧测试（19 项既有 + 1 项新边界用例），无 heyaki
    上游测试进入 Aki 构建面（`BUILD_TESTING` 局部 OFF 挡住 heyaki
    `CMakeLists.txt:507` 的 `add_subdirectory(tests)`）。
  - ③ executor 单图单副本：`find build/m3-01-debug -name executor.lib` →
    仅 `lib/Debug/executor.lib` 一份，对象目录 `third_party/heyaki/
    third_party/executor/src/executor.dir`（heyaki checkout 提供）；Aki
    `third_party/executor` 不在构建图。
  - ④ 三方 executor 校验：debug 与 release 两树 configure 均输出 STATUS
    `Executor pin verified three-way consistent (Aki lock / heyaki lock /
    checkout @ 74a94198fbe0f2a4081cd260658a26f969986870)`；FATAL 负向语义
    在联调中实际触发过（解析失败路径输出 `Executor pin mismatch across the
    single build graph (DEC-006) ... Align both lock files before
    configuring.`）。
  - ⑤ MSVC 实测（configure 命令：`cmake -S . -B build/m3-01-{debug,release}
    -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE={Debug,Release}
    -DAKI_BUILD_TESTS=ON -DAKI_WARNINGS_AS_ERRORS=ON
    -DOPENSSL_ROOT_DIR=G:/OpenSSL-Win64`）：
    - debug：configure Configuring done → build exit 0 → `ctest --test-dir
      build/m3-01-debug -C Debug --timeout 180` → 100% passed 20/20（既有
      19 项零回归）；release 同构 → 20/20。
    - OpenSSL DLL 部署集实测：build 输出 `deploying libssl-3-x64.dll for
      test_heyaki_client_surface`（及 libcrypto）；tests/{Debug,Release}
      目录实际只含该两个 DLL。
    - LibDataChannelStatic：tests 目录无 datachannel.dll，全部测试通过——
      构建树内静态链接无需 datachannel.dll 确认（`DEC-006` 预期项）。
    - 边界用例运行输出（两配置一致）：`heyaki 1.0.1 @
      e114508ab32d496d52e9db9bac26eb1cc88c4ae7, wire {1,3}` +
      `effective sqlite in this binary: 3.53.4 (lock: 3.53.4)` + feature 位
      断言通过（HEYAKI_BUILD_APPS=OFF：client 在编、relay/tui 不在编）。
  - ⑥ 双 SQLite（`DEC-006` 影响节实测项，重要发现与处置）：边界用例首版以
    heyaki::client 在前的链接序编译，运行期有效版本为 heyaki 副本 3.50.4
    → 用例断言失败暴露（不静默）。处置：测试目标链接序固定
    `aki_persistence` 先于 `heyaki::client`（tests/CMakeLists.txt 链接序
    契约注释在案）——Aki 经哈希审计的 vendored 3.53.4（`DEC-004`）确定性
    胜出。取证（MSVC `/MAP` map 文件；`dumpbin //symbols` 对最终 EXE 不
    枚举静态库符号解析结果，工具替换如实记录）：`grep -c "sqlite3_"
    boundary.map` = 697 且全部来自 `sqlite3:sqlite3.obj`（Aki vendored）；
    `grep -c "heyaki_sqlite" boundary.map` = 0（heyaki 副本零符号进入）；
    最终二进制字符串计数 release exe `3.50.4` 出现 0 次（debug 同）。
    结论：单份 sqlite3 符号、有效版本 = 3.53.4 = 锁文件；无行为冲突，
    无需改 `DEC-004` 或新增统一决策。回归护栏 = 边界用例运行期版本断言
    （未来链接配置漂移致 heyaki 副本胜出时测试即失败）。
  - ⑦ 边界 grep（`RULE-01`/`RULE-10`）：`grep -rnE '#include
    [<"]heyaki/[a-z_]+\.hpp[>"]'`（heyaki 公开单分量头）在第一方代码仅
    `tests/unit/test_heyaki_client_surface.cpp`（8 处，接线层消费点）命中
    （`tests/test_skeleton.cpp` 的 `heyaki/skeleton.hpp` 为 Aki 自有骨架
    文件，非第三方）；Aki 自身 `heyaki/` 层（adapter SPI/Fake）零第三方
    heyaki 类型；产品目标不链 `heyaki::client`（CMake 链接面锁定，公开头
    泄漏即编译失败）。
  - ⑧ MinGW configure-only（限制，未达成）：`cmake -S . -B build/mingw-m3-cfg
    -G "MinGW Makefiles" -DOPENSSL_ROOT_DIR=G:/OpenSSL-Win64` → Aki 侧全部
    通过（三方 executor 校验 STATUS + `Pinned dependency verification
    passed`），随后失败于 heyaki vendored SQLite amalgamation 生成
    （`third_party/heyaki/cmake/HeyakiVendoredRuntime.cmake:378` `Pinned
    SQLite configure failed: unknown error`——heyaki 在 configure 期对
    canonical sqlite 检出树执行其自带 configure+make，w64devkit 环境不可
    用）。较 M1-02 记录限制 1 进一步：失败点前移至 heyaki 子配置；本机另无
    MinGW 兼容 OpenSSL 开发库（slproweb 安装仅 MSVC .lib）。补跑条件：
    MSYS2 完整工具链（含 mingw-w64-x86_64-openssl）重试，或以 CI Linux
    三档为全量主路径（与 `DEC-006` 三工具链风险节预期一致）。
  - ⑨ tsan 联动：preset 已设 `HEYAKI_SANITIZER=thread`（heyaki 侧对
    executor target 补 `-fsanitize=thread` 编译 + PUBLIC 链接选项的补丁
    路径为 `third_party/heyaki/CMakeLists.txt:190-204`，GNU/Clang 生效）；
    本机（MSVC/w64devkit）无法运行 tsan——全图插桩验证随 CI Linux tsan 档
    （PR 门禁）提供，本记录如实标注未本地执行。
  - 限制：CI Linux debug/asan/ubsan/tsan 四档全绿证据随本 PR 门禁产生
    （本地不可执行，ci.yml 已更新）；本地 preset（老 build/debug、
    build/release 树）自本项起 configure 需附 OpenSSL（`-DOPENSSL_ROOT_DIR=
    <OpenSSL 3 前缀>` 或同名环境变量），本项实测以显式 -S/-B 树
    build/m3-01-debug、build/m3-01-release 执行；`build/openssl-3.5.8`
    残留目录被系统 msiexec 服务占用无法删除（build/ 内，不进仓库）；MR
    闭环由后续环节执行，本记录不含 commit/CI 证据。
  - 同步：本里程碑（状态 In Progress、M3-01 勾选、本记录）、总计划当前
    状态与里程碑索引（M3 → In Progress）。

- 2026-09-24（`M3-02`，纯文档变更，无产品代码——沿 `M2-01` 设计先行先例；
  Windows 11 工作站，编辑与核验为本会话执行）：
  - 范围：[docs/decisions/DEC-009-appstate-write-path.md](../decisions/DEC-009-appstate-write-path.md)
    （新建，工程规范 6.2 模板：背景/决策/备选/影响与风险/验证方式/关联文档）；
    [设计第 10.1 节](../design/aki_design.md)（新增「接受后处理器」段：
    `PostAcceptHandler` 构造入参、accept 后单写者上下文按接受顺序同步调用、
    被拒绝不调用、不抛出 + `post_accept_failures` 全捕获、入队拒绝双计数）；
    [设计第 11.1 节](../design/aki_design.md)（标题加 M3-02/DEC-009 修订注记 +
    M2-07 过渡形态声明；① 写入时机替换为正式落点 + 幂等 no-op 同样入队（补回
    原句）+ 容量预算 64×2=128≤256 + 入队拒绝双可见；② 新增时序对齐段：control
    先于 owner 构造、注册指 `mark_registered()` 时点、未注册窗口拒绝可见且预期
    计数 0）；[设计第 8.3 节](../design/aki_design.md)（装配顺序固化为七步：
    initialize → 启动恢复 → DatabaseWorkerControl → AppStateOwner(初始快照 +
    处理器) → 四 Manager → 注册 worker → RouterSink；M1-06/M2-07 过渡形态
    声明）；[设计第 8.1 节](../design/aki_design.md)（记录型来源接入四条：
    扫描型=观察管道启停、「发现→信任确认」触发=DeviceDiscovered 主路径事件 +
    `trust_state == Unknown` 过滤、已知设备记录=启动恢复直入 Store 不重放
    discovered、邀请链接/手动输入=同一入口分期）；[DEC-009](../decisions/DEC-009-appstate-write-path.md)
    子决策 ②=扩展 `UpsertMessage` 载荷加 conversation 字段、第 6 节 Message
    模型不变（评估结论：归属是持久化关联元数据而非消息本体语义，§6 改动牵动
    协议/UI 模型收益为零）。本里程碑文档（M3-02 勾选 + 本记录）、总计划当前
    状态。
  - 依据：本里程碑 `M3-02` 工作项与「设计与决策依据」两项调研结论（写路径
    落点选型、executor 生命周期协调——后者已于 M3-01 落地）；[M2 里程碑
    M2-07 验证记录偏差段](m2-local-persistence.md)（被 DEC-009 取代的临时
    形态）；[设计第 10.1/11.1/8.3/8.1/6 节](../design/aki_design.md)；
    [DEC-006](../decisions/DEC-006-heyaki-api-contract.md)、
    [DEC-008](../decisions/DEC-008-manager-routing-and-executor-tasks.md)、
    [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)；工程规范
    6.2/8（公开 API/事件 schema 变更行）；总计划 `RULE-02`/`RULE-09`、
    `EXEC-02`/`EXEC-04`、`DOD-04`/`DOD-05`。代码锚点核对：`ManagerPump`
    Handler 先例 `app/application/manager_runtime.hpp:115`；`AppStateOwner`
    构造入参形态 `app/state/app_state_owner.hpp:67`、`drain_updates` 的
    apply/计数 `:211-223`；`MessageDeliveredEvent` 自带 conversation 先例
    `app/state/app_events.hpp:33-36`；`MessageRepository::upsert(message,
    conversation_id)` 调用方提供归属 `persistence/repository/repositories.hpp:74`。
  - 一致性自查（验收 ①）：§10.1（处理器契约：调用时点/顺序/异常策略/拒绝双
    计数）↔ §11.1 ①（写入时机正式落点 + 幂等 no-op 原句 + 容量预算 + 失败
    语义）↔ §11.1 ②（control 先于 owner 的时序与未注册窗口）↔ §8.3（七步
    装配序与其引用的 ②③）↔ §8.1（观察管道触发语义，DEC-006 映射权威）↔
    DEC-009（①② 决策与三项否决备选）逐项交叉核对一致；与 M2-07 已实现形态
    的偏差声明清晰（过渡形态保留于 console 宿主，DEC-009 取代其偏差说明、
    M2 历史记录保持原样；正式落点为 M3-03+ 实现依据，当前代码暂不变）。
    `UpsertMessage` 扩展与 `EXEC-02`（owner 上下文执行）/`EXEC-04`（作业仍由
    worker 串行消费）/`RULE-02`（单写者）无冲突；§6 Message 模型未改动。
  - 链接核验：脚本遍历四份变更文档的相对链接目标逐一核实存在（设计/DEC-009/
    本里程碑/总计划；见下方验证命令输出，全部有效）。
  - 验证：纯文档变更（验收：git status 仅涉设计/决策/计划文档），无构建/测试
    行为改动——`git status --short` 确认改动仅
    `docs/design/aki_design.md`、`docs/decisions/DEC-009-appstate-write-path.md`（新建）、
    `docs/plans/m3-heyaki-integration.md`、`docs/plans/aki-implementation-plan.md`
    五份文档内的前四份加 `docs/plans/m2-local-persistence.md`（一项事实修正：
    M2-07 依据行引用的 DEC-008 文件名错误
    `DEC-008-application-layer.md` → 实际文件
    `DEC-008-manager-routing-and-executor-tasks.md`，链接核验暴露、仅改文件名
    不动内容，沿 M1-08 事实修正先例）；相对链接核验：脚本遍历五份文档抽取
    markdown 相对链接 108 条逐一核实目标存在 → `links checked: 108, broken: 0`
    （修正前 1 条断链即上述 DEC-008 文件名）；第 6 节 Message 模型 diff 为零
    （`git diff docs/design/aki_design.md` 不含 `struct Message` 块改动）。
  - 限制：本项为设计先行契约，实现随 M3-03+ 批次跟进（`AppStateOwner` 构造
    入参、`post_accept_failures` 统计、`UpsertMessage.conversation` 字段、
    console 宿主切换到 §8.3 七步序）；实现若与本契约出现偏差，按 M1-08 纪律
    先更新第 10.1/11.1/8.3 节与 DEC-009 再合代码；MR 闭环由后续环节执行，
    本记录不含 commit/CI 证据。
  - 同步：本里程碑工作项 `M3-02`、总计划当前状态。
