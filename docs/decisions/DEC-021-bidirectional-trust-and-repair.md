# DEC-021：双向信任四态、终态重建与连接事实通信

> 状态：Accepted
> 修订：连接事件与信任方向解释由 [DEC-022](DEC-022-link-before-trust-and-grant-direction.md) 更新
> 日期：2026-09-29
> 负责人：Linductor
> 前置：[DEC-019](DEC-019-directional-trust-and-chat-scopes.md)、
> [DEC-020](DEC-020-signed-lan-device-name.md)
> 工作项：M5-18~M5-24

## 背景与问题

第二轮双端实测发现：

1. 信任是单向语义但 UI 只显示单值状态：本机信任对方、对方信任本机、
   互相信任、互不信任四种关系无法表达；对端撤销本机信任后本机状态
   永不更新。
2. 被拒绝/撤销的设备成为死端：会话列表按信任状态禁用入口（与新建
   会话弹窗、聊天窗口行为互相矛盾），终态设备无任何重新建链/重建信任
   入口。
3. 断线重连循环 30s 预算耗尽后静默死亡且无 re-arm；对端恢复上线后
   双方互等，presence 永久停留在离线（「连接断开，等待恢复」不消失）。

## 决策

- **四态信任显示**：`DeviceIdentity.inbound_trust`（bool，`device` 表新
  列）记录本机已向对端签发有效 Heyaki grant（对端信任本机）；既有
  `TrustState` 表达本机→对端方向。UI 按二元组合显示「互相信任 / 本机已
  信任对方 / 对方已信任本机 / 未建立信任」，配对流程态（Pending/
  Rejected/Revoked）优先显示。数据源为 pinned heyaki `Node::
  trust_grants_for`（本机 TrustStore 权威，issued/received 分向），经
  `NodeSession::trust_grants` 封装与 SPI `trust_directions` 暴露；启动、
  会话（重）连接时逐设备校准，撤销时本机即时归零。
- **远端撤销的可观测降级**：协议无撤销推送（签发方 TrustStore 为最终
  裁定），已信任会话被裁定落入 `pairing_restricted`（双向有效 grant 均
  不存在）时，本机信任降级 `Trusted -> Revoked` 并归零对向信任。限制：
  降级只在会话重建裁定处观测；对端在其侧看到的「本机信任对方」同样
  只能在下次会话裁定后更新——协议固有的非对称，在 UI 中如实显示。
- **连接事实门控通信**：会话列表入口按连接/会话事实（连接路径非
  Unknown 或会话 Active）门控；Rejected/Revoked 仅显示 destructive
  徽标，不裁剪点击面。发送路径本就无信任门控（DEC-019），本次消除
  最后一处 UI 残留。
- **终态重建入口**：状态机新增显式边 `Rejected/Revoked -> Pending`
  （仅用户「重新配对」动作触发；自动流程仍不得离开终态）。设备列表
  对终态设备显示「重新配对」按钮（`begin_pairing` 同一入口），
  pairing_restricted 事件把终态行放回 `Pending` 走正常配对流。
- **周期重连对账**：组合根 5s 周期任务（Executor 周期句柄，关闭钩子
  ① 取消）对「离线 + LAN 目录重新可见 + 未认证」的已知设备重启单飞
  重连循环（`ReconnectCoordinator::start` 单飞安全，消费已终结循环后
  可重启）；恢复仍由 pipeline connected diff 事件链承载，不新增恢复
  回调。
- **文件选取防御**：Linux 对话框实现（zenity/kdialog，third_party
  EUI-NEO 平台层）将 stderr 合并进输出，Aki 侧对选取结果按存在性过滤，
  杜绝把诊断行当路径导致的 stat ENOENT。

## 备选方案

- 拆分撤销为「仅收回对端授权 / 仅声明本机不信任」两个操作：更精确但
  与 Heyaki 撤销模型（按 grant 逐条）不匹配，留待底层能力演进。
- 对端撤销即时推送：需新增 wire 帧，扩大 pinned 依赖边界，不接受。
- LAN 发现放开 trusted 条目合成 presence：与发现面职责冲突，周期对账
  是单点更小修复。

## 影响与风险

- `inbound_trust` 为本机视角缓存：对端单方向撤销后，本机 issued 记录
  仍有效（签发方权威在本机），显示可能滞后于对端意图，直到会话裁定
  或对端显式动作；UI 不宣称实时性。
- 降级启发依赖 restricted 裁定语义（双向均无效），grant 过期同样触发，
  语义为「授权关系已不存在」。
- 24.04 Wayland 会话任务栏图标根因是窗口 app_id/WM_CLASS 未设置
  （GLFW Wayland 后端 `glfwSetWindowIcon` 为空实现）。经决策不修改
  pinned EUI-NEO：按
  [EUI-NEO 反馈台账](../eui_neo_feedback/ledger.md)登记
  EUI-20260929-002 并已报上游
  ([#77](https://github.com/sudoevolve/EUI-NEO/issues/77))，随 pinned
  版本升级跟进；本批落 `StartupWMClass=aki`（X11 会话关联）与文件
  选取的存在性过滤（EUI-20260929-001，
  [#76](https://github.com/sudoevolve/EUI-NEO/issues/76)）。
  2026-09-30 用户复现后，Aki 窗口身份兼容适配由
  [DEC-024](DEC-024-linux-desktop-window-identity.md) 补充；上游缺口及
  不修改 pinned 依赖的约束保留，旧的“仅等待升级”处置不再适用。

## 验证方式

单测覆盖：四态派生键、终态重建边（状态机 + PairingReady 终态回
Pending + Trusted 降级）、inbound_trust 字段级更新与迁移、撤销归零、
选取结果存在性过滤；集成覆盖断线重连对账 re-arm。双端真机复验四态
显示、撤销双向感知、掉线恢复与重配对全流程。
