# DEC-004：本地持久化采用 SQLite

> 状态：Accepted
> 日期：2026-09-22
> 负责人：Linductor
> 冻结里程碑：M2 开始前（2026-09-22 提前冻结，调研依据见文末）
> 替代/被替代：无

## 背景与问题

设计第 11 节要求本地持久化保存设备身份、受信设备、Conversation metadata、消息历史、
Transfer history 与应用设置；大文件本体保存在文件系统，数据库记录 metadata。需要确定
存储引擎与访问方式，并按 `DEC-003` 的锁定纪律确定第三方引入方式。

## 决策

- **引擎与访问方式**：SQLite C API + 自研薄 RAII 封装（`Database` / `Statement` /
  `Transaction` 守卫 + `SqliteError`），不引入 ORM 或第三方 C++ 封装库；`sqlite3*`
  封死在 persistence 层，不得出现在 Core 公开头文件（`RULE-10`）。schema 直接对应设计
  第 11 节 ER 模型 4 张表（DEVICE / CONVERSATION / MESSAGE / TRANSFER）；领域枚举
  （`TrustState`、`MessageType`、`DeliveryState`、`TransferState`）映射
  `INTEGER + CHECK`。数据库访问属阻塞 I/O，运行期经 Executor blocking worker 承载
  （`EXEC-04`）：单一 `DatabaseWorker`（`start_worker(BlockingWorkerSpec)`）独占一个
  连接、从有界工作通道串行消费 DB 作业；取消经 stop token 在语句间检查；admission
  拒绝与执行失败经 Executor 监控设施暴露（`EXEC-06`）；容量上限落在工作通道与
  prepared-statement 缓存（`RULE-09`）。shutdown 顺序遵循 `EXEC-01`。
- **引入方式**：sqlite.org 官方 amalgamation 源码（当前锁定 3.53.4，2026-07-24 发布，
  public domain）并入 `third_party/sqlite`，仅 `sqlite3.c` / `sqlite3.h` 两个文件，非
  submodule；在 `third_party/dependencies.lock.json` 登记 `class=vendored` +
  `submodule=false` + 版本 + `sqlite3.c` / `sqlite3.h` 的 SHA-256，并扩展
  `cmake/Dependencies.cmake` 增加 vendored 校验分支（`file(SHA256)` 比对，不匹配即
  `FATAL_ERROR`），与 `DEC-003` 的“锁定 + configure 校验、漂移即失败”纪律一致。
  编译宏按官方建议最小化：`SQLITE_DQS=0`、`SQLITE_OMIT_LOAD_EXTENSION=1`；连接期
  runtime pragma 统一在薄封装 open 处设置 `journal_mode=WAL`、`synchronous=NORMAL`、
  `foreign_keys=ON`、`busy_timeout`。DB 文件必须位于本地盘（WAL 不支持网络文件系统）。
- **文件本体布局**：数据根目录（平台 app-data，由 Platform Adapter 解析，Core 不见
  平台类型）下 `db/aki.db3` 与 `files/` 并置。写入期文件放
  `files/tmp/<transfer_id>.part`；进入终态 `Completed` 时在 blocking worker 内流式计算
  SHA-256，随后原子改名到 `files/<transfer_id>/<净化文件名>`。DB 只存 POSIX 相对路径、
  hash、size、MIME；远端提供的原始文件名只存 DB 供展示，禁止拼入路径（防路径穿越）。
  `TransferId` 生成处固化字符集约束（`[A-Za-z0-9_-]{1,64}`）作为目录名安全的前提。
  `Failed` / `Cancelled` 终态幂等删除对应 `.part`；启动时清扫无活动 Transfer 行的
  tmp 残留（崩溃恢复）。不做 CAS/内容寻址去重（需引用计数 GC，MVP 无此压力），记为
  带触发条件的延后项。
- **迁移**：schema 变更走 `persistence/migration` 版本化迁移（`PRAGMA user_version`），
  不直接改历史 schema。

## 备选方案

- 系统库 `find_package(SQLite3)`：已否决——本机 w64devkit GCC 15.2 无 `sqlite3.h`；
  Windows/MSVC 无系统 SQLite，CI 只能走包管理器路线（与 `DEC-003` 的 pinned + configure
  校验纪律不一致）；发行版版本不受控，schema 行为漂移破坏可复现性。
- submodule 指向官方 `sqlite/sqlite` 镜像：canonical 源码树无预生成 amalgamation，构建
  需 TCL/AWK/SED 代码生成步骤，给 MinGW/MSVC/CI 三套工具链都增加负担，否决。
- submodule 指向第三方 amalgamation 镜像：为官方已带哈希发布的文件引入非官方供应链
  一跳，收益为零，否决。
- ORM（sqlite_orm / SOCI / sqlpp11）或 C++ 封装库（SQLiteCpp）：为 4 张固定表引入新
  pinned 依赖与宏/反射面；版本化迁移无论如何要写裸 SQL，自研薄封装更干净地满足
  `RULE-10`，否决。
- 每设备 JSON/KV 存储（LMDB 等）：查询、迁移与并发写能力弱，消息关联查询需自建索引，
  否决（维持 2026-09-21 初版结论）。
- CAS/内容寻址 blob 存储：延后项，触发条件为“重复大文件造成可观测存储压力”。

## 影响与风险

- `cmake/Dependencies.cmake` 现循环假定全部依赖是 git repo（对每条执行
  `git rev-parse HEAD`）；vendored 条目须新增 `file(SHA256)` 校验分支，保持
  “漂移即 FATAL_ERROR + 修复提示”语义。
- `sqlite3.c` 纳入 asan/ubsan preset 编译；若 UBSan 对 SQLite 内部报点，仅在 sqlite
  target 上做编译旗标豁免并记录，不放松全局。
- 供应链：只从 sqlite.org 下载 amalgamation zip；锁文件登记版本、zip 的 SHA3-256 与
  解包后两个文件的 SHA-256；升级走 `DEC-003` 的独立变更 + supply-chain 审计流程。
  建议在 debug 构建断言 `sqlite3_sourceid` 与锁文件版本一致。
- WAL 约束：db 与 files 必须在本地盘；Windows `%APPDATA%` / Linux XDG 数据根目录解析
  落在 Platform Adapter 并写测试。
- 进程外竞争（用户用 sqlite3 CLI 同时打开 db）属预期场景，由 `busy_timeout` 覆盖。

## 验证方式

M2 退出条件（在 M2 里程碑测试矩阵中执行）：重启后恢复设备、会话、消息与传输历史；
迁移 `user_version` 前进与回滚失败路径；DB 文件损坏时 open 干净失败；传输取消删除
`.part`；shutdown 时在途 DB 作业 drain 后才 join（`EXEC-01` 顺序）；工作通道满时
admission 拒绝可见（`RULE-09` / `EXEC-06`）；CI（Linux debug/asan/ubsan + Windows
MSVC）全绿。

## 关联文档和工作项

- [Aki 设计方案](../design/aki_design.md)第 11 节
- [Aki 实施总计划](../plans/aki-implementation-plan.md)（`SCOPE-09`、`RULE-09`、`RULE-10`、`EXEC-04`，M2）
- [DEC-003：依赖锁定](DEC-003-dependency-locking.md)
- [M2：本地持久化](../plans/aki-implementation-plan.md)（里程碑文档待 M2 启动时创建）

## 调研依据

2026-09-22 冻结调研（负责人 Linductor）核实：sqlite.org/download.html 官方发行物即
amalgamation（sqlite-amalgamation-3530400.zip，SHA3-256
`628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e`）；
sqlite.org/howtocompile.html 明言 canonical 源码树需 TCL/AWK/SED 生成步骤并推荐应用
使用 amalgamation；sqlite.org/changes.html 最新稳定版 3.53.4（2026-07-24）；
sqlite.org/copyright.html 确认 public domain；sqlite.org/wal.html、quirks.html 支撑
WAL/NORMAL/DQS 编译与运行配置结论。M1 领域类型（`TrustState` 等均为 u8 枚举、
`FileMetadata` 仅 name/size/mime）与该 schema 映射兼容，`hash` 与本地路径字段由本
决策补齐。本决策在 M1-02 工作项内随调研结论一并冻结，实现自 M2-01 起。
