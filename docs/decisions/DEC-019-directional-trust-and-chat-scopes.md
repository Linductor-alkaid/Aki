# DEC-019：连接与单向信任分离

> 状态：Accepted
> 修订：连接事件与信任方向解释由 [DEC-022](DEC-022-link-before-trust-and-grant-direction.md) 更新
> 日期：2026-09-29
> 负责人：Linductor
> 冻结里程碑：M5-12
> 替代/被替代：修订 [DEC-018](DEC-018-user-pairing-password.md) 的双侧自动 Trusted 推进

## 背景与问题

双端实测发现：一侧输入另一侧口令后，两侧都显示 Trusted。原因是本机看到
`authenticated` 会话时也执行 `Pending → Trusted`。Heyaki TrustGrant 有
issuer/subject 方向；会话建立只表示某个方向的授权已足够建立通信，不能推出
本机也已获得对端签发的 grant。

同时接收端 profile 的默认 scope 只有 `message.send`，发送端申请的
`file.push:inbox` 会在配对策略交集处丢失，使图片和文件传输被拒。

## 决策

- `ConnectionPath`、presence 和会话状态表示连接事实；`TrustState` 表示本机
  对该设备主动配对并取得对端签发 grant 的结果。连接事件只更新连接事实，
  `PairingCompleted(success)` 才能让本机 `Pending → Trusted`。对端输入本机口令
  不自动改变本机 TrustState。
- Aki 聊天与文件能力在授权会话中开放；新 profile 的 Heyaki 配对策略默认
  scope 为 `message.send` 与 `file.push:inbox`。旧 profile 启动时幂等增加缺失
  的文件 scope 并提高 policy generation；不改变密码 verifier 和既有 grant。
  已签发的旧 grant 不会自动扩大权限，用户须重新授权相关设备。
- 会话入口以当前连接及可用的聊天权限门控，不以双向 `Trusted` 为必要条件。
  单向授权与会话是否可通信分别显示。

## 备选方案

- 继续在连接事件中推进 Trusted：把对端获得的授权误表示为本机获得授权。
- 绕过 Heyaki scope 判定发送文件：破坏底层权限边界。

## 影响与风险

旧数据中由此前双侧自动推进得到的 Trusted 行无法在 Aki 数据库中辨别来源；
后续恢复须以 Heyaki 本地 grant 为权威校准。旧 grant 的 scope 不自动升级，
重新配对前文件发送可能继续失败。真实双端验证仍为验收门槛。

## 验证方式

单测覆盖单向成功/对端会话授权、默认和迁移策略 scope、入站时间及传输
admission；Linux/Windows 双端分别验证两个授权方向与图片/文件传输。

## 关联文档和工作项

- [Aki 设计](../design/aki_design.md)第 4、6、8.1 节
- [M5-12~M5-15](../plans/m5-eui-neo-ui-mvp.md)
