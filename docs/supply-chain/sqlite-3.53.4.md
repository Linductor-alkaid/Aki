# SQLite 3.53.4 vendored 审计结论

> 登记日期：2026-09-23（M2-08，随 M2 收口）
> 负责人：Linductor
> 依据：[DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)（引入决策）、
> [DEC-003](../decisions/DEC-003-dependency-locking.md)（锁定纪律与升级流程）、
> 工程规范 10.7；首次引入与获取/校验过程见
> [M2 里程碑验证记录 M2-02](../plans/m2-local-persistence.md)。

## 引入物与哈希

- **来源 artifact**：`https://sqlite.org/2026/sqlite-amalgamation-3530400.zip`
  （sqlite.org/download.html 官方发行物，version 3.53.4，2026-07-24 发布）。
- **zip SHA3-256**（sqlite.org 下载页标注与 DEC-004 调研记录一致，先校验后解包）：
  `628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e`
- **入库文件**（仅两文件，zip 内 shell.c / sqlite3ext.h 未纳入）：

| 文件 | SHA-256 | 本地复算（2026-09-23，M2-08） |
| --- | --- | --- |
| `third_party/sqlite/sqlite3.c` | `b1dd5d74ec7f29055a6684fa06fb3c2f6821c87dd38f9a458dfd2e8a1db28189` | 一致（python hashlib.sha256） |
| `third_party/sqlite/sqlite3.h` | `919e7f2e8ed1d8f56ac17b412b8971c76aa5d1a879752cc6058f75e7d5910e1d` | 一致（python hashlib.sha256） |

锁文件登记：`third_party/dependencies.lock.json` 的 sqlite 条目
（`class=vendored`、`submodule=false`、版本、双文件 SHA-256、`source_artifact`
溯源字段）。configure 校验：`cmake/Dependencies.cmake` vendored 分支逐文件
`file(SHA256)`，缺失/漂移即 `FATAL_ERROR`（负向验证见 M2-02 验证记录：篡改
SHA-256 后 configure 失败并输出 actual/required 双哈希与官方来源 URL）。

## 许可证结论

**public domain**。依据 sqlite.org/copyright.html（DEC-004 调研核实）：作者
放弃全部版权，无附加条件，可静态编译并入发行物，无需随附许可证文本或履行
notice 义务。仓库层面仍在锁文件登记 `license: "public domain"` 以保持依赖
清单完整。

## 构建与验证结论

- 编译宏（官方建议最小化）：`SQLITE_DQS=0`、`SQLITE_OMIT_LOAD_EXTENSION=1`
  （`third_party/sqlite/CMakeLists.txt`；第三方 target 不继承第一方告警级别）。
- debug 构建断言 `sqlite3_sourceid` 与锁文件版本三方一致（header 宏 /
  `sqlite3_libversion` / 锁文件注入值，`test_sqlite_sourceid`）。
- sanitizer：`sqlite3.c` 为 C 编译单元，asan/ubsan/tsan preset 已补
  `CMAKE_C_FLAGS` 插桩；PR #13~#18 的 Linux asan/ubsan（#16~#18 另含 tsan）
  CI 全绿（run 链接见 M2 里程碑 M2-08 验证记录）。UBSan 未对 SQLite 内部
  报点，无需旗标豁免。

## 上游缺陷处置（3.53.4 约束失败语句的 reset 复用异常终止）

**现象**：SQLite 3.53.4 中，语句 step 因外键约束失败（`SQLITE_CONSTRAINT`）
后对其 `sqlite3_reset` 会异常终止（非返回错误码）。三路复现：官方预编译
DLL、本地 MSVC 编译、w64devkit GCC 编译（`:memory:` 库、多外键子表——
conversation 含 2 外键；单外键无 CHECK 的表不复现）。纯 C API 复现脚本与
最小化变体保留于 `build/scratch/repro_raw2.cpp` 等会话工件（未入库；复现
配方：构造含两个外键与 CHECK 的子表，prepare INSERT → step 至约束失败 →
reset 复用 → 异常终止）。

**规避（已实现）**：仓储层全部语句经 `run_cached` 执行，`SqliteError` 时
`StatementCache::invalidate` 逐出该语句（确定性 finalize），调用方重试总是
fresh prepare（fresh 语句行为正确）。影响面仅"约束失败后复用同一 prepared
语句"路径；正常写入/恢复路径（父行先于子行）不受影响。回归测试：
`test_persistence_repository`（外键拒绝路径）+ `test_persistence_database`
（prepare/step 错误路径）持续通过。

**版本升级评估（2026-09-23）**：sqlite.org 官方 changelog 最新稳定版仍为
3.53.4（2026-07-24），无包含该缺陷修复的新版本，**升级不可行亦无必要**
（规避有效、已测试覆盖）。

**上游报告处置**：缺陷可稳定复现且影响一般集成者，**建议由仓库负责人向
sqlite.org 报告**（官方渠道：sqlite.org 支持/论坛，需提交者账户，超出本
里程碑 agent 权限，未代为提交）。触发条件：官方发布 3.53.5+ 时，先以
`build/scratch/repro_raw2.cpp` 同型脚本对新版本复跑确认是否已修复，再按
DEC-003 升级流程评估升级；若上游确认/修复，在本文档回填 issue/变更链接。

## 升级流程约束

升级为独立变更（DEC-003 / 工程规范 10.7）：下载新 zip 先校验官网标注哈希
→ 仅取两文件替换 → 更新锁文件（版本 + 双文件 SHA-256 + source_artifact）
→ 升级前复跑上游缺陷复现脚本 → 全量回归（本地 MSVC debug/release + CI
Linux debug/asan/ubsan/tsan）→ 本文档登记新审计结论。
