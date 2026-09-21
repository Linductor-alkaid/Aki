# DEC-003：依赖锁定方式与 pinned 依赖来源

> 状态：Accepted
> 日期：2026-09-21
> 负责人：Linductor
> 冻结里程碑：M0
> 替代/被替代：无

## 背景与问题

AGENTS.md 要求 Aki 依赖仓库中的 pinned `third_party/executor` 作为唯一并发基础设施；
设计文档同时依赖 EUI-NEO（GUI）与 Heyaki（网络）。工程规范第 9.1 节允许两种锁定方式：
submodule + `dependencies.lock.json`，或 lock 清单 + CMake 拉取。初始化时 `third_party/`
为空，无法 pin；2026-09-21 由 Linductor 提供三个依赖的来源并完成拉取，本决策随之冻结。

## 决策

- 锁定方式采用 **submodule + `dependencies.lock.json`**：`.gitmodules` 声明三个
  submodule；`third_party/dependencies.lock.json` 记录 source、精确 commit、版本号、
  许可证及许可文件路径；`cmake/Dependencies.cmake` 在 configure 时校验各 submodule
  HEAD 与锁定 commit 一致，缺失或漂移即 FATAL_ERROR（缺失时提示
  `git submodule update --init <path>`，漂移时提示 `git -C <path> checkout <commit>`）。
- 已锁定的依赖（2026-09-21）：

| 依赖 | 路径 | 来源 | pinned commit | 版本 | 许可证 |
| --- | --- | --- | --- | --- | --- |
| executor | `third_party/executor` | https://github.com/Linductor-alkaid/executor.git | `74a94198fbe0f2a4081cd260658a26f969986870` | v0.5.0-7 | MIT |
| EUI-NEO | `third_party/EUI-NEO` | https://github.com/sudoevolve/EUI-NEO.git | `b9032a8a848f8d8cf096bb7711c626f9c051e0ce` | v0.6.0 | Apache-2.0 |
| heyaki | `third_party/heyaki` | https://github.com/Linductor-alkaid/heyaki.git | `e114508ab32d496d52e9db9bac26eb1cc88c4ae7` | v1.0.1-38 | MIT |

- 依赖的目标级接入不在本决策范围：executor 于 M1、heyaki 于 M3、EUI-NEO 于 M5 分别
  按各自里程碑与集成指南接入构建图；本决策只覆盖锁定与校验。
- EUI-NEO 的 assets（字体、图标、shader、示例素材）许可证审计保留为发行前检查项
  （设计第 9 节）。
- 初始化期间曾预留 `AKI_FETCH_DEPENDENCIES` 开关；submodule 模式确定后该开关已移除，
  缺失依赖的同步路径统一为 `git submodule update --init`。

## 备选方案

- lock 清单 + CMake 拉取（`third_party/dependencies.lock`）：对 CI 离线缓存更友好；
  若后续 CI 出现 submodule 同步瓶颈，可在新决策中切换并在 configure 侧保持同样校验。
- vcpkg/Conan 等包管理器：与“pinned commit + configure 校验”的仓库纪律不一致，否决。

## 影响与风险

- 任何依赖升级走独立变更：更新 submodule、更新锁文件、说明版本差异与许可证变化、
  回归通过后在 `docs/supply-chain/` 记录审计结论（工程规范 10.7）。
- configure 校验依赖本机 git；CI 镜像需自带 git（与 submodule 工作流一致，无额外要求）。
- 未校验离线克隆场景下的行为差异（无 `.git` 目录的源码包会触发缺失错误并提示
  submodule 初始化），属预期行为。

## 验证方式与证据

2026-09-21（工作树状态，见 M0 验证记录）：

- `cmake --preset debug -G "MinGW Makefiles"`：三条
  `Dependency '<name>' pinned at <commit>` STATUS 通过，configure 成功。
- 负向测试：临时将锁文件中 executor 的 commit 改为全零后 configure 失败，报
  `... is required by third_party/dependencies.lock.json`；还原后重新 configure 通过，
  debug 构建 + ctest 2/2 通过。

## 关联文档和工作项

- [Aki 实施总计划](../plans/aki-implementation-plan.md)（`EXEC-01`~`EXEC-07`、`RISK-2026-001`）
- [M0：工程骨架与协作基线](../plans/m0-project-skeleton.md)（`M0-05`、退出-5）
- 工程规范第 9.1、10.7 节
