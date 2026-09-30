# Heyaki 能力反馈台账

> 状态：Active
> 更新日期：2026-09-30

## HEY-20260929-001：受限会话由接收方直接批准

- **状态**：待上游能力设计；按用户 2026-09-30 指示独立管理 Heyaki，
  已提交 [Heyaki #2](https://github.com/Linductor-alkaid/heyaki/issues/2)，
  未修改 pinned 依赖。当前先验证 Aki 连接与密码授权。
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
  已提交 [Heyaki #1](https://github.com/Linductor-alkaid/heyaki/issues/1)。
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

## HEY-20260930-002：非对称授权状态下接收端静默忽略密码请求

- **状态**：双机实际复现，已提交
  [Heyaki #3](https://github.com/Linductor-alkaid/heyaki/issues/3)；按用户要求
  独立管理上游，未修改 pinned 依赖。
- **版本与条件**：Heyaki `e114508ab32d496d52e9db9bac26eb1cc88c4ae7`；
  Aki CI run `36659102913` / head `ef23c78`，七项 job 全绿。目标机
  手动安装的 deb SHA-256 为
  `05c6dfd24f1d9de7da89d902d1fbe94e288685ac2263a34b480a7a113c783fa8`，
  安装二进制与 deb 内容一致，`dpkg -V aki` 无差异。
- **可复现证据**：本机 ProfileStore 有一条有效 received grant；目标机
  对应 issued grant 已撤销。目标机 UI 显示本机在线、LAN、待确认。
  输入本机口令后弹窗关闭，授权与失败状态都无变化。调试器在本机周期
  维护断点只读取状态及计数：Node `authenticated`、PeerSession `active`；
  `pairing_requests_received=1`、`pairing_results_sent=0`（sent request 和
  received result 均为 0）。因此请求确已抵达本机，未产生结果。
- **确定路径**：`PeerSession::handle_pairing_request` 在已 authenticated
  时仅计数、notify 后返回，不发送 pairing_result。区别于
  HEY-20260930-001 的发送端 strand 校验拒绝，本项是接收端静默忽略。
  缺少 TCP 套接字不能用于推断断线：数据会话可运行在 UDP/WebRTC 上。
- **影响及期望**：既有单向授权/撤销造成两端状态不一致时，受限端可以
  提交密码请求，已授权端却不回应。需要明确反向授权与修复语义，并为
  已认证状态下的合法请求返回成功或明确拒绝；超时、取消、断连及关闭
  均有一次可观察终态，不得永久静默等待。
- **Aki 处理与验收**：当前不能以队列接纳声明密码验证成功；M5-26
  双机授权验收保持未完成。负责人 Linductor；上游补非对称授权、单向
  授权后反向申请、撤销后重配、错误口令、重复请求及超时测试，修复公开
  契约并固定依赖后在 Aki 双机复验。独立测试配置的首次配对只能证明
  初次配对路径，不能替代本项修复验收。
