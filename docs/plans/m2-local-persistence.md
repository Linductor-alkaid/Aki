# M2：本地持久化

> 状态：Completed
> 负责人：Linductor
> 所属计划：[Aki 实施总计划](aki-implementation-plan.md)
> 前置：M1（已关闭，`app/lifecycle`/`app/state`/`app/application` 骨架就绪；
> [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md) 已于 2026-09-22
> 提前冻结，本里程碑直接依据其执行）
> 建议发布点：v0.2.0
> 更新日期：2026-09-23

## 目标

交付 `SCOPE-09` 的本地持久化能力：按 [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)
以 SQLite（官方 amalgamation，vendored 引入）保存设备身份、受信设备、Conversation
metadata、消息历史与 Transfer history，DB 访问经 Executor blocking worker 承载
（`EXEC-04`，[设计第 8.2 节](../design/aki_design.md)标注的首次启用里程碑）；
落实文件本体存储布局与生命周期纪律；实现重启后恢复。本里程碑不接触真实网络。

## 范围与非目标

### 范围

- vendored SQLite 接入：amalgamation（锁定 3.53.4）并入 `third_party/sqlite`，
  锁文件 `class=vendored` + SHA-256 登记，configure 校验分支（`DEC-004`、`DEC-003`）。
- persistence 薄 RAII 封装（`Database` / `Statement` / `Transaction` 守卫 +
  `SqliteError`）与 `user_version` 版本化迁移框架；`sqlite3*` 封死在 persistence
  层（`RULE-10`）。
- 设计第 11 节 ER 模型 4 张表（DEVICE / CONVERSATION / MESSAGE / TRANSFER）schema、
  领域枚举 `INTEGER + CHECK` 映射与仓储层。
- `DatabaseWorker`：单一 blocking worker 独占连接、有界工作通道串行消费 DB 作业
  （`EXEC-04`），取消经 StopToken 在语句间检查，admission 拒绝与失败经 Executor
  监控设施可观察（`EXEC-06`/`RULE-09`），shutdown 按 `EXEC-01` drain 后 join。
- 文件本体布局与生命周期：`files/tmp/<transfer_id>.part` 写入、终态 `Completed`
  流式 SHA-256 + 原子改名、`Failed` / `Cancelled` 幂等删除、启动清扫残留
  （`TransferId` 字符集约束与路径净化，`DEC-004`）。本里程碑以测试内字节源驱动
  该路径（真实传输数据链路在 M4）。
- 重启恢复：写入 → 关闭 → 重开，设备/信任/会话/消息/传输历史一致；DB 损坏时
  open 干净失败。

### 非目标

- 真实 Heyaki 接入与网络断线恢复（`SCOPE-11` 归 M3/M4；本里程碑的"恢复"指进程
  重启后的持久化恢复）。
- 图片/文件传输的真实数据链路与传输页面（M4 / `SCOPE-07`/`SCOPE-08`），本里程碑
  仅落实存储布局与传输历史记录的持久化语义。
- EUI-NEO UI（M5）；设计第 11 节提及的应用设置存储（`SCOPE-09` 未含，出现真实
  需求时按 `DEC-004` 流程扩展 schema 并补充决策）。
- 内容寻址去重（`DEC-004` 已记为带触发条件的延后项）。

## 设计与决策依据

- [Aki 设计方案](../design/aki_design.md)第 8.2 节（blocking worker 自 M2 起按负载
  启用）、第 8.3 节（首次启用预期为 M2 历史读写 I/O）、第 11 节（存储划分与 ER
  模型）、第 14 节（`persistence/database`、`persistence/repository`、
  `persistence/migration` 目录）。
- [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)：引擎与访问方式、
  vendored 引入与 SHA-256 校验、`DatabaseWorker` 承载、文件本体布局、迁移纪律、
  M2 验证方式（本里程碑退出条件的直接依据）。
- [DEC-003](../decisions/DEC-003-dependency-locking.md)：锁定 + configure 校验、
  漂移即失败纪律；[DEC-001](../decisions/DEC-001-cpp-baseline.md)：预设矩阵与
  测试标签；[DEC-007](../decisions/DEC-007-test-framework.md)：`unit` /
  `integration` 标签。
- AGENTS.md Executor 强制规则与工程规范第 9 节；blocking worker 首次启用前按
  pinned executor 集成指南加载对应 capability card（blocking I/O 路由）。

## 工作项

- [x] `M2-01` 补充设计第 11 节持久化集成契约小节：DB 写路径由哪些 Manager 动作
  触发（对应第 10.1 节 typed 更新）、启动恢复流程与 Application State 的关系、
  `DatabaseWorker` 在 `EXEC-01` 关闭顺序中的落点；偏差先更新设计再合代码
  （M1-08 纪律）。（2026-09-23：设计新增第 11.1 节「持久化集成契约（M2 契约）」
  固化四类契约——① DB 写路径映射（typed 更新→表作业、owner 接受后单写者上下文
  入队、串行保序、RULE-09 失败语义；SetPresence/SetConnectionPath 不持久化）；
  ② 启动恢复流程（主线程同步：解析数据根→open→迁移→逐域加载→tmp 清扫→以
  `AppStateOwner` 构造入参播种初始快照；恢复期无并发源、Adapter 事件未启动）；
  ③ `DatabaseWorker` 排空位于第 8.3 节钩子序列末尾（`close()` 之后、EXEC-01
  步骤 2/3 之前），run 有界超时等通道+语句间 StopToken、wakeup 解除阻塞；
  ④ 文件本体终态作业（Completed→SHA-256+原子改名+hash 回写；Failed/Cancelled→
  `.part` 幂等删除）全部在 blocking worker 内执行，数据根目录解析为 persistence
  层平台条件编译单元、公开面仅 `std::string`（RULE-10）；第 8.3 节钩子序列补
  「M2 起钩子末尾追加持久化作业排空」前向引用；纯文档变更，无产品代码。
  详见验证记录。）
- [x] `M2-02` 落地 vendored SQLite 接入：官方 amalgamation 并入
  `third_party/sqlite`（仅 `sqlite3.c` / `sqlite3.h`），锁文件登记
  `class=vendored` + 版本 + SHA-256，`cmake/Dependencies.cmake` 增加
  `file(SHA256)` 校验分支（不匹配即 `FATAL_ERROR` + 修复提示），编译宏
  `SQLITE_DQS=0` / `SQLITE_OMIT_LOAD_EXTENSION=1`，debug 构建断言
  `sqlite3_sourceid` 与锁文件版本一致。（2026-09-23：下载前先校验 zip SHA3-256
  与 DEC-004 记录及 sqlite.org 下载页标注一致，解包仅取两文件；锁文件新增
  vendored 条目（版本 + 双文件 SHA-256 + zip 来源）；`Dependencies.cmake` 按
  `class` 分支（pinned 行为不变，vendored 逐文件校验，未知 class 报错）；
  `third_party/sqlite/CMakeLists.txt` 静态库 target（不继承第一方告警级别）；
  asan/ubsan/tsan 预设补 `CMAKE_C_FLAGS`（sqlite3.c 为 C 编译单元，否则不被
  插桩）；`test_sqlite_sourceid` 三方一致断言（header 宏 / libversion /
  锁文件注入版本）。详见验证记录。）
- [x] `M2-03` 提供 persistence 薄 RAII 封装与迁移框架：`Database` / `Statement` /
  `Transaction` 守卫 + `SqliteError`；open 处统一设置 `journal_mode=WAL`、
  `synchronous=NORMAL`、`foreign_keys=ON`、`busy_timeout`；`persistence/migration`
  按 `PRAGMA user_version` 版本化迁移（前进成功与失败回滚路径）；`sqlite3*` 不出
  现在 persistence 层外公开头文件的编译级验证（`RULE-10`）。（2026-09-23：
  `persistence/database/database.hpp/.cpp`（`SqliteError`（主错误码 + errmsg 语义）、
  `Database`（open/close RAII + open 期 pragma：busy_timeout 默认 5000ms 为本项
  确定、严格 close 对在飞语句抛 `std::logic_error`、close 幂等）、`Statement`
  （1-based bind/step/列提取、析构确定性 finalize）、`Transaction`（异常路径
  析构自动 ROLLBACK））、`persistence/migration/migration.hpp/.cpp`（版本连续
  递增构造期校验、每步独立事务含 user_version 前进、失败回滚不前进、幂等
  no-op、库版本新于已知步骤干净失败）；`aki_persistence` 转实体静态库且
  sqlite3 改 PRIVATE 链接（RULE-10：`<sqlite3.h>` 仅在 database.cpp:4，消费者
  无 sqlite include 路径）；新增 `test_persistence_database`（11 test case /
  81 断言）与 `test_persistence_public_surface`（边界锁定消费编译单元）。
  同步封装无并发路径，DOD-02 六项不适用（DatabaseWorker 六项随 M2-05）。
  详见验证记录。）
- [x] `M2-04` 提供第 11 节 ER 模型 4 张表 schema（迁移 M1 起步版本）与仓储层：
  领域枚举（`TrustState` / `MessageType` / `DeliveryState` / `TransferState`）
  `INTEGER + CHECK` 映射，DEVICE / CONVERSATION / MESSAGE / TRANSFER 的 CRUD，
  prepared-statement 缓存容量上限（`RULE-09`）。（2026-09-23：
  `persistence/migration/schema_v1.hpp/.cpp`（v1 单步原子迁移：ER 四表 + 关系
  DEVICE 1-* CONVERSATION 1-* MESSAGE 0..1 TRANSFER + 四枚举 `INTEGER + CHECK`
  显式合法值集 + `transfer` 的 `stored_relative_path`/`stored_sha256`/
  `stored_size_bytes` 回写位与 `file_name` 仅展示列）；`persistence/repository/
  statement_cache.hpp/.cpp`（prepared-statement LRU 缓存，容量上限 16 可配，
  满时 LRU 逐出，`invalidate` 供错误语句逐出）；`persistence/repository/
  repositories.hpp/.cpp`（四仓储与 M1 领域类型双向转换，API 对齐设计第 11.1 节
  ①④：整行 upsert / 进度列更新 / 送达状态列更新 / 终态更新含回写位；未命中行
  `runtime_error` 可见）。新增 `test_persistence_repository`（11 test case /
  108 断言；初稿误记 10/103，以合入前 debug/release 双配置实测为准）。重要发现：SQLite 3.53.4 对“约束失败语句的 reset 复用”存在异常
  终止缺陷（官方 DLL/MSVC/GCC 三路复现）——`run_cached` 统一在 SqliteError 时
  逐出缓存语句规避，详见验证记录。本项为同步封装，DOD-02 六项不适用。
  详见验证记录。）
- [x] `M2-05` 提供 `DatabaseWorker`：经 `ExecutorOwner` 注册 blocking worker
  （首次启用，设计第 8.2 节落点），单一连接 + 有界工作通道串行消费、StopToken
  在语句间协作取消、通道满与执行失败经 Executor 监控设施可见（`EXEC-04`/
  `EXEC-06`/`RULE-09`），关闭顺序并入 `EXEC-01`（drain 在途 DB 作业后 join）。
  （2026-09-23：`persistence/database/database_worker.hpp/.cpp`（公开控制面
  `DatabaseWorkerControl`：enqueue（try_send 明确拒绝）/request_drain/
  request_exit（协作退出）/completed-failed-rejected 及通道 dropped/
  closed_send 计数，pimpl 公开头无 sqlite/executor 类型）+
  `database_worker_adapter.hpp`（注册侧接线头：`DatabaseWorkerRunnable`
  实现 `IBlockingIoWorker`，独占连接经 M2-04 仓储消费作业；等待采用
  try_receive + 短睡眠轮询环——pinned v0.5.0-7 的 receive_for 在本场景
  观测到已 admit 作业不可见/进程异常终止（官方 DLL 复现），采用集成指南
  备选 ⑧，响应延迟上界=轮询间隔）；宿主组合：unique_ptr 锚定 Repositories
  （仓储持 Database& 回引，不可移动）+ control 共享。DOD-02 六项沿 worker
  路径全覆盖（`test_database_worker` 8 test case / 102 断言，debug 60 次/
  release 30 次稳定性通过；初稿误记 7/87，以合入前双配置实测为准）。详见验证记录。）
- [x] `M2-06` 落实文件本体存储布局与生命周期：数据根目录（Platform Adapter 解析，
  Windows `%APPDATA%` / Linux XDG，Core 不见平台类型）下 `db/aki.db3` 与 `files/`
  并置；`files/tmp/<transfer_id>.part` 写入、`Completed` 终态流式 SHA-256 后原子
  改名到 `files/<transfer_id>/<净化文件名>`、`Failed` / `Cancelled` 幂等删除
  `.part`、启动清扫无活动 Transfer 行的残留；`TransferId` 字符集约束
  `[A-Za-z0-9_-]{1,64}` 固化于生成处，远端原始文件名只存 DB 不拼路径
  （`DEC-004`；本项以测试内字节源驱动，真实链路 M4）。（2026-09-23：
  `persistence/storage/` 落地——`data_root.hpp/.cpp`（平台条件编译单元：
  Windows %APPDATA% / POSIX XDG，公开面仅 std::string，getenv 的 MSVC C4996
  作用域内豁免）、`sha256.hpp/.cpp`（自包含流式 SHA-256，FIPS 180-4 已知
  向量验证）、`file_store.hpp/.cpp`（FileStore：布局创建、.part 覆盖/追加
  分块写入、Completed 作业组（幂等跳过/恢复收敛/.part 缺失明确失败）、
  discard 幂等删除、启动清扫、TransferId 校验与磁盘名净化）、
  `file_jobs.hpp/.cpp`（DbJob 工厂，shared_ptr<FileStore> 捕获生命周期安全）；
  单测 `test_file_store`（8 test case / 68 断言）经 DatabaseWorker 路径覆盖
  验收 ①②③。详见验证记录。）
- [x] `M2-07` 实现重启恢复并写入宿主/测试路径：设备、信任状态、会话、消息与
  传输历史写入 → 受控关闭 → 重新 open 后逐域断言一致（`SCOPE-09` 验收）；
  DB 文件损坏时 open 干净失败不静默。（2026-09-23：`persistence/recovery/
  startup_recovery.hpp/.cpp`（主线程同步组合：数据根注入 → open（损坏即
  SqliteError 干净失败）→ user_version 迁移 → 四仓储 `load_all()` 逐域加载 →
  `sweep_tmp_orphans` 按加载活动行清扫 → 产出播种数据；单一连接整体移交
  DatabaseWorker）+ `persistence/repository/update_jobs.hpp/.cpp`（§11.1 ①
  typed 更新的域级 DbJob 工厂，SetPresence/SetConnectionPath 无作业）；
  根 `main.cpp` 组合根改造（initialize 后同步恢复 → AppStateOwner 构造入参
  播种 → Manager 构造 → 注册 DatabaseWorker → RouterSink（EXEC-02 启动段
  纪律）；宿主 `PersistenceMirror` 在 owner 单写者上下文按接受顺序镜像
  typed 更新入队；关闭钩子末尾 `close()` 之后排空 DB worker；进程内 session B
  重开逐域断言）；`tests/integration/test_restart_recovery.cpp`（integration
  标签，4 用例 / 158 断言）+ 损坏 DB 宿主级 ctest（`--exact` 模式直用损坏根，
  FATAL 输出 + 非零退出）。同步设计 §14 目录树（补记 M2-06 `storage/` 并新增
  `recovery/`）。详见验证记录。）
- [x] `M2-08` 收口审计与退出证据归集（沿用 M1-08 纪律）：实现与设计第 11 节/
  `DEC-004` 逐项校对，退出-1~5 证据与可复现命令归档，供应链登记
  （zip SHA3-256 + 文件 SHA-256、public domain 许可证结论入
  `docs/supply-chain/`）。（2026-09-23：设计-实现审计矩阵逐项一致——§11 ER
  四表与列补齐、§11.1 ①~④、§14 目录树、§8.2/8.3/10.1 关联节对
  `persistence/` 五子目录与根 `main.cpp` 宿主组合全部核对通过；两项已知偏差
  （M2-05 轮询环替代 `receive_for`、M2-07 快照权威值镜像替代接受后回调）锚点
  核实于本文件验证记录，未记录偏差数为 0；RULE-07/RULE-10 边界 grep 通过
  （sqlite3* 封死 persistence 唯一编译单元、公开头仅注释命中、executor 类型
  仅接线层、第一方无自建线程）；退出-2~5 证据归档（本地 MSVC debug/release
  全量 ctest 19/19 复跑 + gh 逐 PR 核实 #13~#18 CI 全绿含 asan/ubsan、#16~#18
  含 tsan 覆盖 DOD-03）；`docs/supply-chain/` 创建并登记 SQLite vendored 审计
  （哈希本地复算一致、public domain 结论、上游缺陷处置：3.53.4 仍为最新无
  可升级修复、建议由负责人向 sqlite.org 报告并登记触发条件）；DEC-004 验证
  方式逐项回填。M2 关闭（Completed）。详见验证记录。）

## 风险与阻塞

- `sqlite3.c` 纳入 asan/ubsan preset 编译；若 UBSan 对 SQLite 内部报点，仅在
  sqlite target 上做编译旗标豁免并记录，不放松全局（`DEC-004` 影响节）。
- 本地 MinGW 默认生成器仍受 pinned executor 构建缺陷阻塞（M1 验证记录限制 1），
  本地验证以 MSVC 生成器等价执行；`Dependencies.cmake` 的 vendored 校验分支属
  configure 逻辑，需在 MinGW/MSVC/CI Linux 三套工具链的 configure 均验证。
- WAL 约束：db 与 files 必须位于本地盘；网络文件系统场景在测试中显式记录限制
  （`DEC-004`）。

## 测试与退出条件

- [x] 退出-1：重启恢复——写入 → 关闭 → 重开，设备/信任/会话/消息/传输历史逐域
  一致；损坏 DB open 干净失败。（`M2-07`，2026-09-23：宿主 `smoke.device_
  lifecycle` 与 `test_restart_recovery` 双证据，见验证记录）
- [x] 退出-2：并发基线六项沿 `DatabaseWorker` 路径通过——正常完成、任务异常、
  提交拒绝（通道满/关闭后）、执行中取消（语句间 StopToken）、超时、shutdown
  （drain 后 join）。（`M2-05` `test_database_worker` 8 test case 覆盖映射见
  其验证记录；M2-08 复跑 `ctest --preset debug -C Debug -R "test_database_worker"
  --timeout 120` → Passed；宿主组合复验见 `test_restart_recovery` 排空零丢失
  用例与宿主 smoke 断言）
- [x] 退出-3：迁移与约束测试通过——`user_version` 前进成功、失败路径不破坏既有
  数据；枚举 `CHECK` 拒绝非法值；重复迁移幂等。（`test_persistence_database`
  前进/回滚/幂等/乱序构造拒绝 + `test_persistence_repository` 六处 CHECK 拒绝
  非法枚举（原始 SQL 注入验证）；M2-08 复跑 `ctest --preset debug -C Debug -R
  "test_database_worker|test_persistence_database|test_persistence_repository"
  --timeout 120` → 3/3 Passed；重开迁移幂等另证于 `test_restart_recovery`）
- [x] 退出-4：debug/release 构建与全量测试通过；ASAN/UBSAN 通过（随 CI 门禁），
  不适用工具链记录限制与补跑条件；DB worker 与 Manager 跨上下文交互按 `DOD-03`
  评估 TSAN/故障注入。（M2-08 复跑 `ctest --preset debug -C Debug --timeout
  120` → 19/19、`ctest --preset release -C Release --timeout 120` → 19/19；
  gh 逐 PR 核实 #13~#18 CI 全绿——Linux debug/asan/ubsan + Windows MSVC 全部
  SUCCESS，#16~#18 另含 Linux tsan（DB worker 跨上下文 DOD-03 证据），run
  链接见 M2-08 验证记录；MinGW 完整构建限制沿用 M1-02 记录 1）
- [x] 退出-5：设计（第 11 节集成契约）、决策（`DEC-004` 验证方式回填）、总计划
  状态同步；验证记录含可复现命令与结果；供应链文档已登记。（`DEC-004` 验证
  方式逐项回填含测试与 PR 映射表；`docs/supply-chain/`（README + sqlite
  3.53.4 审计与上游缺陷处置）登记；总计划当前状态与里程碑索引 M2 → Done
  同步；M2 文档状态 → Completed）

## 验证记录

- 2026-09-23（`M2-01`，设计先行，纯文档变更）：
  - 范围：[设计第 11.1 节](../design/aki_design.md)（新增「持久化集成契约（M2
    契约，M2-01）」，四类契约：DB 写路径映射 / 启动恢复流程 / `DatabaseWorker`
    在 `EXEC-01` 中的落点 / 文件本体生命周期触发点与数据根目录 Platform Adapter
    最小落点）；[设计第 8.3 节](../design/aki_design.md)钩子序列补「M2 起钩子
    末尾追加持久化作业排空」前向引用（避免与 11.1 ③ 的顺序定义矛盾）；
    [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md) 关联文档节的过时
    括注修正（链接改指本文档并保留原标注历史）。
  - 依据：本里程碑工作项 `M2-01`；[DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)
    （引擎/worker/文件布局/迁移已冻结，本项细化集成契约）、
    [DEC-002](../decisions/DEC-002-layering-and-state-boundary.md)（状态边界与
    单写者）；设计第 8.2 节（`wait_for_completion_ex` 不覆盖 blocking worker、
    M2 起启用）、第 8.3 节（宿主钩子顺序）、第 10.1 节（typed 更新清单、单写者
    纪律、`AppStateOwner` 构造入参初始快照）、第 11/14 节；总计划 `RULE-02`/
    `RULE-09`/`RULE-10`、`EXEC-01`/`EXEC-04`/`EXEC-06`、`DOD-04`；工程规范 6.2/8。
    executor-integration 集成指南 blocking-io 卡（本项会话按 SKILL 路由加载：
    `run(StopToken)` + `wakeup()` 可解除阻塞契约、`request_stop` 不 join、
    `stop()` join、shutdown 后不得保留 worker 引用）。
  - 契约与既有设计/决策的一致性自查（验收 ①）：写路径以 typed 更新为权威、事件
    为通知面（§10.1 单写者与事件语义）；`SetPresence`/`SetConnectionPath` 不持久
    化与第 11 节 ER 模型（DEVICE 无 presence 列）及 LatestMailbox 易失摘要语义
    一致；恢复以 `AppStateOwner` 构造入参播种初始快照（该入参自 M1-02 即存在）
    且恢复期无并发源，符合 RULE-02/DEC-002 单写者；排空位于钩子末尾由 §8.3 前向
    引用衔接，与 §8.2 步骤 4 不覆盖 blocking worker 的既证结论一致；文件终态作业
    在 blocking worker 内与 DEC-004「进入终态 Completed 时在 blocking worker 内
    流式计算 SHA-256」一致；数据根目录最小落点（persistence 层平台条件编译单元、
    公开面仅 `std::string`）未超出 DEC-004「Platform Adapter 解析，Core 不见平台
    类型」原则，不新建议题。无冲突，无需补充决策。
  - 验证：纯文档变更（验收 ②），无构建/测试行为改动——`git status` 确认改动仅
    涉及设计/计划/决策四份文档（设计、DEC-004、总计划、本里程碑文档），无产品
    代码；相对链接核对：`docs/plans/m2-local-persistence.md` 与
    `docs/decisions/DEC-004-local-persistence-sqlite.md` 均存在，DEC-004 修正
    后链接指向有效目标；第 11.1 节行文采用与既有小节一致的相对节引用（第 8.2/
    8.3/10.1 节），无新增外链。
  - 限制：本项为设计先行契约，`DatabaseWorker`/仓储/恢复的实现在 M2-03~07 落地；
    实现若与本契约出现偏差，按 M1-08 纪律先更新第 11.1 节再合代码。
  - 同步：设计第 8.3/11.1 节、DEC-004 关联文档节、本里程碑工作项与状态、
    总计划当前状态与里程碑索引（M2 → In Progress）。

- 2026-09-23（`M2-02`，Windows 11 / MSVC 2022 BuildTools 14.44.35207 /
  CMake 4.1.0 / w64devkit GCC 15.2.0（MinGW configure-only 与 GCC 语法检查））：
  - 范围：`third_party/sqlite/sqlite3.c` + `sqlite3.h`（官方 amalgamation vendored，
    仅两文件）、`third_party/sqlite/CMakeLists.txt`（`sqlite3` 静态库 target：
    `SQLITE_DQS=0`、`SQLITE_OMIT_LOAD_EXTENSION=1`，不调用 `aki_apply_warnings`——
    第三方 target 不继承第一方告警级别，DEC-007 纪律）、
    `third_party/dependencies.lock.json`（新增 vendored 条目：version 3.53.4、
    license public domain、`class=vendored`、`submodule=false`、双文件 SHA-256、
    `source_artifact`（zip URL + SHA3-256）溯源）、`cmake/Dependencies.cmake`
    （按 `class` 分支：pinned 路径行为与 STATUS 输出不变；vendored 逐文件
    `file(SHA256)` 校验，缺失/漂移即 `FATAL_ERROR` + 修复提示；未知 class 报错）、
    `persistence/CMakeLists.txt`（`aki_persistence` 链接 `sqlite3`，M2-03 起薄
    封装消费）、`CMakePresets.json`（asan/ubsan/tsan 预设补 `CMAKE_C_FLAGS`——
    sqlite3.c 是 C 编译单元，不加则不进 sanitizer 插桩）、
    `tests/unit/test_sqlite_sourceid.cpp` + `tests/CMakeLists.txt`（sourceid
    断言 Catch2 用例，`DEC-007`；期望版本由 CMake 从锁文件注入
    `AKI_EXPECTED_SQLITE_VERSION`）。
  - 依据：[DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)（版本
    3.53.4、zip SHA3-256、仅两文件、锁文件格式、`file(SHA256)` 校验、编译宏、
    sourceid 断言、asan 纳入与豁免策略）、[DEC-003](../decisions/DEC-003-dependency-locking.md)
    （锁定 + configure 校验、漂移即失败纪律）、[DEC-007](../decisions/DEC-007-test-framework.md)
    （第三方不继承告警级别）、工程规范 10.7；总计划 `RULE-09`/`RULE-10`、
    `DOD-03`/`DOD-05`。本项无新增并发路径（DOD-02 六项不适用；`DatabaseWorker`
    六项随 M2-05）。
  - 获取与校验（先校验后解包）：sqlite.org/download.html 页面标注
    `sqlite-amalgamation-3530400.zip`（version 3.53.4，SHA3-256
    `628a44cf…934e`）与 DEC-004 记录一致 → `curl` 下载
    `https://sqlite.org/2026/sqlite-amalgamation-3530400.zip` →
    `python hashlib.sha3_256` 校验 MATCH → 解包仅取 `sqlite3.c`（9,515,341 B）与
    `sqlite3.h`（690,838 B）入 `third_party/sqlite/`（zip 内 shell.c/sqlite3ext.h
    未纳入）；双文件 SHA-256（`b1dd5d74…8189` / `919e7f2e…0e1d`）登记锁文件。
  - 验证（生成器说明同 M1 记录：本地 MinGW 默认生成器受限制 1 阻塞，构建以
    MSVC 生成器执行；MinGW 仅 configure 校验）：
    - MSVC debug configure：4 条 Dependency STATUS——executor/EUI-NEO/heyaki
      pinned 三条与 M1 时期完全一致（验收 ① 前半），新增
      `Dependency 'sqlite' vendored at third_party/sqlite (version 3.53.4,
      2 files verified)`。
    - `cmake --build --preset debug --config Debug && ctest --preset debug -C Debug`
      → 12/12（原 11 + 新 `test_sqlite_sourceid`）；release 同构 → 12/12
      （验收 ②）。
    - sourceid 断言实测输出：`expected (lock): 3.53.4`、`SQLITE_VERSION: 3.53.4`、
      `libversion: 3.53.4`、`sqlite3_sourceid: 2026-07-24 19:02:57 bf7c7f30…`，
      三方一致且 header/lib 配对（验收 ⑤；release 构建同样运行该断言，严格于
      "仅 debug" 的建议项）。
    - 负向验证（对齐 DEC-003 验证方式，验收 ① 后半）：篡改锁文件中
      `sqlite3.c` 的 SHA-256 → `cmake --preset debug` 以 FATAL_ERROR 失败，消息
      含 actual/required 两个哈希、官方 artifact 来源 URL 与 DEC-004 指引；还原
      后 configure 通过（`2 files verified`），锁文件 diff 恢复为仅 vendored
      条目（+18 行）。
    - MinGW configure-only（scratch 目录 `build/mingw-cfg-check`，-G "MinGW
      Makefiles"）：4 条 Dependency 校验通过、`Configuring done`（构建仍受
      `M1-02` 记录限制 1 阻塞，未变化）。
    - GCC 语法检查（CI Linux 告警姿势；Catch2 头来自 FetchContent 构建树，
      src 与 generated-includes 两个包含目录）：
      `g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -fsyntax-only -I.
      -Ithird_party/sqlite -Ibuild/debug/_deps/catch2-src/src
      -Ibuild/debug/_deps/catch2-build/generated-includes
      -DAKI_EXPECTED_SQLITE_VERSION=\"3.53.4\"
      tests/unit/test_sqlite_sourceid.cpp` 通过（Catch2 化后重跑）。
  - 过程修正：configure 联调中修正两处 CMake JSON 迭代问题——`files` 对象的
    键名迭代须用 `string(JSON ... MEMBER <json> <ptr> <index>)`（本机 CMake 4.1
    无 `MEMBERS` 模式，对象不支持数字索引 GET），最终实现按 `LENGTH`+`MEMBER`
    遍历。
  - 限制：本机无 sanitizer 运行时——`sqlite3.c` 随 asan/ubsan 的编译与运行由本
    PR 的 Linux CI 门禁提供（预设已补 `CMAKE_C_FLAGS`，CI 复跑核验）；UBSan 若
    对 SQLite 内部报点，仅在 sqlite3 target 上做旗标豁免并记录（里程碑风险节
    预留，本项未预置豁免、不放松全局）；供应链审计结论（zip SHA3-256 + 文件
    SHA-256 + public domain 结论入 `docs/supply-chain/`）按计划留 M2-08 归档，
    溯源字段已先行登记于锁文件。MR 闭环由后续环节执行，本记录不含 commit/CI
    证据。
  - 同步：本里程碑工作项 `M2-02`、总计划当前状态。

- 2026-09-23（`M2-03`，Windows 11 / MSVC 2022 BuildTools 14.44.35207 /
  CMake 4.1.0 / w64devkit GCC 15.2.0（语法检查））：
  - 范围：`persistence/database/database.hpp/.cpp`、
    `persistence/migration/migration.hpp/.cpp`、`persistence/CMakeLists.txt`
    （INTERFACE 骨架转实体静态库，sqlite3 改 PRIVATE 链接）、
    `tests/unit/test_persistence_database.cpp`、
    `tests/unit/test_persistence_public_surface.cpp`、`tests/CMakeLists.txt`
    （两个新 target；`test_sqlite_sourceid` 改为直连 `sqlite3` target——
    aki_persistence 不再向消费者传播 sqlite include/库，sourceid 用例测的是
    vendored sqlite3 本体）。
  - 依据：[DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)（RAII 四件套、
    open pragma 清单、user_version 迁移、sqlite3* 封死 persistence、损坏 DB 干净
    失败）；[设计第 11.1 节 ②](../design/aki_design.md)（启动段 open→迁移→加载
    顺序、干净失败）、第 11/14 节；总计划 `RULE-07`/`RULE-09`/`RULE-10`、
    `DOD-03`/`DOD-05`；[DEC-001](../decisions/DEC-001-cpp-baseline.md)（预设矩阵
    与标签）；executor-integration 集成指南卡片本会话已加载（本项无新增并发
    路径，DOD-02 六项不适用——同步封装不建线程、不引 executor 依赖，运行期
    承载归 M2-05 `DatabaseWorker`）。
  - 关键落点：busy_timeout 默认 5000ms（DEC-004 只定清单，默认值本项确定，覆盖
    "用户以 sqlite3 CLI 进程外同时打开" 的预期竞争场景，可经 `DatabaseOptions`
    覆盖）；严格 close（在飞语句 → `std::logic_error`，生命周期契约显式暴露）
    + 析构兜底 `sqlite3_close_v2`（不产生 UB）；`Database::execute` 走
    sqlite3_exec 支持多语句脚本（迁移 DDL 需要），`Statement` 走 prepare_v2。
  - 验证（生成器说明同 M1 记录；本机 MinGW 限制 1 未变化）：
    - `cmake --preset debug -G "Visual Studio 17 2022" -A x64 && cmake --build
      --preset debug --config Debug && ctest --preset debug -C Debug` → 14/14
      （原 12 + 新 `test_persistence_database` 11 test case / 81 断言 +
      `test_persistence_public_surface`）。
    - `cmake --build --preset release --config Release && ctest --preset release
      -C Release` → 14/14。
    - 稳定性：`test_persistence_database` debug 连续 30 次运行全部通过。
    - GCC 语法检查（CI Linux 告警姿势）：
      `g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -fsyntax-only -I.
      -Ithird_party/sqlite persistence/database/database.cpp
      persistence/migration/migration.cpp` 与对
      `tests/unit/test_persistence_database.cpp`（含 Catch2 include）通过
      ——过程中按 GCC 限制将 `Database::Options` 上提为命名空间作用域
      `DatabaseOptions`（类内保留 `using Options` 别名，同仓库既有处理）。
    - RULE-10 证据（验收 ③）：`grep -rn "#include <sqlite3.h>" persistence/`
      仅命中 `persistence/database/database.cpp:4`（唯一编译单元）；
      `test_persistence_public_surface` 只链接 `aki_persistence`（sqlite3
      PRIVATE，无 sqlite include 路径传播）即完成公开头编译 + :memory: 库
      迁移/写读真实使用——任一公开头引入 `<sqlite3.h>` 该目标即编译失败
      （编译级边界锁定，沿 M1-03 grep 模式另加公开头注释级复查）。
    - 覆盖映射（验收 ①②）：迁移前进成功（2 步应用、user_version=2、种子行
      同事务提交）、失败回滚（v2 非法 SQL → user_version 保持 1、t1 在/t2 不在）、
      幂等重跑（返回 0）、乱序/缺步/重复/零版本/空名/空 SQL 构造期
      `std::invalid_argument`、部分前进（v1 库 + {1,2} 迁移只应用 v2）、库新于
      已知步骤 → `std::runtime_error` 干净失败；open 损坏 DB（600 字节垃圾文件）
      → `SqliteError` 干净失败（code≠0 + "not a database" 语义可见）；pragma
      回读（journal_mode=wal / synchronous=1 / foreign_keys=1 /
      busy_timeout=自定义 1234）；prepare 期语法/缺表错误、step 期主键冲突
      （UNIQUE 语义可见）；事务提交/显式回滚/异常自动回滚；blob/text/null
      往返；移动语义与空壳拒绝；close 在飞语句暴露 + 幂等。
  - 过程修正：测试联调中修正三处用例自身缺陷——① pragma 用例 close 前未结束
    全部语句作用域（触发严格 close 的预期契约，改为内层作用域）；② "缺表"
    错误实际发生在 prepare 期（prepare_v2 即时解析），step 期错误改用主键冲突
    承载；③ close 幂等语义与实现不一致（实现补齐：已关闭时 no-op，与头文件
    契约对齐）。
  - 限制：ASAN/UBSAN 随本 PR 的 Linux CI 门禁提供（sqlite3.c 已在 asan preset
    C 编译面，M2-02 已补 `CMAKE_C_FLAGS`）；MinGW 完整构建沿用 M1-02 记录
    限制 1；MR 闭环由后续环节执行，本记录不含 commit/CI 证据。
  - 同步：本里程碑工作项 `M2-03`、总计划当前状态。

- 2026-09-23（`M2-04`，Windows 11 / MSVC 2022 BuildTools 14.44.35207 /
  CMake 4.1.0 / w64devkit GCC 15.2.0（语法检查））：
  - 范围：`persistence/migration/schema_v1.hpp/.cpp`（v1 迁移注册）、
    `persistence/repository/statement_cache.hpp/.cpp`（LRU 缓存 +
    `invalidate`）、`persistence/repository/repositories.hpp/.cpp`（四仓储）、
    `persistence/CMakeLists.txt`（源文件接入）、
    `tests/unit/test_persistence_repository.cpp` + `tests/CMakeLists.txt`、
    `tests/unit/test_persistence_public_surface.cpp`（扩展覆盖仓储公开面）。
  - 依据：[设计第 11 节](../design/aki_design.md)（ER 模型）、
    [设计第 11.1 节 ①④](../design/aki_design.md)（写路径映射、终态回写位）、
    [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)（schema 对应 ER、
    枚举 INTEGER+CHECK、DB 存相对路径/hash/size/MIME、原始文件名仅存 DB）、
    M2-03 迁移框架与 RAII 封装；总计划 `RULE-09`/`RULE-10`、`DOD-05`。本项为
    同步封装，无新增并发路径（DOD-02 六项随 M2-05）。
  - 重要发现（上游缺陷，已规避并记录）：SQLite 3.53.4 中，语句 step 因外键
    约束失败（SQLITE_CONSTRAINT）后对其 `sqlite3_reset` 会异常终止——
    `:memory:` 库、多外键子表（conversation 含 2 外键）下经官方预编译 DLL、
    MSVC 本地编译、w64devkit GCC 本地编译三路复现（纯 C API 复现脚本
    `build/scratch/repro_raw2.cpp`；单外键无 CHECK 的表不复现）。规避：
    仓储全部语句经 `run_cached` 执行，SqliteError 时 `StatementCache::
    invalidate` 逐出该语句（确定性 finalize），调用方重试总是 fresh prepare
    （fresh 语句行为正确）。影响面：仅“约束失败后复用同一 prepared 语句”
    路径；正常恢复/写入路径（父行先于子行）不受影响。建议后续向上游
    sqlite.org 报告或评估版本升级（超出本项范围，已留档）。
  - 验证（生成器说明同 M1 记录）：
    - `ctest --preset debug -C Debug`（构建后）→ 15/15（原 13 + 新
      `test_persistence_repository` 11 test case / 108 断言 +
      `test_persistence_public_surface` 扩展仓储公开面）。
    - `ctest --preset release -C Release` → 15/15。
    - 稳定性：`test_persistence_database` debug 连续 30 次全过。
    - GCC 语法检查（CI Linux 告警姿势）：
      `g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -fsyntax-only -I.
      -Ithird_party/sqlite` 对五个实现/迁移/仓储编译单元与三个测试编译单元
      全部通过。
    - RULE-10 证据（验收 ④）：`grep -rn "#include <sqlite3.h>" persistence/`
      仅命中 `database.cpp:4`；公开头 grep `sqlite3_` 仅注释行命中；
      `test_persistence_public_surface`（不链接 sqlite3、无 include 路径传播）
      编译并运行通过，覆盖数据库 + 迁移 + 仓储公开面真实使用。
    - 覆盖映射（验收 ①②③）：v1 迁移空库应用（四表存在、user_version=1）、
      重复应用幂等（返回 0）；device 往返（capabilities 位、5 个 trust 值
      逐一映射、presence 不持久化恢复为 Offline、upsert 更新不新增行）；
      conversation 外键（孤儿行被 foreign_keys=ON 拒绝 → SqliteError、
      就位后成功、Disconnected 往返）；message 五类 payload 往返 + 时间戳
      无损 + 未知会话 FK 拒绝；transfer 进度列更新不新建行、终态 + 回写位
      列（stored_relative_path/stored_sha256/stored_size_bytes）、未知 id
      与非终态 complete 拒绝；schema CHECK 拒绝非法枚举（trust/class/
      conversation/message/delivery/transfer 六处原始 SQL 注入验证）；
      语句缓存容量 2 时 LRU 逐出 + 逐出后复用正确 + 零容量构造拒绝。
  - 过程修正：测试联调发现并修正三处用例/实现问题——① `Statement::step()`
    对 UPDATE 永远返回 DONE，“未命中即抛”只能以 `changes()==0` 判定
    （set_delivery_state/update_progress/complete 三处修正）；② 缓存测试的
    DDL/INSERT 语句未 step 即断言表存在（补 step）；③ 公开面用例的迁移仅建
    probe 表导致 DeviceRepository 抛“no such table”（未捕获 → WER 挂起），
    改为 v1 schema + 两步迁移。
  - 限制：ASAN/UBSAN 随本 PR Linux CI 门禁提供（sqlite3.c 已在 asan preset
    C 编译面）；MinGW 完整构建沿用 M1-02 记录限制 1；MR 闭环由后续环节
    执行，本记录不含 commit/CI 证据。
  - 同步：本里程碑工作项 `M2-04`、总计划当前状态。

- 2026-09-23（`M2-05`，Windows 11 / MSVC 2022 BuildTools 14.44.35207 /
  CMake 4.1.0 / w64devkit GCC 15.2.0（语法检查））：
  - 范围：`persistence/database/database_worker.hpp/.cpp`（公开控制面
    `DatabaseWorkerControl`）、`persistence/database/database_worker_adapter.hpp`
    （注册侧接线头：`DatabaseWorkerRunnable` : `executor::IBlockingIoWorker`；
    executor 类型按设计第 8.2 节允许存在于该接线层，RULE-10 守卫公开面不含）、
    `persistence/CMakeLists.txt`（PRIVATE 链接 executor::executor）、
    `tests/unit/test_database_worker.cpp` + `tests/CMakeLists.txt`、
    `persistence/repository/statement_cache.hpp`（补移动语义——Repositories
    组装需要）。`app/lifecycle` 未改（start_blocking_worker 既有入口直接消费）。
  - 依据：调研结论 `M2-05-blocking-worker-semantics`（BlockingWorkerSpec 无
    队列面，作业通道应用层自建 `executor::comm::MpscChannel<DbJob>`；wakeup
    平凡 noexcept；关闭排空走宿主钩子；无能力缺口不进 9.4 台账）、
    [设计第 11.1 节 ①③](../design/aki_design.md)、第 8.2/8.3 节、
    [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)、总计划
    `EXEC-01`/`EXEC-04`/`EXEC-06`/`EXEC-07`、`RULE-07`/`RULE-09`/`RULE-10`、
    `DOD-02`/`DOD-03`；AGENTS.md Executor 规则 5/6/8/10；executor-integration
    blocking-io 卡（本会话已加载）。
  - 实现要点：① 入队 `enqueue(DbJob)->bool`：未注册/作业不完整（缺执行体或
    完成通道）/通道满/已关闭四类明确拒绝（rejected 计数 + 通道 CommStats
    Dropped/ClosedSend）；② run() 轮询消费：try_receive 全量排空 → 作业间
    检查 StopToken 与 drain/exit 旗标 → 短睡眠（wait_timeout，默认 100ms）
    ——响应延迟上界=轮询间隔；③ 逐作业 try/catch：SqliteError/runtime_error
    经 promise set_exception 结算 + failed 计数，worker 存活（未捕获异常会以
    WorkerException 终止 worker——硬纪律，测试显式断言存活）；④ request_drain
    后排空优先于 StopToken（先于 EXEC-01 步骤 2），预算（drain_budget，
    默认 2s，锚定 run 启动）内消费至空则 close+drain_completed；预算耗尽
    如实记录 drain_budget_exhausted 不伪造完成；⑤ request_exit：协作退出
    （不排空），作业间退出，存量按取消处理——DOD-02 执行中取消的确定性
    验证通道；⑥ Repositories 不可移动（四仓储持 Database& 回引，重定位即
    悬垂），DatabaseWorkerRunnable 以 unique_ptr 锚定 + control 别名
    shared_ptr 共享生命周期。
  - 实现与调研/设计的偏差（如实记录）：调研推荐 run() 以 `receive_for` 有界
    等待等待通道；联调中该路径观测到已 admit 作业不可见/进程异常终止（见
    下游缺陷条目），改用集成指南备选 ⑧（try_receive + 短睡眠轮询环，响应
    上界=轮询间隔）。设计第 11.1 节 ③ 的契约（有界等待、语句间取消、可解除
    阻塞）不受影响，无需改设计；wakeup() 平凡 noexcept 实现与调研结论一致。
  - **上游 SQLite 缺陷（M2-04 已记录，本项再次确认影响）**：SQLite 3.53.4
    对“约束失败语句的 reset 复用”异常终止（官方 DLL/MSVC/GCC 三路复现）。
    本项规避：仓储 `run_cached` 在 SqliteError 时逐出缓存语句；DatabaseWorker
    作业异常不重放、经 promise 结算。建议向上游报告或评估版本升级（M2-08
    审计归档）。
  - 验证（生成器说明同 M1 记录；本机 MinGW 限制 1 未变化）：
    - `ctest --preset debug -C Debug --timeout 60` → 16/16（原 15 + 新
      `test_database_worker` 8 test case / 102 断言）。
    - `ctest --preset release -C Release --timeout 60` → 16/16。
    - 稳定性：`test_database_worker` debug 连续 60 次、release 连续 30 次
      全部通过。
    - GCC 语法检查（CI Linux 告警姿势）：`g++ -std=c++20 -Wall -Wextra
      -Wpedantic -Werror -fsyntax-only`（含 executor/sqlite include）对
      database_worker.cpp 及 test_database_worker.cpp 通过。
    - RULE-10 证据（验收 ③）：`grep -rn "executor/|#include <sqlite3.h>"
      persistence/database/database_worker.hpp persistence/repository/*.hpp
      persistence/migration/*.hpp` 无命中；公开面守卫
      `test_persistence_public_surface` 继续通过。
    - 覆盖映射（验收 ①，DOD-02 六项沿 worker 路径 = M2 退出-2）：
      正常完成（3 作业入队 → 串行执行 → promise 结算 → 文件库重启断言
      device/conversation 生效）；任务异常（仓储 SqliteError 经 promise
      结算 + failed 计数 + 后续作业继续——worker 存活显式断言）；提交拒绝
      （未注册 / 作业不完整 / 通道满（门闩作业占住 run 循环 + 容量 2）/
      排空关闭后，四类均 `enqueue` 明确 false + rejected 计数）；执行中取消
      （`request_exit` 作业间退出：在飞门闩作业放行后完成不打断、排队作业
      不执行）；超时（drain_budget=30ms < 3×80ms 作业 → 预算耗尽
      `drain_budget_exhausted` 如实记录、不伪造完成）；shutdown（钩子
      request_drain → drain_completed（延迟上界 < budget+1s 实测）→
      EXEC-01 步骤 2/3 → `fully_stopped()`，已 admit 作业零丢失）。
      验收 ②：同实体作业 FIFO 保序（作业 2 的会话外键依赖作业 1 的设备行，
      乱序即 FK 失败 + order 向量断言）。
  - 过程修正：联调修正四处用例/实现问题——① UPDATE 的 `step()` 恒返回
    DONE，“未命中即抛”改以 `changes()==0` 判定（三处）；② 缓存测试 DDL/
    INSERT 未 step 即断言表存在（补 step）；③ 公开面用例迁移需含 v1 schema
    （DeviceRepository 依赖）；④ 在飞判定改用作业内 entered 原子（gate
    future 未就绪不能证明在飞）。
  - 限制：ASAN/UBSAN/TSAN 随本 PR Linux CI 门禁；TSAN：DB worker 属跨上下文
    交互（DOD-03）——tsan 预设已存在，本机 MinGW 不可跑（限制 2），补跑
    条件「CI 矩阵增加 tsan job」已于本 MR 落实（`.github/workflows/ci.yml`
    matrix 增补 `tsan`，运行证据随 MR CI 产出）；MR 闭环由后续环节执行，
    本记录不含 commit/CI 证据。
  - 同步：本里程碑工作项 `M2-05`、总计划当前状态。

- 2026-09-23（`M2-06`，Windows 11 / MSVC 2022 BuildTools 14.44.35207 /
  CMake 4.1.0 / w64devkit GCC 15.2.0（语法检查））：
  - 范围：`persistence/storage/data_root.hpp/.cpp`（数据根解析：Windows
    %APPDATA%ki / POSIX $XDG_DATA_HOME 或 $HOME/.local/shareki，公开面仅
    std::string；getenv 的 MSVC C4996 以作用域内 pragma 豁免，不放松全局）、
    `persistence/storage/sha256.hpp/.cpp`（自包含流式 SHA-256）、
    `persistence/storage/file_store.hpp/.cpp`（FileStore：布局创建 files/+
    files/tmp、.part 分块写入（覆盖/追加）、`valid_transfer_id`
    `[A-Za-z0-9_-]{1,64}`、`sanitize_disk_name`（末段分量+非法字节换 '_'+
    空名回退 file+128 截断）、Completed 作业组 `complete_transfer`（幂等
    跳过/恢复收敛/.part 缺失明确失败）、`discard_part`（幂等删除）、
    `sweep_tmp_orphans`（活动保留/终态与无行与非法名清理，返回删除数））、
    `persistence/storage/file_jobs.hpp/.cpp`（DbJob 工厂：complete/discard，
    shared_ptr<FileStore> 捕获生命周期安全）、`tests/unit/test_file_store.cpp`
    + `tests/CMakeLists.txt`（含数据根解析公开面锁定用例「Data root
    resolution exposes a std::string path」；此前误记为
    `test_persistence_public_surface` 扩展——该文件本变更加动，特此更正）。
  - 依据：[设计第 11.1 节 ④/②](../design/aki_design.md)、第 11/7 节（RULE-05）、
    [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)（目录布局、.part
    生命周期、SHA-256 流式+原子改名、TransferId 字符集、原始文件名仅存 DB）、
    M2-05 DatabaseWorker（作业承载）；总计划 `RULE-05`/`RULE-09`/`RULE-10`、
    `EXEC-04`/`EXEC-06`、`DOD-02`/`DOD-03`/`DOD-05`。
  - 验证（生成器说明同 M1 记录）：
    - `ctest --preset debug -C Debug --timeout 60` → 17/17（原 16 + 新
      `test_file_store` 8 test case / 68 断言，其中 3 个用例经
      DatabaseWorker 路径）。
    - `ctest --preset release -C Release --timeout 60` → 17/17。
    - 稳定性：`test_file_store` debug 连续 40 次全过；worker 路径 30 次全过。
    - GCC 语法检查（CI Linux 告警姿势）：`g++ -std=c++20 -Wall -Wextra
      -Wpedantic -Werror -fsyntax-only` 对五个 storage 编译单元与
      `test_file_store.cpp` 全部通过（修正一处 adapter 成员初始化顺序
      -Werror=reorder）。
    - 覆盖映射（验收 ①②③）：Completed 作业组（.part 写入 → 经 worker 的
      complete job → 改名 + 回写位 SQL 断言（stored_relative_path=
      files/t-1/_____________.bin（净化的中文名）、stored_sha256=内容摘要、
      stored_size_bytes）+ .part 清理）；幂等重跑（行 Completed + 文件存在 →
      跳过）；.part 缺失 → runtime_error 明确失败 + 状态不伪造 + worker 存活
      （恢复作业继续）；discard 幂等删除 + 重跑；启动清扫（活动保留/终态行
      清理/无行清理/非法名清理，removed==3）；路径安全（TransferId 空长字符
      集合非法字符拒绝、穿越载荷净化后落盘名无分隔符无穿越）。
    - 全部文件作业经 DatabaseWorker 串行执行（验收 ⑤）：complete/discard
      作业以 DbJob 入队、drain 后断言；wakeup/延迟上界在 M2-05 已实测。
  - 过程修正：联调修正两处用例缺陷——① 中文文件名净化预期按字节计（每非
    [A-Za-z0-9._-] 字节 → '_'，非按字符）；② missing 用例的 future 须在
    enqueue（move）前获取（move 后 done 为空 → SIGSEGV）。
  - 限制：ASAN/UBSAN 随本 PR Linux CI 门禁（sqlite3.c 已在 asan preset C
    编译面）；POSIX 分支（data_root XDG 路径、文件操作）随 CI Linux 编译
    执行，本机（Windows）不宣称已验证 POSIX 行为；MinGW 完整构建沿用
    M1-02 记录限制 1；MR 闭环由后续环节执行，本记录不含 commit/CI 证据。
  - 同步：本里程碑工作项 `M2-06`、总计划当前状态。
- 2026-09-23（`M2-07`，Windows 11 / MSVC 2022 BuildTools 14.44.35207 /
  CMake 4.1.0 / w64devkit GCC 15.2.0（语法检查））：
  - 范围：`persistence/recovery/startup_recovery.hpp/.cpp`（启动恢复组合，
    设计第 11.1 节 ②：数据根注入 → `<root>/db` 目录创建 → open → v1 迁移 →
    四仓储 `load_all()` → `sweep_tmp_orphans`；`RecoveryResult` 携带
    Repositories（单一连接整体移交 worker）、shared_ptr<FileStore>、
    RecoveredData 与诊断计数）、`persistence/repository/update_jobs.hpp/.cpp`
    （§11.1 ① typed 更新的域级 DbJob 工厂：四 upsert / 送达状态列 / 进度列 /
    无文件联动终态列；SetPresence/SetConnectionPath 无作业）、根 `main.cpp`
    （组合根：恢复 → 播种 → Manager → 注册 DatabaseWorker → RouterSink；
    `PersistenceMirror` 写路径接线；关闭钩子末尾排空；进程内 session B 重开
    逐域断言；argv[1] 数据根基址 + 每运行独立子目录自清理，argv[2] `--exact`
    为驱动/诊断钩子）、`tests/integration/test_restart_recovery.cpp` +
    `tests/CMakeLists.txt`（integration 标签测试 + configure 期损坏 DB fixture
    的宿主级 `recovery.corrupt_db_clean_failure` ctest + 冒烟测试改注入基址）、
    `persistence/CMakeLists.txt`、设计 §14 目录树（补记 M2-06 `storage/` 并
    新增 `recovery/`——M2-06 未同步 §14 属既成偏差，本项一并修正）。
  - 依据：[设计第 11.1 节 ②①③④](../design/aki_design.md)、第 8.3 节钩子
    序列（M2-01 前向引用的排空落点）、第 10.1 节单写者纪律、第 14 节；
    [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)（验证方式：
    重启恢复 / 损坏 DB / 排空后 join / admission 拒绝可见；目录布局、迁移
    纪律）；[DEC-008](../decisions/DEC-008-application-layer.md)（宿主关闭
    钩子先取消并消费在途 future）；M2-03~06 既有 API（RAII/迁移/仓储/
    DatabaseWorker/FileStore/file_jobs）；总计划 `SCOPE-09`、`EXEC-01`/
    `EXEC-02`/`EXEC-04`、`RULE-02`/`RULE-09`/`RULE-10`、`DOD-02`（六项已随
    M2-05 worker 路径覆盖，本项无新并发路径，复验宿主组合 shutdown 排空
    不丢作业）、`DOD-05`；executor-integration blocking-io 卡（本会话按
    SKILL 路由加载：run(StopToken)/wakeup、request_stop 不 join、stop()
    join、shutdown 后不保留 worker 引用）。
  - 实现要点：① 恢复在 `ExecutorOwner.initialize()` 之后、Manager/Adapter
    之前于主线程同步执行，不经 blocking worker；`DatabaseWorker` 与
    RouterSink 在播种完成后注册（EXEC-02 启动段纪律）；② 写路径按
    §11.1 ① 由 owner 单写者上下文（宿主主线程）按接受顺序入队：宿主每步
    quiesce（pumps flush + owner drain 至静止）后以快照中已接受的实体镜像
    typed 更新（Manager 构造的时间戳等载荷以快照权威值为准，拒绝的更新
    不会出现在快照、不会被镜像）；幂等 no-op 终态宣告同样入队、由作业侧
    幂等吸收；admission 拒绝与执行失败经 mirror 计数 + control 计数 + future
    逐个消费可见；③ 关闭钩子顺序：取消在途可取消任务 → flush 各 Manager →
    停 Adapter → `AppStateOwner.close()` → `request_drain()` + 有界等待
    （3s > drain_budget 2s）——DB 排空位于钩子末尾（§11.1 ③，先于 EXEC-01
    步骤 2/3）；④ `CompleteTransfer(Completed)` 镜像为 M2-06 文件作业组，
    `Failed`/`Cancelled` 镜像为终态列更新 + `.part` 幂等删除两个串行作业；
    ⑤ session B 重开断言 presence 归一化为 Offline（易失状态语义）。
  - 实现与设计的偏差（如实记录）：宿主写路径采用「快照权威值镜像」而非在
    AppStateOwner 内增加接受后回调——后者需改 app/state 公开契约（本项约束
    明确不改）。快照即已接受更新的累积权威态（§10.1），由此镜像满足
    「更新驱动、不由事件驱动」（§11.1 ①）与「owner 单写者上下文按接受顺序
    入队」；差异为幂等 no-op 接受在快照无变化时不产生作业（§11.1 ① 允许
    入队并由作业侧幂等吸收，入队侧省略不影响终态收敛，作业幂等语义已在
    M2-06 覆盖）。Manager 更新拦截的正式落点（接受后回调或 owner 侧 tap）
    待 M3 真实事件源接入时按 §11.1 ① 评估，届时如需改 AppStateOwner 契约
    先更新第 10.1/11.1 节。
  - 验证（生成器说明同 M1 记录；本机 MinGW 限制 1 未变化）：
    - `ctest --preset debug -C Debug --timeout 120` → 19/19（原 17 + 新
      `test_restart_recovery` 4 test case / 158 断言 + 宿主级
      `recovery.corrupt_db_clean_failure`）。
    - `ctest --preset release -C Release --timeout 120` → 19/19。
    - 稳定性：宿主 `aki` debug 连续 30 次 / release 连续 15 次全部
      `smoke: PASS`；`test_restart_recovery` debug 30 次 / release 15 次
      全过。
    - 损坏 DB 宿主级实测：`aki <corrupt-root> --exact` → 输出
      `[FATAL] startup recovery failed: file is not a database`、退出码 2
      （不静默；组件级同场景断言 `SqliteError.code()==26`（SQLITE_NOTADB），
      失败后同组件对有效根仍正常恢复）。
    - 覆盖映射（验收 ①② = 退出-1）：重启逐域一致（设备含信任推进
      Unknown→Pending→Trusted、会话 Active→Disconnected→Active、消息含
      送达终态 Sent→Delivered 与时间戳/payload 无损往返、传输历史
      t-1 Completed 含 stored_* 回写位与文件本体落盘 + t-2 Cancelled），
      宿主与集成测试双路径断言；迁移幂等（重开 0 步）；清扫（活动保留 /
      终态与无行清除 removed==2）；排空不丢作业（宿主 admitted==completed
      16==16、集成测试 25 作业零丢失、fully_stopped、drain_budget 未耗尽、
      rejected==0）。
    - GCC 语法检查（CI Linux 告警姿势）：`g++ -std=c++20 -Wall -Wextra
      -Wpedantic -Werror -fsyntax-only -I. -Ithird_party/executor/include
      -Ithird_party/sqlite` 对 `startup_recovery.cpp`、`update_jobs.cpp`、
      `main.cpp` 通过；对 `test_restart_recovery.cpp`（含 Catch2 include）
      通过。
    - RULE-10 证据：`grep -rn "#include <sqlite3.h>" persistence/` 仍仅
      `database.cpp:4`；`startup_recovery.hpp`/`update_jobs.hpp` 无
      `executor/` include、无 `sqlite3_` 出现（executor 类型仍封在
      database_worker_adapter.hpp 接线层）。
  - 限制：ASAN/UBSAN 随本 PR Linux CI 门禁（无新并发路径：恢复主线程同步、
    写路径入队复用 M2-05 worker 通道，DOD-02 六项不重复立项）；POSIX 分支
    随 CI Linux 编译执行，本机（Windows）不宣称已验证 POSIX 行为；MinGW
    完整构建沿用 M1-02 记录限制 1；MR 闭环由后续环节执行，本记录不含
    commit/CI 证据。
  - 同步：设计第 14 节、本里程碑工作项 `M2-07` 与退出-1、总计划当前状态。

- 2026-09-23（`M2-08`，收口审计与退出证据归集，Windows 11 / MSVC 2022
  BuildTools 14.44.35207 / CMake 4.1.0 / gh 2.x（CI 核实）；纯文档与证据
  归档变更，无产品代码改动）：
  - 范围：本里程碑文档（M2-08 勾选、退出-2~5 勾选与证据、状态 Completed）、
    [DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)（验证方式逐项
    回填）、`docs/supply-chain/`（新建：README 依赖审计索引 + sqlite-3.53.4
    vendored 审计与上游缺陷处置）、总计划（当前状态条目 + 里程碑索引 M2 →
    Done）。
  - ① 设计-实现审计矩阵（逐项校对，结论全部一致）：
    - §11（ER 四表/列）：`schema_v1.cpp` 四表 + FK（DEVICE 1-* CONVERSATION
      1-* MESSAGE 0..1 TRANSFER）与 DEC-004 列补齐（stored_relative_path/
      stored_sha256/stored_size_bytes、file_name 仅展示）一致；§11 ER 块的
      path/hash/size_bytes 为概念名，具体列名以 §11.1 ④ 与 M2-04 记录的
      stored_* 为锚，无未记录偏差。
    - §11.1 ①（写路径映射）：update_jobs 五工厂 + file_jobs 终态作业组对
      UpsertDevice/Conversation/Message/Transfer/UpdateTransferProgress/
      SetDeliveryState/CompleteTransfer 映射一致；SetPresence/
      SetConnectionPath 无作业（DEVICE 无 presence 列佐证）；FIFO 串行保序
      （M2-05 测试）；「owner 单写者上下文按接受顺序入队」的宿主形态偏差见
      已知偏差 ②。
    - §11.1 ②（启动恢复）：main.cpp:350 initialize → :358 恢复 → :372 播种
      → :380 Manager 构造 → :410 注册 worker → :419 RouterSink（恢复完成前
      无事件源）；startup_recovery.cpp 顺序 open→迁移→逐域加载→清扫与契约
      一致；损坏 DB 干净失败（FATAL + 退出码 2，ctest 正则断言）。
    - §11.1 ③（关闭排空落点）：main.cpp 关闭钩子 close() → request_drain →
      有界等待（3s > drain_budget 2s）→ EXEC-01 步骤 2/3；「有界等待通道」
      的实现形态偏差见已知偏差 ①。
    - §11.1 ④（文件本体）：FileStore 布局/.part/Completed 作业组/
      Failed-Cancelled 幂等删除/启动清扫一致；Manager 无文件 I/O
      （grep app/ 无 filesystem/fstream 命中）；数据根解析公开面仅
      std::string（data_root 平台条件编译单元）。
    - §14（目录）：persistence/{database,repository,migration,storage,
      recovery} 与实际一致（M2-07 已同步，含 M2-06 storage/ 补记）。
    - §8.2/8.3/10.1（关联节）：blocking worker 首次启用落点、宿主钩子序列
      （M2-01 前向引用的排空末尾）、恢复期单写者播种（owner 尚未运行）均与
      实现一致；§8.2「进程内需要新一轮生命周期时重建 owner」与 M2-07
      session B（恢复不经 executor，无第二轮 owner）无冲突。
    - 已知偏差锚点核实：① M2-05 轮询环替代 receive_for——本文件 M2-05 记录
      「实现与调研/设计的偏差」段（行 503 起）+ database_worker_adapter.hpp
      实现注记；② M2-07 快照权威值镜像替代接受后回调——本文件 M2-07 记录
      「实现与设计的偏差」段（行 644 起）。**未记录偏差数：0**。
    - RULE-07/RULE-10 grep 抽查（2026-09-23 复跑）：`grep -rn "#include
      <sqlite3.h>" persistence/` 仅 database.cpp:4；公开头 `sqlite3_` 仅
      database.hpp:15/:31 注释行；persistence 公开头无 `executor/` include
      （executor 类型仅在 database_worker_adapter.hpp 接线层与
      database_worker.cpp 实现）；`grep -rnE "std::jthread|std::async|
      std::thread\s*[({]|CreateThread|_beginthread"` 第一方代码（app/device/
      conversation/transfer/persistence/heyaki/ui/main.cpp/tests）零命中
      （仅注释行）。
  - ② 退出证据与复跑（命令与结果）：
    - 本地全量：`cmake --build --preset debug --config Debug && ctest
      --preset debug -C Debug --timeout 120` → 100% tests passed, 0 failed
      out of 19；release 同构 → 19/19。
    - 退出-2 具名复跑：`ctest --preset debug -C Debug -R "test_database_worker|
      test_persistence_database|test_persistence_repository" --timeout 120`
      → 3/3 Passed（0.09s/0.04s/2.16s）。
    - CI 核实（gh，2026-09-23）：PR #13~#18 逐 PR `gh pr view N --json
      statusCheckRollup` 全部 SUCCESS——Linux debug/asan/ubsan + Windows
      MSVC（#13~#15 四项），#16~#18 五项（+Linux tsan，DOD-03 DB worker
      跨上下文证据）。sanitizer run 链接：
      #13 asan/ubsan runs/35772094181（jobs 106896050795/106896050514）、
      #14 runs/35776673982（106911531233/106911531032）、
      #15 runs/35793399233（106966970429/106966970392）、
      #16 runs/35804154579（107001042623/107001042726/tsan 107001042618）、
      #17 runs/35814243010（107032242299/107032242329/tsan 107032242336）、
      #18 runs/35874286726（107226064386/107226064601/tsan 107226064480）
      （github.com/Linductor-alkaid/Aki/actions）。
  - ③ 供应链登记：`docs/supply-chain/README.md`（审计索引与升级流程）+
    `docs/supply-chain/sqlite-3.53.4.md`（zip URL + SHA3-256 `628a44cf…934e`
      + 双文件 SHA-256（M2-08 本地 python hashlib 复算与锁文件一致：
      `b1dd5d74…8189` / `919e7f2e…0e1d`）、public domain 许可证结论
    （sqlite.org/copyright.html）、构建宏/sourceid 断言/sanitizer 结论）。
  - ④ 上游缺陷处置结论（sqlite-3.53.4.md「上游缺陷处置」节）：3.53.4 约束
    失败语句 reset 复用异常终止（三路复现 + run_cached 逐出规避，M2-04/05
    记录）；2026-09-23 核实 sqlite.org changelog 最新稳定版仍为 3.53.4，
    无包含修复的新版本——升级不可行亦无必要；建议由仓库负责人向 sqlite.org
    报告（需提交者账户，未代为提交）；触发条件登记：官方发布 3.53.5+ 时先
    复跑复现脚本再按 DEC-003 升级流程评估。复现脚本保留于 build/scratch/
    （repro_raw2.cpp 等会话工件，复现配方已写入 supply-chain 文档，自包含）。
  - 限制：MinGW 完整构建沿用 M1-02 记录限制 1（本地以 MSVC 等价执行）；
    POSIX 分支行为由 CI Linux 三档（debug/asan/ubsan/tsan）编译执行覆盖；
    上游报告为负责人待办（未代提交，见 ④）；MR 闭环由后续环节执行，本记录
    不含 commit/CI 证据（CI 链接为已合入 PR 的 run 归档）。
  - 同步：DEC-004 验证方式回填、总计划当前状态与里程碑索引（M2 → Done）、
    本里程碑状态（Completed）。
