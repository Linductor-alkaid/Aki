# M2：本地持久化

> 状态：In Progress
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
  路径全覆盖（`test_database_worker` 7 test case / 87 断言，40 次稳定性
  通过）。详见验证记录。）
- [ ] `M2-06` 落实文件本体存储布局与生命周期：数据根目录（Platform Adapter 解析，
  Windows `%APPDATA%` / Linux XDG，Core 不见平台类型）下 `db/aki.db3` 与 `files/`
  并置；`files/tmp/<transfer_id>.part` 写入、`Completed` 终态流式 SHA-256 后原子
  改名到 `files/<transfer_id>/<净化文件名>`、`Failed` / `Cancelled` 幂等删除
  `.part`、启动清扫无活动 Transfer 行的残留；`TransferId` 字符集约束
  `[A-Za-z0-9_-]{1,64}` 固化于生成处，远端原始文件名只存 DB 不拼路径
  （`DEC-004`；本项以测试内字节源驱动，真实链路 M4）。
- [ ] `M2-07` 实现重启恢复并写入宿主/测试路径：设备、信任状态、会话、消息与
  传输历史写入 → 受控关闭 → 重新 open 后逐域断言一致（`SCOPE-09` 验收）；
  DB 文件损坏时 open 干净失败不静默。
- [ ] `M2-08` 收口审计与退出证据归集（沿用 M1-08 纪律）：实现与设计第 11 节/
  `DEC-004` 逐项校对，退出-1~5 证据与可复现命令归档，供应链登记
  （zip SHA3-256 + 文件 SHA-256、public domain 许可证结论入
  `docs/supply-chain/`）。

## 风险与阻塞

- `sqlite3.c` 纳入 asan/ubsan preset 编译；若 UBSan 对 SQLite 内部报点，仅在
  sqlite target 上做编译旗标豁免并记录，不放松全局（`DEC-004` 影响节）。
- 本地 MinGW 默认生成器仍受 pinned executor 构建缺陷阻塞（M1 验证记录限制 1），
  本地验证以 MSVC 生成器等价执行；`Dependencies.cmake` 的 vendored 校验分支属
  configure 逻辑，需在 MinGW/MSVC/CI Linux 三套工具链的 configure 均验证。
- WAL 约束：db 与 files 必须位于本地盘；网络文件系统场景在测试中显式记录限制
  （`DEC-004`）。

## 测试与退出条件

- [ ] 退出-1：重启恢复——写入 → 关闭 → 重开，设备/信任/会话/消息/传输历史逐域
  一致；损坏 DB open 干净失败。
- [ ] 退出-2：并发基线六项沿 `DatabaseWorker` 路径通过——正常完成、任务异常、
  提交拒绝（通道满/关闭后）、执行中取消（语句间 StopToken）、超时、shutdown
  （drain 后 join）。
- [ ] 退出-3：迁移与约束测试通过——`user_version` 前进成功、失败路径不破坏既有
  数据；枚举 `CHECK` 拒绝非法值；重复迁移幂等。
- [ ] 退出-4：debug/release 构建与全量测试通过；ASAN/UBSAN 通过（随 CI 门禁），
  不适用工具链记录限制与补跑条件；DB worker 与 Manager 跨上下文交互按 `DOD-03`
  评估 TSAN/故障注入。
- [ ] 退出-5：设计（第 11 节集成契约）、决策（`DEC-004` 验证方式回填）、总计划
  状态同步；验证记录含可复现命令与结果；供应链文档已登记。

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
      `test_database_worker` 7 test case / 87 断言）。
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