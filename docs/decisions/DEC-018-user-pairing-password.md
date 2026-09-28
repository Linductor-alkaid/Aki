# DEC-018：用户设置本机配对口令并主动认证设备

> 状态：Accepted
> 日期：2026-09-28
> 负责人：Linductor
> 冻结里程碑：M5 维护项 `M5-11`
> 替代/被替代：替代 [DEC-016](DEC-016-pairing-password-verifier.md) 的固定口令方案

## 背景与问题

固定的公开口令允许局域网中的其他 Aki 实例通过目标端密码校验。首次启动没有用户
设置入口；发现设备停留在 `Unknown`，没有主动建链动作，因此也无法进入配对所需的
`pairing_restricted` 会话。

## 决策

- 新 profile 启动前由 GUI 要求用户设置至少 8 个 Unicode 标量的本机配对口令。
  明文只用于生成 Heyaki Argon2id verifier，profile 持久化 verifier，不另存明文。
  未设置时不启动网络节点。旧版固定口令或无效 verifier 的 profile 在升级后首次
  启动时也要求设置新口令，保留身份和业务数据。现有 profile 可在 Settings
  更换本机口令，
  使用 `set_password_verifier` 和递增的 `password_generation`，不删除数据。
- 已发现的 `Unknown` 设备提供“连接并认证”入口。用户触发 `connect_lan`，
  观察到 `pairing_restricted` 后按合法边推进 `Unknown → Pending`。确认弹窗展示
  指纹并要求输入**对端设备的本机配对口令**；提交给 `pair_peer`，结果再推进
  成功时 `Pending → Trusted`。错误口令保持 `Pending` 并通过易失快照提示重试；
  `Rejected` 仅代表用户明确拒绝。提交失败不乐观改变信任状态。
- 对端先发起连接时，本机即使没有开启扫描，也从受限会话的 LAN endpoint
  取得公钥并建立 `Pending` 设备行，提供同一确认入口。
- 口令经 UI → Application → Heyaki Adapter 显式传递，不写入事件、快照、日志
  或持久化业务库。输入组件只渲染掩码，不把明文放进 UI 节点。弹窗关闭、提交后
  清空草稿。

## 备选方案

- 继续使用固定口令：任何安装都共享已公开凭据，且无法表达目标端授权。
- 发现时自动建链：可能对每个未信任端点发起网络连接；改为用户主动触发。
- 用明文配置文件保存口令：会扩大泄露面，Heyaki verifier 已满足验证需要。

## 影响与风险

现有固定口令 profile 保留身份并在升级后要求更换口令；无法从 verifier
恢复旧口令。口令变更不自动撤销现有信任 grant，撤销仍须用户显式操作。
真实双端认证需 LAN 入站环境验证，未实测不得标记完成。

## 验证方式

验证首次设置、跨重启 verifier、错误口令、旧 profile 升级、Unknown → Pending
入口、配对成功/失败及提交拒绝；在 Linux/Windows 双端执行实际配对。

## 关联文档和工作项

- [Aki 设计方案](../design/aki_design.md)第 4、8.1 节
- [Aki UI 设计规范](../design/aki_ui_design.md)第 3、4 节
- [M5 里程碑](../plans/m5-eui-neo-ui-mvp.md) `M5-11`
