# DEC-002：分层结构与 Application State 状态边界

> 状态：Accepted
> 日期：2026-09-21
> 负责人：Linductor
> 冻结里程碑：M1
> 替代/被替代：无

## 背景与问题

Aki 同时包含网络事件（Heyaki）、应用状态与渲染（EUI-NEO）。若网络回调直接进入 UI，
大文件传输与多设备连接加入后会形成难以控制的跨线程状态修改；Heyaki 接口变化也会扩散
到全部上层代码（设计第 8/10/14 节）。

## 决策

- 分层与依赖方向固定为：EUI-NEO UI -> Application -> Domain -> Heyaki Adapter -> Heyaki；
  Adapter 依赖 Core 抽象，禁止反向依赖。
- Application State 是网络侧与渲染侧之间唯一状态边界：Heyaki 异步事件先进入对应
  Manager，由 Manager 更新 Application State，UI 只消费状态变化；网络线程不直接修改 UI。
- Manager 之间的跨上下文通信按语义使用 `executor::comm` 组件（`LatestMailbox`、
  `DoubleBuffer`、`Topic`、`MpscChannel`），Application State 更新为单写者。
- M1 先以 C++ 抽象接口固定 Heyaki Adapter SPI（设备发现、消息发送、文件传输、连接
  事件），用 FakeHeyakiAdapter 打通领域闭环；真实 Heyaki 在 M3 接入。

## 备选方案

- UI 直接订阅 Heyaki 事件：实现最短，但跨线程状态修改不可控且违反设计第 10 节，否决。
- 以消息总线替代显式 Manager：事件路由灵活但生命周期与取消路径难以观测，与 Executor
  可观测性要求冲突，否决。

## 影响与风险

- 事件 schema 与状态边界需在 M1 早期固定，后续扩展（状态、命令、Agent 消息）复用同一
  模型。
- Manager 职责划分一旦冻结，调整需走决策流程。

## 验证方式

M1 测试矩阵：两台假设备经 FakeHeyakiAdapter 完成“发现 -> 信任 -> 文本消息 -> 断开 ->
重连”闭环；并发路径六项基线测试；ASAN/UBSAN。

## 关联文档和工作项

- [Aki 设计方案](../design/aki_design.md)第 8、10、14 节
- [Aki 实施总计划](../plans/aki-implementation-plan.md)（`RULE-01`、`RULE-02`、`EXEC-02`、`EXEC-03`）
- [M1：领域模型与状态边界](../plans/m1-domain-state.md)
