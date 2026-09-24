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
- 2026-09-23：`M2-08` 完成，M2 关闭（Done）：收口审计与退出证据归集——设计-实现审计矩阵逐项一致（§11/11.1/14 与 §8.2/8.3/10.1 对 persistence/ 五子目录与根 main.cpp 宿主组合；两项已知偏差（M2-05 轮询环、M2-07 快照权威值镜像）锚点核实，未记录偏差数 0）；RULE-07/RULE-10 边界 grep 通过（sqlite3* 封死 persistence 唯一编译单元、executor 类型仅接线层、第一方无自建线程）。退出证据：本地 MSVC debug/release 全量 ctest 复跑 19/19；gh 逐 PR 核实 #13~#18 CI 全绿（Linux debug/asan/ubsan + Windows MSVC，#16~#18 含 Linux tsan 覆盖 DOD-03）。`docs/supply-chain/` 创建并登记 SQLite vendored 审计（zip SHA3-256 + 双文件 SHA-256 本地复算一致、public domain 结论）与上游缺陷处置（3.53.4 仍为最新无可升级修复；建议负责人向 sqlite.org 报告，触发条件已登记）；DEC-004 验证方式逐项回填（测试 + PR 映射表）；M2 文档状态 Completed、里程碑索引 M2 → Done。详见 M2 里程碑文档 M2-08 验证记录。
- 2026-09-24：`M3-01` 完成，M3 启动（In Progress）：heyaki 目标级构建接入（`DEC-006`）——依赖树首次拉取（fetch_third_party.sh，ref+commit 双校验，33 项 runtime + googletest/zstd 全部 verified）；单一构建图接入只链 `heyaki::client`（HEYAKI_BUILD_APPS/AUTO_INSTALL=OFF、heyaki 测试不进构建面）；Aki 移除自身 executor add（SYSTEM include 转移到 heyaki 提供的 executor target，构建图单份 executor 实证）；configure 三方 executor commit 一致校验（Aki lock / heyaki lock / checkout）实测；MSVC debug/release 全量 configure/build/ctest 20/20（既有 19 项零回归），OpenSSL DLL 部署集与 LibDataChannelStatic 无需 datachannel.dll 实测；双 SQLite 实测出链接序敏感并以 aki_persistence 先行固化为契约（map 取证 697 sqlite3_* 符号全部来自 Aki vendored 3.53.4、heyaki 副本零符号进入、无行为冲突）；边界锁定用例 test_heyaki_client_surface（RULE-01/10）；tsan 预设联动 HEYAKI_SANITIZER=thread 与 CI 四档（fetch+缓存+OpenSSL）更新随 PR 门禁取证；MinGW configure-only 未达成（失败点前移至 heyaki vendored SQLite 生成，w64devkit 限制扩展）如实记录。详见 M3 里程碑文档 M3-01 验证记录。
- 2026-09-24：`M3-02` 完成（设计先行，纯文档变更，无产品代码）：新建 [DEC-009](../decisions/DEC-009-appstate-write-path.md) 固化持久化写路径正式落点——AppStateOwner 构造注入接受后处理器（accept 后 owner 单写者上下文按接受顺序同步调用、幂等 no-op 同样入队、不抛出 + post_accept_failures 全捕获、容量预算 64×2=128≤DB 通道 256、入队拒绝双计数可见），并同批固化阻塞子决策：UpsertMessage 扩展 conversation 归属字段、第 6 节 Message 模型保持不变；设计第 10.1/11.1①②/8.3/8.1 同步（八步→七步装配序：恢复→control→AppStateOwner(seed+handler)→Manager→注册 worker→RouterSink；control 先于 owner 构造的时序对齐；记录型来源接入与「发现→信任确认」触发语义）。取代 M2-07 镜像过渡形态偏差（M2 历史记录保持原样）；实现随 M3-03+ 按新契约跟进。相对链接全部核验有效。详见 M3 里程碑文档 M3-02 验证记录。
- 2026-09-24：[M4 里程碑文档](m4-image-file-transfer.md) 创建（Planned，`M4-01`~`M4-07`，
  里程碑索引 M4 文档链接更新）。范围：图片消息与文件传输真实数据链路
  （`SCOPE-07`/`SCOPE-08` 非 UI 部分；Transfers 页面归 M5）。前置说明：M3
  工作项已全部完成但里程碑 In Progress（退出-1/退出-3 待防火墙放行/LAN 双端
  环境补跑，见 M3-09 记录）——环境补跑不阻塞 M4 文档创建与设计先行工作项
  （`M4-01`），M4 实现工作项开工时复核 M3 状态。开工前无待冻结决策
  （`DEC-005` 干 M5 开始前）；关键风险已在 M4 文档登记：heyaki BLAKE3 wire
  校验与 `DEC-004` SHA-256 存储哈希关系澄清（`M4-01` 固化）、`DatabaseWorker`
  通道预算复核（`DEC-009` 复核触发条款）、防火墙环境降级纪律沿用。
- 2026-09-24：`M3-03` 完成：本地设备身份真实化（`SCOPE-01`）+ DEC-009 契约首批实现——`AppStateOwner` 构造注入 `PostAcceptHandler`（accept 后 owner 单写者上下文按接受顺序同步调用、幂等 no-op 同样入队、不抛出 + `post_accept_failures` 全捕获；默认空源兼容）；`heyaki/adapter/local_identity.hpp` 接线面（ProfileStore create-or-open + initialize_local + endpoint_for("org.aki.app") + derive_device_id 恒等绑定断言，RULE-10 公开面仅 aki/std 类型）；根 main.cpp 切换 §8.3 七步装配序（镜像形态移除）；本地身份经 UpsertDevice 经处理器落库、二次启动逐字节一致加载。测试：test_app_state 五类断言 + 幂等 no-op 吸收、新建 test_local_identity、restart 恢复身份用例；MSVC debug/release ctest 21/21 零回归，宿主稳定 5 次；DEC-006「规范 hex」措辞按实测（hy1_ 前缀编码串）修正（M1-08）。UpsertMessage 载荷扩展留 M3-05（已在记录登记）。详见 M3 里程碑文档 M3-03 验证记录。
- 2026-09-24：`M3-04` 完成（含如实降级记录）：设备发现与信任真实化首批落地——`heyaki/session/runtime_node.hpp`（§14 目录落地）：借用注入 Runtime/Node（DEC-006：borrowed_executor_not_running 拒绝路径实测；ExecutorConfig 定容；关闭钩子 Node::shutdown + Runtime::shutdown 且 `executor_shutdown_performed==false` + fully_stopped 断言宿主实测）；`heyaki/adapter/lan_discovery.hpp` 观察管道（EXEC-04 timer 首用 submit_periodic_with_handle，endpoints() diff 合成 discovered（含公钥指纹），trusted 不重放，stop 后零事件实测）；DOD-02 六项沿 periodic 路径全过。**如实降级**：本机防火墙拦截至端 TLS 入站，会话停滞 authenticating——配对→信任全链路与 RULE-08 断言未在本机走通，测试以 [skip] 证据路径通过并登记补跑条件（防火墙放行/LAN 双端）；同 executor 双借用 Runtime asio_worker_start_failed 实测（per-node 测试形态，借用语义不变）。debug/release ctest 22/22 零回归。详见 M3 里程碑文档 M3-04 验证记录。
- 2026-09-24：`M3-05` 完成（含如实降级记录）：文本消息真实化——DEC-009 ② 正式落地（UpsertMessage 载荷新增 conversation 归属字段 + owner FK 前置校验 + 全仓修正 + M3-03 临时簿记移除）；SPI 第 10 方法 on_message_send_failed（设计 §8.1 先行修订）+ RouterSink/MM Failed 终态映射（映射断言经 test_heyaki_adapter.cpp 的 BridgeSink 测试桥接；Fake 不实现 Sink）；NodeSession 消息面（aki.text 信封、MessageId 规范字符串双射——DEC-006 措辞澄清、入站/ack 映射）；tests/unit/test_heyaki_message.cpp：双射/信封往返（网络无关 unit 二进制，评审拆分自回环文件——[skip] 受控退出不得掩盖同二进制既有失败）+ test_message_loopback 双节点收发端到端因本机防火墙拦截至端 TLS 以 [skip] 证据路径降级（补跑条件登记）；失败面路由测试驱动经评审补齐（RouterSink 第 10 路由、MM 失败回报用例、BridgeSink 失败面用例）。debug/release ctest 24/24 零回归（评审修正后复测）。详见 M3 里程碑文档 M3-05 验证记录。
- 2026-09-24：`M3-07` 完成（含如实降级记录）：断线恢复（`SCOPE-11`）——`app/application/reconnect_loop.hpp` 重连协调器（EXEC-05 长任务 submit_cancellable + StopToken 可中断切片等待、有界 recovery_budget 超时如实计数、回调异常 future 结算 + callback_failures、per-peer 单飞、stop_all 取消并消费在途 future 零悬挂——DEC-008 宿主关闭纪律）；NodeSession 增补 restart_session/close_lan 与 session_id/epoch 可观测（DEC-006 映射 6）；DOD-02 六项沿重连长任务路径（网络无关单测 test_reconnect_loop 6 用例，含排队期/运行期取消两形态语义）。集成回环以 close_lan 合成断开（真实中断不可编程触发，偏差登记），本机防火墙拦截下以 [skip] 证据路径降级（补跑条件登记）。debug/release ctest 29/29 零回归。详见 M3 里程碑文档 M3-07 验证记录。
- 2026-09-24：`M3-08` 完成（含如实降级记录）：双端真实链路验证与宿主切换——统一真实 Adapter `heyaki/adapter/heyaki_node_adapter.hpp`（SPI 十方法 + 发现/peer_sessions/消息分件组装 + SPI 同接口 static_assert 编译期锁定；2026-09-24 评审修正：断线重连职责回收至组合根协调器——Adapter 不再内嵌 M3-07 协调器，heyaki/adapter 零 app/application include（RULE-01/DEC-002 层向），仅经 SPI on_device_disconnected 如实上报断开；析构闭合补强：停管道 + 中和 session 消息 handler（this 捕获生命周期闭合）；入站面升为公开注入与 Fake inject_* 对称）；宿主 main.cpp 切换真实 Adapter（Fake 移至测试目标；presence/path 观察关闭保确定性；真实发现启停实测；本地行恢复断言；交互链路 [degraded] 证据输出；评审修正：遗留死代码清理——enum_text×5/event_type_name/find_*×3/bytes_of 九个无引用函数 + 未调用 consume_events/next_sequence + 无符号恒真断言（GCC -Wtype-limits -Werror CI 必挂项，MSVC /W4 不告警））；test_heyaki_node_adapter（评审补证新建：SPI/注入面网络无关单测 5 用例 49 断言——构造校验/出站校验/sink 分发/无 sink 静默/析构闭合回归守卫）；test_real_adapter_loopback（单用例回环：配对信任 → Conversation → SPI 出站（评审修正：断言改经 send_text_message，原 NodeSession 直呼 SPI 未被执行）→ 消息行 conversation 归属列 SQL → 重启恢复；防火墙受限 [skip] 降级，补跑条件登记）。debug/release ctest 31/31 零回归（评审修正后复测）。详见 M3 里程碑文档 M3-08 验证记录。
- 2026-09-24：`M3-09` 完成（收口审计与退出证据归集/处置；2026-09-24 评审修正：退出-1/退出-3 核心验收——双端交互闭环、真实断线恢复回环——因防火墙拦截 TLS 入站未在本机/CI 执行，按工程规范 §4 勾选规则恢复未勾选（降级说明/负责人/补跑条件保留），M3 里程碑关闭条件未满足）：设计-实现审计矩阵逐项一致（§3~8/10/10.1/11.1/14 与 DEC-006 对 heyaki/adapter+session、app/state+application+lifecycle、根 main.cpp；六项已知偏差锚点核实，未记录偏差数 0）；RULE-07/10 边界 grep 通过。退出证据：本地 MSVC debug/release 全量 ctest 复跑 31/31；gh 逐 PR 核实 #21~#28 CI 五档全绿（Linux debug/asan/ubsan/tsan + Windows MSVC）。退出-1/3 双端交互与断线恢复回环因防火墙拦截 TLS 入站沿既定纪律降级为 [skip] 证据路径（网络无关半边已验证；补跑条件两条登记：防火墙放行入站 TCP/专用测试网络、LAN 双端真机）。发现来源分期（Relay/邀请链接/手动输入）与 M5 补做条件（占位口令/secret backend/DeviceIdentity 元数据）登记；M3 文档状态 In Progress、里程碑索引 M3 → In Progress（M3-09 范围保持完成；M3 关闭待退出-1/3 补跑后复核，如需缩小退出口径须先经决策记录重新划界 §14）。详见 M3 里程碑文档 M3-09 验证记录。
- 2026-09-24：`M3-06` 完成：Presence 与连接路径（`SCOPE-10`）——`heyaki/adapter/peer_sessions_pipeline.hpp`：peer_sessions diff 管道（EXEC-04 timer + TimerHandle 持有，stop 后零回调实测；映射 DEC-006 映射 5 四值 + direct_host+relay→P2p 补充分支；authenticated↔closed → connected/disconnected、路径变化 → connection_path_changed 纯函数网络无关单测）；宿主 main.cpp 装配（RouterSink 双 Manager 扇出接线，构造不 start——smoke 确定性，启动随 M3-08）；SetPresence/SetConnectionPath 不持久化语义保持（§11.1①）；独立 unit 二进制 test_peer_sessions_pipeline（映射/diff/中间态翻动零事件，18 断言）。集成回环为独立单用例二进制 test_peer_sessions_loopback（2026-09-24 评审拆分：原与 M3-04 用例同二进制且各自可 [skip] 受控退出——互相掩盖既有失败并使回环证据随机不可达（实测三连跑各仅其一执行）；沿 M3-05 先例一拆三：DOD-02 专项 + test_discovery_pairing_loopback + test_peer_sessions_loopback），因本机防火墙拦截至端 TLS 以 [skip] 降级（补跑条件登记）。debug/release ctest 27/27 零回归（拆分后复测）。详见 M3 里程碑文档 M3-06 验证记录。
- 2026-09-23：[M3 里程碑文档](m3-heyaki-integration.md) 创建（Planned，`M3-01`~`M3-09`，
  里程碑索引 M3 文档链接更新）。范围：pinned heyaki 真实接入与文本消息
  （`SCOPE-01/02/03/05/06/10/11`）。开工前必须完成：`DEC-006`（Heyaki API 契约
  版本，`M3-01` 冻结）；两项调研——`ExecutorOwner` 与 heyaki 库内 executor 的
  生命周期协调（EXEC-01 唯一 owner 纪律，缺口走 9.4 台账）、第 11.1 节 ① 写路径
  正式落点（接受后回调或 owner 侧 tap，替代 M2-07 宿主快照镜像，`M3-02` 固化）。
- 2026-09-23：三项 M3 开工前调研完成，[DEC-006](../decisions/DEC-006-heyaki-api-contract.md)
  冻结为 `Accepted`（Heyaki API 契约版本与目标级集成方式）：pinned v1.0.1-38
  （`e114508a`）公开头文件 + api.md/client-library.md 为契约基线（wire {1,3}）；
  单一构建图 `add_subdirectory(third_party/heyaki)` 只链 `heyaki::client`，executor
  target 由 heyaki 子目录提供（双侧同 pin `74a94198`，Aki 移除自己的 executor
  add，DEC-003 executor 接入条款显式修订）；运行期
  `Runtime::create_borrowed(ExecutorOwner.executor())` 注入，EXEC-01 唯一 owner
  保持（非能力缺口，不进 9.4 台账）；§8.1 SPI 九事件映射（消息类走推送回调、
  发现/Presence/路径类由 Adapter 轮询 `endpoints()`/`peer_sessions()` diff 合成）
  与冻结常量（`application_id="org.aki.app"`、`"aki.text"`、`message.send` scope、
  ID hex 双射）落档；LanPresence 元数据缺口如实记录（M3 占位）。两项
  kind=research 结论已写入 M3 里程碑文档：executor 协调采 borrowed 注入
  （`Node::shutdown`+`Runtime::shutdown` 编入 EXEC-01 步骤 1 钩子，断言
  `executor_shutdown_performed==false`）；写路径正式落点选「AppStateOwner 接受后
  回调」（对齐 ManagerPump Handler 先例，M3-02 先更新 §10.1/§11.1① 再动代码，
  阻塞子决策——UpsertMessage 的 conversation_id 归属缺口——须同批固化）。设计
  第 8.1/8.2 节已补 DEC-006 映射与协调锚点。M3 可开工（`M3-01`）。

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
| M2 | 本地持久化 | Done | M1 | v0.2.0 | [m2-local-persistence.md](m2-local-persistence.md) |
| M3 | Heyaki 真实接入与文本消息 | In Progress | M1、M2、DEC-006 | v0.3.0 | [m3-heyaki-integration.md](m3-heyaki-integration.md) |
| M4 | 图片消息与文件传输 | Planned | M3 | v0.4.0 | [m4-image-file-transfer.md](m4-image-file-transfer.md) |
| M5 | EUI-NEO UI 与 MVP 验收 | Planned | M2、M3、M4、DEC-005 | v0.5.0（MVP） | 待创建 |

依赖说明：M1 先以契约与假实现交付可运行的领域骨架（先契约后实现、先假实现后真实依赖）；
M3 引入真实 Heyaki；M5 整合 UI 并按设计第 15 节逐项验收 MVP。每个里程碑必须产生可独立
验收的能力增量，对应 tag 与里程碑文档“建议发布点”一一对应。

## 尚未冻结的决策（暂定默认值）

| 编号 | 主题 | 暂定默认值 | 负责人 | 最迟冻结里程碑 |
| --- | --- | --- | --- | --- |
| `DEC-005` | EUI-NEO 集成方式 | 源码/子模块引入 + CMake target，不用 WebView | Linductor | M5 开始前 |

`DEC-005` 在冻结时创建正式决策记录文件；已生效决策见
[docs/decisions/](../decisions/)（含已冻结的 [DEC-003](../decisions/DEC-003-dependency-locking.md)、
[DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)（2026-09-22 提前冻结）、
[DEC-006](../decisions/DEC-006-heyaki-api-contract.md)（2026-09-23，Heyaki API 契约
版本与目标级集成方式——pinned v1.0.1-38 公开面为契约基线、单一构建图接入
heyaki::client、executor 由 heyaki 子目录提供同 pin、borrowed Runtime 注入保持
EXEC-01 唯一 owner、SPI↔API 映射与冻结常量；含 DEC-003 executor 接入条款修订）、
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
