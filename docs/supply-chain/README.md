# 供应链审计登记（Supply Chain）

按[工程规范第 10.7 节](../project/project-standards.md)与
[DEC-003](../decisions/DEC-003-dependency-locking.md)/[DEC-004](../decisions/DEC-004-local-persistence-sqlite.md)，
第三方依赖的引入与升级在此登记审计结论。锁定事实的唯一权威源是
`third_party/dependencies.lock.json` + `cmake/Dependencies.cmake` 的 configure
校验（pinned：submodule commit 比对；vendored：逐文件 SHA-256 比对，漂移即
`FATAL_ERROR`）；本目录登记的是审计结论与升级处置记录，不替代锁文件。

| 依赖 | 引入方式 | 版本 | 许可证 | 审计记录 |
| --- | --- | --- | --- | --- |
| executor | submodule（pinned `74a9419`） | v0.5.0-7 | MIT | [DEC-003](../decisions/DEC-003-dependency-locking.md)（M0 冻结，升级走独立变更后在此补登记） |
| EUI-NEO | submodule（pinned `b9032a8`） | v0.6.0 | Apache-2.0 | 同上；assets 许可证审计为发行前检查项（设计第 9 节） |
| heyaki | submodule（pinned `1ceb42c`） | v1.1.1 | MIT | [heyaki-v1.1.1.md](heyaki-v1.1.1.md)（M5-33，回归进行中） |
| sqlite | vendored（仅 `sqlite3.c`/`sqlite3.h`） | 3.53.4 | public domain | [sqlite-3.53.4.md](sqlite-3.53.4.md)（M2-08，2026-09-23） |

升级流程（DEC-003 / 工程规范 10.7）：任何依赖升级为独立变更——更新
submodule/文件与锁文件、说明版本差异与许可证变化、回归通过后在本目录登记
审计结论。vendored 依赖升级前必须先复跑既有缺陷规避的复现脚本（见
[sqlite-3.53.4.md](sqlite-3.53.4.md) 上游缺陷处置节）。

## 2026-10-01：授权的 EUI X11 临时构建补丁

用户明确授权 EUI-20261001-002，见 [台账](../eui_neo_feedback/ledger.md)和
[上游 #78](https://github.com/sudoevolve/EUI-NEO/issues/78)。EUI pin 不变。
`cmake/EuiX11ImePatch.cmake` 仅在 Linux + X11 为 GLFW target 替换单一
编译单元：原源文件复制到构建目录后应用仓库补丁，原件/补丁/产物三项
SHA256 按 dependencies.lock.json 校验。原 checkout 不改，漂移即失败，
Windows 不覆盖。每次 configure 从原件重新生成，升级后必须重新评审或
移除此边界；上游正式修复升级并通过 GUI 验证后删除补丁和 manifest。
