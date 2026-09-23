# Aki 实施总计划

> 状态：Active
> 负责人：Linductor
> 更新日期：2026-09-23
> 设计依据：[Aki 设计方案](../design/aki_design.md)
> 协作约束：[AGENTS.md](../../AGENTS.md)、[项目管理与工程规范](../project/project-standards.md)

## 当前状态

- 2026-09-21：项目完成初始化（M0 工程骨架与协作基线）。M0 剩余：CI 基线（首次 push
  后）与提交前 `git status` 复核。
- 2026-09-21：executor、EUI-NEO、heyaki 三个依赖已完成 submodule + 锁文件登记与
  configure 校验，[DEC-003](../decisions/DEC-003-dependency-locking.md) 冻结为
  `Accepted`，`RISK-2026-001` 解除。
- 2026-09-21：[UI 设计规范](../design/aki_ui_design.md)沉淀完成（综合
  zai-org/ZCode Design System 与 pinned EUI-NEO 令牌体系），作为 `SCOPE-12` 与
  `RISK-2026-002` 的前期输入；组件能力实际运行验证仍留待 M5。
- 2026-09-21：M1 启动。`M1-01`（领域状态机与类型）与 `M1-07`（[DEC-007](../decisions/DEC-007-test-framework.md)
  测试框架）完成并通过 debug/release 全量测试；设计文档第 3~6 节已补充状态集合
  枚举。本机 MinGW 工具链无 sanitizer 运行时，ASAN/UBSAN 证据待 Linux CI 补跑
  （`RISK-2026-003`）。详见 [M1 里程碑文档](m1-domain-state.md)。
- 2026-09-22：PR #1 合入（`9ad1eb7`），CI 门禁建立且首轮 4/4 全绿（Linux
  debug/asan/ubsan + Windows MSVC debug），`RISK-2026-003` 解除，M0 关闭（Done）。
  标准 MR 闭环（分支 -> PR -> CI -> Squash 合并 -> 清理分支 -> 同步 master）经用户
  确认固化，见 AGENTS.md 与工程规范 10.4。后续变更一律走该闭环。
- 2026-09-22：UI 设计约束经用户确认改为**直接采用** ZCode Design System：原文
  归档为 [docs/design/zcode-design-system.md](../design/zcode-design-system.md)
  （上游 commit `872ad960`，Apache-2.0），[Aki UI 设计规范](../design/aki_ui_design.md)
  重写为其在 EUI-NEO 上的绑定映射，作为 `SCOPE-12` 与 `RISK-2026-002` 的输入；
  组件能力实际运行验证仍留待 M5。
- 2026-09-22：`M1-02` 完成：Application State 单写者边界与设计第 10 节 9 类事件模型
  落地于 `app/state/`，跨上下文交付映射 pinned executor `executor::comm`（`EXEC-03`：
  `DoubleBuffer<AppState>` 一致快照 / `LatestMailbox` 单值最新状态 / `Topic` 观察者
  广播 / `MpscChannel` 必达事件与汇聚）；设计第 10.1 节先行固化 comm 语义映射与三条
  硬约束；pinned executor 库完成 M1 目标级接入（关闭其 tests/examples 与 GPU 探测）。
  [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md) 随调研提前冻结为
  `Accepted`。本地 MSVC debug/release ctest 7/7 通过；本地 MinGW 默认生成器受 pinned
  executor 构建缺陷阻塞（限制与补跑条件见 M1 验证记录）；ASAN/UBSAN 证据随下次 CI
  门禁提供。详见 [M1 里程碑文档](m1-domain-state.md)。
- 2026-09-22：`M1-03` 完成：Heyaki Adapter SPI（`HeyakiAdapter` 出站 + 
  `HeyakiAdapterSink` 入站，9 个 Sink 方法与设计第 10 节事件一一对应）与
  `FakeHeyakiAdapter`（`EXEC-02` 有界校验 + 投递式 `inject_*` 注入）落地于
  `heyaki/adapter/`，仅依赖第 3~7 节领域类型（`RULE-01`/`RULE-10`），未接入
  `third_party/heyaki`（`DEC-003`，M3 才目标级集成）；设计第 8.1 节先行固化 SPI
  契约。本地 MSVC debug/release ctest 8/8 通过；MinGW 限制与 ASAN/UBSAN 补跑条件
  沿用 `M1-02` 记录。详见 [M1 里程碑文档](m1-domain-state.md)。
- 2026-09-22：`M1-04` 完成：`app/lifecycle` 的 `ExecutorOwner` 生命周期 owner 落地
  （设计第 8.2 节先行固化）——独立实例持有 pinned executor `Executor` facade，
  `EXEC-01` 五步受控关闭逐一实现且可观察（`ShutdownResult::Completed` +
  `lifecycle==Stopped` + `wait_timeout_count==0`），blocking worker 句柄由 owner
  持有（M1-05/M2 预留）；shutdown 测试覆盖五类断言与 DOD-02 六项（含阻塞 worker
  wakeup 解除阻塞契约与 owner 等待预算耗尽的如实记录）。本地 MSVC debug/release
  ctest 9/9 通过；MinGW 限制与 sanitizer 补跑条件沿用 `M1-02` 记录。
  详见 [M1 里程碑文档](m1-domain-state.md)。
- 2026-09-22：`M1-05` 完成：`app/application/` 四 Manager 骨架（Device / Conversation
  / Message / Transfer）与 `RouterSink` 落地（设计第 8.3 节先行固化）——9 类 Sink
  事件按 [DEC-008](../decisions/DEC-008-manager-routing-and-executor-tasks.md) 路由表
  投递各 Manager 私有有界收件箱（`EXEC-02`），Manager 以单飞有界排空泵
  （`submit_auto` + 消费 future）在自身执行上下文串行处理，并经既有
  `AppStateOwner` 的 `MpscChannel` 更新指令更新 Application State（只投递不直写
  快照，`RULE-02`/`EXEC-03`）；传输会话长任务 `submit_cancellable` + StopToken，取消
  经 `request_task_cancel`（`EXEC-05`/`EXEC-07`；M1 无 TimerHandle/周期任务，
  blocking worker 不启用）；新增 `SetPresence`/`SetDeliveryState`/`CompleteTransfer`
  三类 typed 更新（设计第 10.1 节先行固化）。本地 MSVC debug/release ctest 10/10
  通过（新 `test_app_managers` 含 DOD-02 六项与迟到事件
  不复活终态；合入版 10 test case / 314 断言，M1-08 审计实测）；MinGW 限制与 ASAN/UBSAN 补跑条件沿用 `M1-02` 记录。
  详见 [M1 里程碑文档](m1-domain-state.md)。
- 2026-09-22：`M1-06` 完成：根 `main.cpp` console 冒烟宿主落地（设计第 8.3 节
  组合根的进程内实现，进程内 owner 自本项起为正式 `ExecutorOwner`）——两台假设备
  （local-1 / alpha-01）经 FakeHeyakiAdapter 完成"发现 → 信任（Pending→Trusted，
  宿主经 `UpsertDevice` 模拟用户确认，设计第 8.3 节先行补充该语义）→ 文本消息
  （send_text + delivered/received，主路径序列号 1~6 FIFO 断言）→ 断开 → 重连
  （同一会话回到 Active、历史保持，`RULE-06`）"，按 §8.3 钩子顺序受控关闭并以
  `fully_stopped` 证据收尾；ctest 注册 `smoke.device_lifecycle` 为 `integration`
  标签（自本项启用，M1 退出-1 通过），`skeleton.app_runs` 保持兼容。本地 MSVC
  debug/release ctest 11/11 通过；宿主 debug 连续 50 次输出逐字节一致、release
  50 次全 PASS（确定性可复现）；MinGW 限制与 ASAN/UBSAN 补跑条件沿用
  `M1-02` 记录。详见 [M1 里程碑文档](m1-domain-state.md)。
- 2026-09-23：`M1-08` 完成，**M1 关闭（Done）**：收口审计逐项校对设计第
  3~7/8.1/8.2/8.3/10/10.1/14 节对 `device/`、`conversation/`、`transfer/`、
  `heyaki/adapter/`、`app/lifecycle/`、`app/application/`、`app/state/` 与根
  `main.cpp`，结论全部一致；四项已知偏差均有设计/决策/工作项锚点，未记录偏差
  数为 0；RULE-01/07/10 与 EXEC-04 M1 边界 grep 抽查通过。退出-2~5 证据归集：
  六项并发基线沿四条路径（test_app_state/heyaki_adapter/executor_lifecycle/
  app_managers 共 26 个 dod02 用例）映射并通过；四个状态机单测（193 断言）+
  Manager 路径 RULE-08 用例通过；本地 MSVC debug/release 全量 ctest 11/11；
  ASAN/UBSAN 由 PR #8/#9 的 Linux CI 四项检查 SUCCESS 覆盖全部 M1 代码（gh 核实）。
  [M1 里程碑](m1-domain-state.md)状态置 `Completed`，里程碑索引置 `Done`；
  v0.1.0 发布点就绪，tag 创建待用户授权。事实修正：`test_app_managers` 计数
  以 PR #8 合入版为准（10 test case / 314 断言）。详见 [M1 里程碑文档](m1-domain-state.md)
  M1-08 验证记录。
- 2026-09-23：M2 启动，`M2-01` 完成（设计先行，纯文档变更）：设计新增第 11.1 节
  「持久化集成契约（M2 契约）」固化四类契约——DB 写路径映射（typed 更新→表作业、
  owner 接受后单写者上下文入队、`DatabaseWorker` 串行保序、失败可见不静默，
  `RULE-09`/`EXEC-06`）、启动恢复流程（主线程同步 open→迁移→逐域加载→tmp 清扫→
  以 `AppStateOwner` 构造入参播种初始快照，恢复期 Adapter 事件未启动）、
  `DatabaseWorker` 排空位于第 8.3 节钩子序列末尾（`close()` 之后、`EXEC-01`
  步骤 2/3 之前）、文件本体终态作业（SHA-256+原子改名/`.part` 幂等删除）全部在
  blocking worker 内执行；数据根目录解析为 persistence 层平台条件编译单元最小
  落点（公开面仅 `std::string`，`RULE-10`）。第 8.3 节补钩子序列前向引用；
  [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md) 过时括注修正。
  [M2 里程碑](m2-local-persistence.md)状态置 `In Progress`。详见 M2 里程碑文档
  M2-01 验证记录。

- 2026-09-23：`M2-02` 完成：vendored SQLite 接入——官方 amalgamation
  （`2026/sqlite-amalgamation-3530400.zip`，下载前校验 SHA3-256
  `628a44cf…934e` 与 DEC-004/sqlite.org 下载页一致）仅取 `sqlite3.c`/`sqlite3.h`
  入 `third_party/sqlite/`；锁文件新增 `class=vendored` 条目（版本 3.53.4、双文件
  SHA-256、zip 溯源字段）；`cmake/Dependencies.cmake` 按 `class` 分支（pinned 三条
  校验行为与 STATUS 输出不变，vendored 逐文件 `file(SHA256)`，缺失/漂移即
  `FATAL_ERROR` + 修复提示）；`sqlite3` 静态库 target（`SQLITE_DQS=0`/
  `SQLITE_OMIT_LOAD_EXTENSION=1`，不继承第一方告警级别）；asan/ubsan/tsan 预设补
  `CMAKE_C_FLAGS`（sqlite3.c 为 C 编译单元）；新增 `test_sqlite_sourceid`
  （header/libversion/锁文件版本三方一致断言）。验证：MSVC debug/release
  ctest 12/12；负向篡改锁文件哈希 → configure FATAL_ERROR 含修复提示，还原通过；
  MinGW configure-only 通过；GCC -Werror 语法检查通过。sqlite3.c 随 asan/ubsan
  的编译由本 PR Linux CI 门禁提供（本机无 sanitizer 运行时，限制沿用 M1-02
  记录）。详见 M2 里程碑文档 M2-02 验证记录。

- 2026-09-23：`M2-04` 完成：v1 schema（设计第 11 节 ER 四表 + DEC-004 列补齐，
  枚举 INTEGER+CHECK，transfer 含文件回写位）注册为迁移框架首个正式迁移；
  仓储层（`persistence/repository/`）提供四表与 M1 领域类型双向转换，API 对齐
  设计第 11.1 节 ①④（整行 upsert / 进度列更新 / 送达状态列更新 / 终态更新含
  回写位）；prepared-statement LRU 缓存（容量 16 可配，满时逐出）；重要发现：
  SQLite 3.53.4 对“约束失败语句 reset 复用”存在异常终止缺陷（官方 DLL 复现，
  非本仓库构建问题），仓储以“错误语句逐出缓存”统一规避。本地 MSVC
  debug/release ctest 15/15（+`test_persistence_repository` 11 test case /
  108 断言，初稿误记 10/103 以实测为准）；ASAN/UBSAN 随本 PR Linux CI；MinGW
  限制沿用 M1-02 记录。
  详见 M2 里程碑文档 M2-04 验证记录。

- 2026-09-23：`M2-05` 完成：`DatabaseWorker` 落地（blocking worker 首次启用，
  设计第 8.2/8.3/11.1 节落点）——公开控制面 `DatabaseWorkerControl`（enqueue
  明确拒绝/排空/协作退出/计数，公开头无 sqlite/executor 类型）+ 注册侧
  `DatabaseWorkerRunnable`（IBlockingIoWorker，独占连接经 M2-04 仓储串行
  消费，try_receive+短睡眠轮询环等待）；经 `ExecutorOwner::
  start_blocking_worker` 注册（EXEC-07），关闭顺序并入 EXEC-01（钩子内
  drain→close，先于步骤 2/3）。DOD-02 六项沿 worker 路径全覆盖（M2 退出-2）。
  本地 MSVC debug/release ctest 16/16（+`test_database_worker` 8 test case /
  102 断言，debug 60 次/release 30 次稳定；初稿误记 7/87 以实测为准）；发现并规避 pinned executor
  v0.5.0-7 `receive_for` 在 FK 约束失败语句复用场景的作业不可见/进程异常
  终止问题（改用轮询环，调研备选 ⑧；配合 M2-04 的错误语句逐出），非能力
  缺口不进 9.4 台账。ASAN/UBSAN 随本 PR Linux CI；MinGW 限制沿用 M1-02
  记录。详见 M2 里程碑文档 M2-05 验证记录。

- 2026-09-23：`M2-03` 完成：persistence 薄 RAII 封装与迁移框架落地
  （`persistence/database`：`SqliteError`/`Database`/`Statement`/`Transaction`，
  open 期 pragma `journal_mode=WAL`、`synchronous=NORMAL`、`foreign_keys=ON`、
  `busy_timeout`（默认 5000ms）；`persistence/migration`：`user_version`
  版本化迁移——每步独立事务、失败回滚不前进、幂等 no-op、连续性构造期校验、
  库新于已知步骤干净失败）。`aki_persistence` 转实体静态库且 sqlite3 改
  PRIVATE 链接（`<sqlite3.h>` 仅在 database.cpp 一处，消费者无 sqlite include
  路径——RULE-10 编译级边界由 `test_persistence_public_surface` 锁定）。
  本地 MSVC debug/release ctest 14/14（+`test_persistence_database` 11 test
  case / 81 断言、`test_persistence_public_surface`）；同步封装无并发路径
  （DOD-02 六项随 M2-05 DatabaseWorker）。ASAN/UBSAN 随本 PR Linux CI；
  MinGW 限制沿用 M1-02 记录。详见 M2 里程碑文档 M2-03 验证记录。

- 2026-09-23：`M2-06` 完成：文件本体存储布局与生命周期落地
  （`persistence/storage/`）——数据根目录解析（平台条件编译单元：
  Windows %APPDATA% / POSIX XDG，公开面仅 std::string，RULE-10）；
  `FileStore`：DEC-004 布局（files/ + files/tmp/）、.part 分块流式写入、
  Completed 终态作业组（流式 SHA-256 + 原子改名 + hash/size/relative_path
  回写 TRANSFER 行）、Failed/Cancelled 幂等删除、启动清扫（活动保留/
  残留清理）；TransferId `[A-Za-z0-9_-]{1,64}` 入口校验与磁盘名净化
  （穿越载荷不落盘）。作业经 DatabaseWorker 串行执行（DbJob 工厂）。新增
  `test_file_store`（8 test case / 68 断言）。本地 MSVC debug/release
  ctest 17/17；file_store debug 40 次稳定；ASAN/UBSAN 随本 PR Linux CI；
  POSIX 分支随 CI Linux 编译执行（本机 Windows 不宣称已验证）；MinGW
  限制沿用 M1-02 记录。详见 M2 里程碑文档 M2-06 验证记录。
- 2026-09-23：`M2-07` 完成：重启恢复与宿主/测试写路径落地（`SCOPE-09` /
  M2 退出-1）——`persistence/recovery/perform_startup_recovery`：主线程同步
  组合（数据根注入 → open（损坏即 SqliteError 干净失败）→ user_version 迁移
  → 四仓储逐域加载 → sweep_tmp_orphans 按加载活动行清扫）；根 `main.cpp`
  组合根按设计第 11.1 节 ②③ 接线：initialize 后同步恢复 → 加载结果经
  AppStateOwner 构造入参播种初始快照 → 注册 DatabaseWorker 与 RouterSink
  （EXEC-02 启动段纪律）；写路径 `PersistenceMirror` 在 owner 单写者上下文
  按接受顺序镜像 typed 更新为 DbJob（§11.1 ①，域级工厂 update_jobs；
  SetPresence/SetConnectionPath 不持久化）；关闭钩子末尾 `close()` 之后
  request_drain + 有界等待（排空先于 EXEC-01 步骤 2/3，宿主路径复验
  admitted==completed 零丢失）；进程内 session B 重开逐域断言一致（设备/
  信任/会话/消息含送达终态/传输历史 + stored_* 回写位）。新增
  `test_restart_recovery`（integration 标签，4 test case / 158 断言）与
  损坏 DB 宿主级 ctest（`--exact` 模式，FATAL 输出原因 + 退出码 2）。设计
  §14 目录树同步（补记 storage/ 并新增 recovery/）。本地 MSVC debug/release
  ctest 19/19；宿主与恢复测试 debug 30 次 / release 15 次稳定；无新并发
  路径（DOD-02 六项已随 M2-05 覆盖）；ASAN/UBSAN 随本 PR Linux CI；MinGW
  限制沿用 M1-02 记录。详见 M2 里程碑文档 M2-07 验证记录。

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
| M0 | 工程骨架与协作基线 | Done | 无 | 无（仓库基线） | [m0-project-skeleton.md](m0-project-skeleton.md) |
| M1 | 领域模型与状态边界 | Done | M0（依赖来源解锁） | v0.1.0 | [m1-domain-state.md](m1-domain-state.md) |
| M2 | 本地持久化 | In Progress | M1 | v0.2.0 | [m2-local-persistence.md](m2-local-persistence.md) |
| M3 | Heyaki 真实接入与文本消息 | Planned | M1、M2、DEC-006 | v0.3.0 | 待创建 |
| M4 | 图片消息与文件传输 | Planned | M3 | v0.4.0 | 待创建 |
| M5 | EUI-NEO UI 与 MVP 验收 | Planned | M2、M3、M4、DEC-005 | v0.5.0（MVP） | 待创建 |

依赖说明：M1 先以契约与假实现交付可运行的领域骨架（先契约后实现、先假实现后真实依赖）；
M3 引入真实 Heyaki；M5 整合 UI 并按设计第 15 节逐项验收 MVP。每个里程碑必须产生可独立
验收的能力增量，对应 tag 与里程碑文档“建议发布点”一一对应。

## 尚未冻结的决策（暂定默认值）

| 编号 | 主题 | 暂定默认值 | 负责人 | 最迟冻结里程碑 |
| --- | --- | --- | --- | --- |
| `DEC-005` | EUI-NEO 集成方式 | 源码/子模块引入 + CMake target，不用 WebView | Linductor | M5 开始前 |
| `DEC-006` | Heyaki API 契约版本 | 以 M3 启动时 pinned 版本公开 API 为准 | Linductor | M3 开始前 |

`DEC-005`、`DEC-006` 在冻结时创建正式决策记录文件；已生效决策见
[docs/decisions/](../decisions/)（含已冻结的 [DEC-003](../decisions/DEC-003-dependency-locking.md)、
[DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)（2026-09-22 提前冻结）、
[DEC-007](../decisions/DEC-007-test-framework.md)
与 [DEC-008](../decisions/DEC-008-manager-routing-and-executor-tasks.md)
（2026-09-22，Manager 职责切分、事件路由与 Executor 任务承载））。

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
| `RISK-2026-003` | Resolved (2026-09-22) | 本机 Windows/MinGW 工具链对 sanitizer 支持有限 | 曾致 TSAN/部分 ASAN 证据缺失 | Linductor | 已解除：Linux CI 门禁建立（`.github/workflows/ci.yml`，PR #1 首轮 asan/ubsan 全绿），sanitizer 证据由 CI 常规提供 |

2026-09-21 复核 `RISK-2026-003`：本机 w64devkit GCC 15.2 工具链未随附 sanitizer
运行时，asan preset configure 即失败（`cannot find -lasan`），证据见
[M1 验证记录](m1-domain-state.md)。
