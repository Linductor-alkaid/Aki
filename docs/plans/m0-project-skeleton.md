# M0：工程骨架与协作基线

> 状态：In Progress
> 负责人：Linductor
> 所属计划：[Aki 实施总计划](aki-implementation-plan.md)
> 前置：无
> 建议发布点：无（仓库基线，不打 tag）
> 更新日期：2026-09-21

## 目标

在开始功能开发前建立仓库协作与构建基线：实例化协作规范文档，建立总计划/里程碑/决策/
台账文档框架，提供可构建、可测试的 CMake 骨架与预设矩阵、代码风格与静态检查配置，
并准备好 pinned 依赖的锁定机制。

## 范围与非目标

### 范围

- `AGENTS.md` 与[工程规范](../project/project-standards.md)从模板实例化为 Aki 内容。
- 总计划、M0/M1 里程碑文档、DEC-001~004、Executor 反馈台账。
- CMake 构建骨架：按设计第 14 节目录建立领域目标，`debug`/`release`/`asan`/`ubsan`/
  `tsan` 预设与对应 `ctest` 入口，无框架 smoke 测试。
- `.clang-format`、`.clang-tidy`、`.gitignore`。

### 非目标

- 任何领域功能实现（M1 起）；真实依赖接入（`M0-05` 解锁后）；CI workflow（首次 push 后）。
- 设计第 14 节之外的目录调整；如需调整先更新设计。

## 设计与决策依据

- [Aki 设计方案](../design/aki_design.md)第 14 节（工程目录）。
- [DEC-001](../decisions/DEC-001-cpp-baseline.md)：C++ 与构建基线。
- [DEC-003](../decisions/DEC-003-dependency-locking.md)：依赖锁定方式与来源（暂定）。
- AGENTS.md 工程约束；工程规范第 11 节（C++ 工程基线）。

## 工作项

- [x] `M0-01` 实例化仓库协作基线文档：`AGENTS.md` 与工程规范替换 `<PROJECT>` 占位符，
  固化产品目标、标准状态集、scope 词表与 `AKI_FETCH_DEPENDENCIES` 选项名。
- [x] `M0-02` 建立文档框架：总计划（SCOPE/RULE/EXEC/DOD/POST/风险）、M0/M1 里程碑、
  DEC-001~004、[Executor 反馈台账](../executor_feedback/ledger.md)。
- [x] `M0-03` CMake 构建骨架与预设矩阵：领域 INTERFACE 目标锁定依赖方向，5 个 configure/
  build/test 预设，smoke 测试（`skeleton.includes`、`skeleton.app_runs`）。debug/release
  已验证（见验证记录）；asan/ubsan/tsan 已定义，本机工具链不可用，限制已记录。
- [x] `M0-04` 代码风格与仓库卫生配置：`.clang-format`、`.clang-tidy`、`.gitignore`。
- [x] `M0-05` 依赖锁定落地（2026-09-21）：Linductor 提供三个依赖来源后，登记
  submodule 并写入 `third_party/dependencies.lock.json`，`cmake/Dependencies.cmake`
  在 configure 时校验 commit（含负向测试）。见
  [DEC-003](../decisions/DEC-003-dependency-locking.md)（已 Accepted）。
- [ ] `M0-06` CI 基线（GitHub Actions workflow，Linux GCC/Clang + Windows，执行预设矩阵）。
  **待触发**：仓库首次 push 后补充并验证；解除 `RISK-2026-003` 的 sanitizer 证据缺口。

## 风险与阻塞

- ~~`RISK-2026-001`：依赖来源未定~~ 已于 2026-09-21 解除：依赖完成 submodule + 锁文件
  登记，configure 校验通过，[DEC-003](../decisions/DEC-003-dependency-locking.md) 冻结。
- `RISK-2026-003`：本机 Windows/MinGW 工具链缺少 `libasan`/`libubsan`/`libtsan` 运行时，
  sanitizer 预设无法在本机执行（见验证记录），需 Linux CI 补齐证据（`M0-06`）。
- 设计第 14 节未包含 `tests/`、CMake 与 `.clang-*`/`.gitignore` 等仓库级文件，本次初始化
  按 DEC-001 补充，属对设计目录的扩展而非变更；如无异议在 M1 评审时并入设计文档。

## 测试与退出条件

- [x] 退出-1：`debug` 预设 configure + build + ctest 全部通过（本机 Windows x64 +
  w64devkit GCC 15.2.0 + CMake 4.1.0，证据见验证记录）。
- [x] 退出-2：`release` 预设 configure + build + ctest 全部通过（同上环境）。
- [x] 退出-3：`asan`/`ubsan`/`tsan` 预设已定义并在文档中声明适用工具链（GCC/Clang；
  tsan 仅 Linux）；本机不适用的限制与补跑条件已记录。
- [x] 退出-4：`git status` 不含构建产物；文档相对链接检查通过。（2026-09-21 校验：
  `build/` 经 `.gitignore` 排除，`docs/` 全部相对链接解析有效；首次提交前需再次复核）
- [x] 退出-5：`M0-05` 完成，依赖 pin 与 configure 校验通过（含 commit 漂移负向测试）。

## 验证记录

2026-09-21（工作树初始状态，仓库尚无 commit）：

- 环境：Windows 11 x64（10.0.26200）；w64devkit GCC 15.2.0（`g++ (GCC) 15.2.0`）；
  GNU Make 4.4.1；CMake 4.1.0；ctest 4.1.0。构建类型：MinGW Makefiles 单配置生成器。
- 命令与结果：
  - `cmake --preset debug -G "MinGW Makefiles"`：通过；configure 输出按预期提示
    `dependencies.lock` 缺失并跳过依赖同步（DEC-003 未解锁的预期行为）。
  - `cmake --build --preset debug`：通过，产出 `aki.exe` 与 `aki_skeleton_test.exe`。
  - `ctest --preset debug`：2/2 通过（`skeleton.includes` [unit]，
    `skeleton.app_runs` [smoke]），Total Test time 0.21s。
  - `cmake --preset release -G "MinGW Makefiles"` + `cmake --build --preset release` +
    `ctest --preset release`：2/2 通过。
  - `cmake --preset asan` / `cmake --preset ubsan`：configure 失败，编译器自检链接报
    `cannot find -lasan` / `-lubsan`——w64devkit GCC 不含 sanitizer 运行时，属工具链
    限制而非配置错误。`tsan` 未尝试（同类限制且仅支持 Linux）。
- 限制与补跑条件：asan/ubsan/tsan 证据需在 Linux GCC/Clang（或 MSVC ASAN 变体）上补跑，
  依赖 `M0-06` CI 就绪；负责人 Linductor。
- 遗留：`M0-06`（等待首次 push）、退出-4 复核（提交前）。

2026-09-21（同日第二批次，依赖锁定落地）：

- 输入：Linductor 拉取三个依赖到 `third_party/`（executor @ `74a9419`，EUI-NEO @
  `b9032a8` 即 tag v0.6.0，heyaki @ `e114508`）；许可证核验为 MIT / Apache-2.0 / MIT。
- 变更：`git submodule add` 登记三个 submodule；新增
  `third_party/dependencies.lock.json` 与 `cmake/Dependencies.cmake`（configure 校验）；
  根 `CMakeLists.txt` 移除预留的 `AKI_FETCH_DEPENDENCIES` 开关，改为包含校验模块。
- 命令与结果：
  - `cmake --preset debug -G "MinGW Makefiles"`：输出三条
    `Dependency '<name>' pinned at <commit>`，configure 通过。
  - 负向测试：临时将锁文件中 executor commit 改为全零，configure 如预期 FATAL_ERROR
    （`... is required by third_party/dependencies.lock.json`）；还原后 configure 通过。
  - 回归：debug 构建 + `ctest --preset debug` 2/2 通过。
- 证据与细节：[DEC-003](../decisions/DEC-003-dependency-locking.md)（已 Accepted）。
- 剩余：`M0-06` CI 基线（首次 push 后），完成后 M0 可关闭。
