# DEC-007：单元测试框架采用 Catch2 v3

> 状态：Accepted
> 日期：2026-09-21
> 负责人：Linductor
> 冻结里程碑：M1 开始前（本记录即冻结）
> 替代/被替代：无

## 背景与问题

M0 的测试为无框架 smoke（`tests/test_skeleton.cpp`）。M1 起需要覆盖状态机非法转移、
终态幂等、迟到事件不复活等断言密集的单测，以及后续里程碑的并发基线六项测试
（正常完成、任务异常、提交拒绝、执行中取消、超时、shutdown）。需要固定测试框架、
引入方式与测试组织约定。

## 决策

- 单元/集成测试框架采用 [Catch2](https://github.com/catchorg/Catch2/) v3
  （当前 pin `v3.9.1`，commit `644821ce28cb25d7992a4d0375b1d83214392592`）。
- 引入方式：CMake `FetchContent`，`GIT_TAG` 锁定完整 commit hash；关闭其 docs/extras
  安装。版本升级 = 修改 `tests/CMakeLists.txt` 中的 `AKI_CATCH2_COMMIT` 并在本记录
  追加变更说明，不作为 submodule 纳入 `third_party/dependencies.lock.json`（测试
  依赖不进入发布产物）。
- 测试以标签组织：`unit` / `integration` / `smoke`；CI 按标签选择执行集。CI 基线
  随 M0 收尾（首次 push）建立，见总计划 `RISK-2026-003`。
- 第三方依赖编译不继承第一方警告级别（`aki_apply_warnings` 只作用于 Aki 目标），
  避免上游告警被 `-Werror` 放大。

## 备选方案

- GoogleTest：生态最广，但需要额外 mock/断言配置，对纯值域单测无明显优势；宏风格
  与 C++20 值语义代码契合度一般，暂不采用。
- doctest：编译更快，但集成测试对 matcher/fixture 的表达力弱于 Catch2 v3 的
  SECTION 模型；M1 断言密度高，优先表达力。
- 自建最小断言头：无依赖成本最低，但无法覆盖后续并发故障注入场景的表达需求，否决。

## 影响与风险

- FetchContent 在 configure 阶段需要网络（或预热好的 build cache）；CI 与离线环境
  需要保证可拉取 GitHub。若后续离线成为常态，可改为 submodule + 锁文件（升级本记录）。
- Catch2 v3 编译产物较大、首次配置构建耗时；对开发迭代影响集中在冷构建，可接受。

## 验证方式

`cmake --preset debug && cmake --build --preset debug && ctest --preset debug`
在 Windows/MinGW GCC 15.2 下通过；单元测试以 `unit` 标签可独立执行。

## 关联文档和工作项

- [Aki 实施总计划](../plans/aki-implementation-plan.md)（`DEC-007`、`DOD-02`）
- [M1：领域模型与状态边界](../plans/m1-domain-state.md)（`M1-07`）
