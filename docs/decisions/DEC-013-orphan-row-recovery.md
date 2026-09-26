# DEC-013：孤儿活动传输行的重启处置语义（降级 Paused 待显式再驱动）

> 状态：Accepted
> 日期：2026-09-26
> 负责人：Linductor
> 冻结里程碑：M4（`M4-06` 开工首步冻结；本记录即冻结，调研依据见文末）
> 替代/被替代：无（对 [DEC-012](DEC-012-receive-merge-bearing.md) 风险登记的
> 「孤儿活动行的重启再驱动未设计」定稿；对设计 §7 状态边作显式扩展）

## 背景与问题

孤儿活动行**不是崩溃边角而是受控关闭的常态产物**：关闭钩子第一步即
`transfers.request_cancel_all()`，而其处理只置 closing + io cancel + 清会话
表、不写任何终态——正常退出的在途传输行就是孤行。无预期态基线则 M4-06 的
重启一致性断言（退出-1）不可写。三选一：A 自动恢复（push_file same-id 断点
续传）/ B 降级 Paused 待显式再驱动 / C 判 Failed。

## 决策

**定稿为 B（降级 Paused 待用户/显式再驱动）**：

1. **恢复段改写**：启动恢复段（主线程、播种前、同步落库、不经 blocking
   worker 与 owner 状态机，§11.1② 纪律）把全部**非终态** TRANSFER 行改写为
   `Paused`（改写先于 `sweep_tmp_orphans`——Paused 非终态故 `.part` 保留，
   清扫判定不变）；恢复诊断新增改写计数（可见）。
2. **恢复为显式动作且按方向分化**：
   - **接收行**（无发送会话形态）：恢复 = 行自 Paused 经 wire 进度事件推进
     `Transferring`（合法边）后自然收敛；committed-崩溃窗口行由对端/宿主重发
     `CompleteTransfer(Completed)` 幂等收敛（供源=接收根文件，DEC-012①）。
     为使重启后的 wire 事件能走「已知的 Paused 行」路径，恢复段将改写后的
     非终态行**播种进 TransferManager 已知行缓存**（构造选项注入——运行期
     语义与 DEC-012④ 路由一致）。
   - **发送行**：M4-06 范围内只能**显式 Cancelled 终结**——完整 push_file
     same-id 断点续传需 `source_path` 持久化（SPI 参数不入库，信息外泄防线，
     transfer_types 头注）且 Aki 归档 hash-first 无法中途续（流式 SHA-256 需
     全前缀），登记为 M5+ 前提。无会话行的 `cancel_transfer` 补「直接终态
     写入」路径（`CompleteTransfer(Cancelled)` 交 owner 状态机校验——行缺失
     /已终态为可见拒绝，discard 作业经既有接受后处理器入队）。
3. **状态机边扩展**（设计先行，`transfer_types.hpp` 同批回填）：
   `Queued → Paused` 与 `Negotiating → Paused` 新增为合法边（重启降级语义；
   运行期语义与恢复段改写一致）。其余边不变。
4. **重启后首事件毛刺（风险②定案）**：择「恢复段播种 known_rows_」——
   Paused 行收到 wire 进度事件即合法推进（无拒绝毛刺）；`started(Negotiating)`
   对 Paused 行仍为可见拒绝（owner 状态机，一次性、计入断言基线）。
5. **不做自动恢复（否决 A）**：发送侧续传需 source_path 持久化 + hash 无法
   中途续，语义退化为整体重跑；committed-窗口会被自动重推恶化成整体重传
   （正确收敛是幂等重发 Completed）；M5 前无 UI 无可控面。**不判 Failed
   （否决 C）**：对抗 heyaki 会话 attach 自动 re-manifest（迟到 transferring/
   committed 被终态拒绝 → 接收根文件永久无法收敛）、触发 `.part` discard
   主动销毁可收敛进度、与「断线恢复」产品目标相悖。**不维持现状（否决
   折中/不冻结）**：孤行是常态产物，无基线则退出-1 断言不可写。
6. **边界不变项**：接收根残留仍不扩清扫面（M5 GC 议题，DEC-012③，断言
   基线写明「保留不动」）；多对端同 id 沿用 TransferId 全局唯一假设；无新
   并发原语（改写在恢复段主线程、恢复动作经既有 TM 泵，DEC-009 容量复核
   不触发）。

## 备选方案

（全文见冻结调研记录；摘要见决策 5——A/C 及两个折中的否决理由。）

## 影响与风险

- **实现落点**：`transfer_types.hpp` 状态边 + `startup_recovery`（加载后、
  清扫前改写 + 诊断计数）+ `TransferManagerOptions` 播种行 + 无会话
  cancel 直接终态写入 + 组合根传播种行。恢复段改写不经 owner 状态机
  （直接仓储同步 upsert）——设计声明该边使运行期语义一致（AGENTS「新增
  转换先更新设计文档」）。
- **实现时必须验证**：改写次序（先于清扫）；播种行经 progress 事件推进
  Paused→Transferring 的合法链；committed-窗口收敛链（Paused→显式恢复/
  重发 Completed→供源=接收根→Completed+stored_* 回写）；无会话 cancel 的
  可见拒绝形态（行缺失）；`started(Negotiating)` 对 Paused 行的一次性可见
  拒绝计入断言基线；push_file same-id 对已 committed 接收方的重传语义与
  heyaki 簿记引用未在公开头核实——回环补跑观察，异常即登记。
- 本调研为纯静态证据（未编译未运行）；动态验证归 M4-06。

## 验证方式

本记录依据 2026-09-26 冻结调研（负责人 Linductor；pinned heyaki
`file.hpp`/`node.hpp`/file_service 与 aki 仓库 transfer_manager/
app_state_owner/file_store/startup_recovery/main.cpp 逐文件静态核实）。
动态验证归 M4-06：网络无关单测（改写语义/清扫次序/播种推进/无会话 cancel/
全链路组合含对账断言）+ 全链路回环二进制（[skip] 降级纪律）+ debug/release
全量 ctest 零回归。证据记录于 [M4 里程碑文档](../plans/m4-image-file-transfer.md)
M4-06 验证记录。

## 关联文档和工作项

- [Aki 设计方案](../design/aki_design.md)§7（状态边扩展）、§7.1⑥（重启处置
  小节）、§11.1②（恢复段增补）、§11.1④（恢复路径）
- [DEC-010](DEC-010-image-message-contract.md)（运行期零传导）、
  [DEC-011](DEC-011-transfer-io-bearing.md)（发送侧承载/source_path 防外泄）、
  [DEC-012](DEC-012-receive-merge-bearing.md)（供源回退/幂等收敛/风险移交）
- [Aki 实施总计划](../plans/aki-implementation-plan.md)（`SCOPE-08/09`、
  `RULE-08`、M4）
- [M4：图片消息与文件传输](../plans/m4-image-file-transfer.md)（`M4-06` 实现
  与退出-1 证据）
