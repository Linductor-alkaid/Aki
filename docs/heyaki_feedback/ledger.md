# Heyaki 能力反馈台账

> 状态：Active
> 更新日期：2026-09-29

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
