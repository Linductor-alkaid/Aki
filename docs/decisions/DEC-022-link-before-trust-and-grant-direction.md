# DEC-022：先建链再建立设备信任，并按 grant 签发方显示方向

> 状态：Accepted
> 日期：2026-09-29
> 负责人：Linductor
> 冻结里程碑：M5 维护项 M5-25~M5-28
> 替代/被替代：修订 [DEC-019](DEC-019-directional-trust-and-chat-scopes.md) 与 [DEC-021](DEC-021-bidirectional-trust-and-repair.md) 中的连接事件和信任方向解释；两者其余约束继续有效

## 背景与问题

当前 UI 直到 Heyaki 会话达到 `authenticated` 才显示连接。其实
`pairing_restricted` 已经建立经身份验证的控制连接，只是消息服务仍需要
授权。UI 同时把“本机取得对端签发的 grant”解释为“本机已信任对方”，
而用户要看到的是“对方已信任本机”。

## 决策

- `pairing_restricted` 和 `authenticated` 都是已建链状态。前者按信令路由
  显示 LAN/Relay，后者按实际数据路径显示 LAN/P2P/Relay。两态之间不
  重复发出连接事件；授权完成另行校准信任方向。
- Heyaki 会话快照包含当前 attempts 与历史 closed 记录。Adapter 按设备
  汇总连接时排除 closed 历史；同设备多条当前记录以 authenticated、
  pairing_restricted、握手态的顺序选择代表。存在当前建链则保持连接，
  仅当所有当前记录均未建链/缺失时发出断开；历史记录不触发配对或授权。
- 设备在线是发起 `connect_lan` 的前提，不要求先有 trust grant。已建链
  不意味着可发送消息：pinned Heyaki `Node::send_message` 仍只接受
  `authenticated` 会话，发送失败须按既有失败路径向用户展示。
- 已在线而未建链的设备无论信任状态如何都提供「连接」入口；待确认设备
  只有在连接路径已建立时才开放「验证密码」。UI 队列拒绝时保留弹窗并
  提示失败；Adapter 拒绝或异步配对失败均进入设备行失败标志。队列接纳
  不能作为密码验证结果。NodeSession 在提交前核对受限会话快照，避免
  向已知缺失/非受限会话提交；余下竞态见 HEY-20260930-001。
- 本机输入对端口令并取得**对端签发给本机**的 grant，表示“对方已信任
  本机”；本机签发给对端的 grant 表示“本机已信任对方”。现有持久化
  字段不改格式：`TrustState::Trusted` 对应前者，`inbound_trust` 对应后
  者。字段名沿用历史“入站授权”含义，UI 不直接展示字段名。
- 设置页密码弹窗在主窗口根层渲染，其 `.screen()` 尺寸与坐标原点同属
  整个窗口，滚动设置页不改变弹窗中心。
- 重连对账以已进入配对/信任轮（Pending/Trusted）、LAN 目录可见且尚未
  建链为条件；LAN 发现可能把 Pending 设备先标为 Online，presence 不能
  作为重连排除条件。Unknown 与 Rejected/Revoked 不自动发起新配对轮。

## 备选方案

- 将受限会话显示为离线：混淆连接和授权，无法验证建链阶段。
- 在受限会话强行开放消息：与 pinned Heyaki 的授权门控冲突。
- 直接重命名持久化字段：需要额外数据库迁移，且不改变 grant 语义。

## 影响与风险

用户可以在受限会话打开 Conversation，但 Heyaki 会拒绝尚未授权的发送。
双方均在线仍取决于局域网可达性、监听端口和防火墙；在线发现本身不保
证 TCP 建链。对端无密码直接批准尚缺 Heyaki API，详见
[HEY-20260929-001](../heyaki_feedback/ledger.md)。真实双端消息与文件
验证由 M5-25~27 验收记录承载。

## 验证方式

单测覆盖 restricted 初连、restricted→authenticated 不重连、授权后方向
校准、断连及四态文案；两台同版设备实际验证先连接、分别输入口令、收发
文本与文件、密码弹窗居中。

## 关联文档和工作项

- [Aki 设计](../design/aki_design.md)第 4、8.1 节
- [Aki UI 设计规范](../design/aki_ui_design.md)第 3、4 节
- [M5 里程碑](../plans/m5-eui-neo-ui-mvp.md) `M5-25`~`M5-28`
