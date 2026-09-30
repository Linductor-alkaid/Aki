# Heyaki 能力反馈台账

> 状态：Active
> 更新日期：2026-09-30

## HEY-20260929-001：受限会话由接收方直接批准

- **状态**：待上游能力设计；未修改 pinned 依赖，未向上游发送消息。
- **用户需求**：A 发起连接后，B 在自己的设备上点击“允许”即可签发给 A
  的授权，无需 A 输入 B 的密码，也无需 B 输入 A 的密码。
- **当前证据**：`third_party/heyaki/include/heyaki/node.hpp` 公开的配对入口
  只有 `Node::pair_peer(peer, password, requested_scopes)`；回调
  `set_pairing_observer` 只报告尝试结果，未提供接收方批准待处理请求的 API。
  `Node::send_message` 仅在 `authenticated` 会话上接受发送，不能用普通
  Aki 消息在 `pairing_restricted` 会话中实现批准。
- **影响**：当前 Aki 可先连接并显示受限会话，但建立消息授权只能由一侧
  输入另一侧的设备密码。把现有“确认”按钮当作无密码“允许”会误导用户。
- **期望语义**：受限会话给接收端一个有稳定请求 ID、发起方身份和公钥指纹、
  有界超时的待批准请求；接收端可明确批准/拒绝，批准后按选定 scope 签发
  grant，双方收到可观测结果；迟到、重复、取消和断连结果幂等且不可复活。
- **最小能力建议**：Heyaki 公开 pending request 观察与
  `approve_pairing(request_id, scopes)` / `reject_pairing(request_id)`，并
  将批准绑定到已验证身份、会话与 nonce；拒绝/超时有诊断事件。
- **Aki 侧处理**：当前 UI 明确称“验证密码”，不宣称提供“对方直接允许”。
  不在 Aki 自建未授权控制信道，也不修改 `third_party/heyaki`。
- **负责人及补跑条件**：Linductor；Heyaki 上游提供并固定公开 API 后，
  在 Adapter 接入并补双设备无密码批准、拒绝、超时与重放测试。

## HEY-20260930-001：配对派发后的校验失败未回传

- **状态**：已核对 pinned API 与实现；Aki 增加快照前置校验，未修改依赖。
- **可复现证据**：`Node::pair_peer` 成功 post 到 strand 后返回成功，
  派发 lambda 丢弃 `pair_peer_strand` 的结果。会话缺失、非受限或已存在
  pending 请求时，该方法失败而不建立 pending，也不调用 pairing observer。
  Aki 双机首轮出现密码提交后无可见结果；相关实现位于
  `third_party/heyaki/src/client/node.cpp` 的两个配对入口。
- **影响**：调用方无法区分业务接纳与单纯派发；会话状态竞态下可能永久
  等待结果。已授权会话也无法直接通过该入口申请反向 grant 或续期。
- **期望语义和最小能力**：每个已接纳的配对操作携带稳定请求 ID，并在
  成功、strand 校验拒绝、断连、超时和关闭时产生一次终态结果；公开结果
  必须区分派发与协议接纳。明确已授权会话如何申请反向 grant 和续期。
- **临时边界**：`NodeSession::pair_peer` 用公开会话快照先拒绝已知无效
  请求，`DeviceManager` 将 Adapter 拒绝映射到失败状态；快照与 strand
  之间仍有竞态，不能宣称此临时边界覆盖全部操作完成语义。
  UI 移除会向已授权会话再次调用 `pair_peer` 的“更新文件授权”入口。
- **负责人及补跑条件**：Linductor；上游修复公开完成契约并固定依赖后，
  补会话校验竞态、重复请求、取消、超时、关闭和反向 grant 验证。
