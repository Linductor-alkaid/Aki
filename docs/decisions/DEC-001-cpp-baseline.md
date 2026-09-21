# DEC-001：C++ 与构建基线

> 状态：Accepted
> 日期：2026-09-21
> 负责人：Linductor
> 冻结里程碑：M0
> 替代/被替代：无

## 背景与问题

Aki 是长期运行的桌面即时通信客户端，涉及网络事件、文件 I/O 与 UI 的跨线程协作，需要
在项目开始前固定语言标准、构建系统、预设矩阵、代码风格与静态检查基线，避免各里程碑
自行其是。

## 决策

依据 AGENTS.md 工程约束与工程规范第 11 节，Aki 采用：

- 语言标准 C++20；构建系统 CMake（≥ 3.25）+ `CMakePresets.json`，提供 `debug`、
  `release`、`asan`、`ubsan`、`tsan` 预设及对应 `ctest` 入口。
- 仓库根放置 `.clang-format` 与 `.clang-tidy`；关键警告可经
  `AKI_WARNINGS_AS_ERRORS` 开启为 error。
- 测试经 CTest 注册并打标签（`unit`、`integration`、`protocol`、`network`、`security`、
  `fuzz`、`performance`、`platform`）；依赖外部环境的用例缺失环境时显式 skip 并记录
  补跑条件。
- sanitizer 预设按 GCC/Clang 旗标定义；Windows MSVC 变体在引入对应工具链时补充，
  未验证的平台不做出跨平台声明。

## 备选方案

- C++17：兼容性更宽，但缺少 designated initializers、`std::stop_token` 等依赖的设施，
  与 AGENTS.md 强制约束冲突，否决。
- 其他构建系统（Bazel/Meson）：与仓库既有规范和工具链约定不一致，否决。

## 影响与风险

- 所有里程碑共享同一预设矩阵，验证证据可直接对比。
- MinGW/GCC 15.2 对 TSAN、部分 sanitizer 的支持有限，相关证据依赖 Linux CI
  （`RISK-2026-003`）。

## 验证方式

M0 中以本机工具链实际执行 configure/build/ctest 并在
[m0-project-skeleton.md](../plans/m0-project-skeleton.md) 记录证据。

## 关联文档和工作项

- [Aki 实施总计划](../plans/aki-implementation-plan.md)（`M0`、`DOD-03`）
- [M0：工程骨架与协作基线](../plans/m0-project-skeleton.md)（`M0-03`、`M0-04`）
