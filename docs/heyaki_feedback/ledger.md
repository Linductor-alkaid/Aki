# Heyaki 能力反馈台账

> 状态：Active
> 更新日期：2026-10-07

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

## HEY-20260930-003：基础消息/文件与设备控制信任解耦

- **状态**：公开 API 与通道/服务门控已核对，待上游提供 opt-in 策略；
  未修改 pinned 依赖。已提交
  [Heyaki #4](https://github.com/Linductor-alkaid/heyaki/issues/4)，关联 M5-30、DEC-023。
- **版本与证据**：Heyaki `e114508ab32d496d52e9db9bac26eb1cc88c4ae7`。
  `NodePeerSessionState::pairing_restricted` 仅允许配对帧；NodeConfig 无免
  grant 的基础通信策略，PairingPolicy 的 default_scopes 用于配对 grant。
  PeerSession 的 open/adopt_business_channel、Node 消息发送及服务安装要求
  authenticated；MessageService 检查 message.send，FileService 检查
  file.push:<root>。不属于 Executor 能力缺口。
- **用户目标**：完成密码学身份验证且已连接后可收发文本/图片和指定 inbox
  文件，不建立持久化设备信任；shell 等设备控制继续要求独立权限。
- **最小能力**：默认兼容的 opt-in 应用策略，双端通道创建/接纳及业务帧
  统一强制执行，区分策略能力与 grant scope。指定接收根、路径、配额、
  容量与速率限制保留，不顺带开放远程目录读取/控制 RPC/shell/gateway。
- **影响与临时处理**：当前 Aki 即使显示已连接也可能被 Heyaki 拒绝消息/
  文件；仅改按钮不能达成需求，不以自动签发宽 scope grant 代替。上游
  缺口未解决前，免信任通信仍未实现，失败沿既有状态路径可见。
- **负责人及验收**：Linductor；Heyaki 独立实现并固定公开策略 API 后接入，
  补无 grant 双端消息/图片/inbox 文件、拒绝策略、身份/路径攻击、配额/
  背压、断连、取消与 shutdown；基础通道不得打开高级控制能力。

## HEY-20260930-004：关闭会话时回调重置竞争

- **状态**：TSAN 实际复现，已提交
  [Heyaki #5](https://github.com/Linductor-alkaid/heyaki/issues/5)，未修改依赖。
- **版本与复现**：Heyaki `e114508`，libdatachannel v0.23.2 @ `9e6a13a`。
  Aki run `36671067469`、head `580d989`、TSAN job `109745963853`；
  `test_discovery_pairing_loopback` 53 个断言通过、双端 authenticated，
  关闭时 TSAN 报同地址两次 2 字节写，全量 42/43，故此轮 CI 失败。
- **竞争路径**：T20 从 PeerConnection::closeDataChannels 经 DataChannel::close
  调用 Channel::resetCallbacks；T29 从 Heyaki transport Channel::close 调用
  rtc::Channel::resetCallbacks。共同顶帧为 utils.hpp:105 的
  `rtc::synchronized_stored_callback<>::operator=(const&)`。依据实现推断：
  派生类隐式赋值在基类赋值的锁释放后复制 mutable optional stored，导致
  同一无参数回调对象并发重置。需上游核对公开 close/reset 的调用纪律。
- **影响与最小修复**：会话 fail/关闭时存在真实数据竞争；核对 Heyaki 显式
  reset 与 libdatachannel 内部 close 的责任，避免并发重置或上游补齐赋值
  同步。不能由 Aki 创建新线程/队列或业务 mutex 代替传输内部同步。
- **临时边界**：沿现有 cmake/tsan-suppressions.supp 的 vendor-only 纪律，
  仅登记 `rtc::synchronized_stored_callback<>::operator=`，不豁免 Aki 或
  Heyaki 的其他路径、不关闭测试/插桩。此豁免不表示竞态已修复。
- **负责人及移除条件**：Linductor；上游补并发 close、远端 close、fail、
  取消、超时和 Node shutdown 的 TSAN 回归，升级 pinned 后移除精确条目。

## 2026-10-01 上游修复与 Aki 接入进度

Heyaki #1~5 均 closed/completed，官方 v1.1.1 release 的 pin 为
1ceb42c7b244e950ebeeb10edc84b6d83e423626。依次修复有界密码 admission
(9626ed0)、receiver approval (0c317c7)、已授权会话回答密码请求
(5e191ef)、basic communication (0440a03)、回调 copy/move 同步
(8f4843e)。这些记录的上游阻塞已解除，旧复现证据仍保留。
Aki 接入归 M5-33~35，尚不标为 Aki 双设备验收 Completed；M5-35 批准
界面仍待接线，M5-25/26 需同版双设备密码与反向授权复验。

HEY-20260930-001 的快照临时校验已移除，使用发布 API 的有界 admission；
HEY-20260930-004 的精确 TSAN 条目已移除，等待 Aki 最新 head TSAN CI。
HEY-20260930-003 显式策略已在 Host 接入，零 grant 本地回环通过，
Linux/Windows 安装版尚待验收，不能以升级/上游关闭替代。

## HEY-20261001-001：单端基础策略拒绝文件时缺少有界终态

- **状态**：Resolved；已提交 [Heyaki #13](https://github.com/Linductor-alkaid/heyaki/issues/13)，关联 M5-34。
- **版本与复现**：v1.1.1 @ 1ceb42c，Ubuntu 24.04 / GCC 13；新身份零 grant、
  单 Executor 两 borrowed Runtime，两端配置 inbox，仅 A basic=true。
  push_file 返回成功，3 秒内无失败/取消终态，B 无落盘、无 committed；
  Debug 输出 file admitted=1 terminal-before-cancel=0，随后显式取消收到
  cancelled（109 assertions / 3 cases）。ASAN 下同样无终态，但取消返回
  false；该失败已保留，不能声称取消必定可用。上游 basic 测试明确承认
  strict 端拒绝 push 没有 TTL 有界终态，停在 unaccepted 状态。
- **影响与期望**：同版双方 opt-in 的文件正常；严格策略/旧版接收端可能
  让 Aki 长期显示等待。需传回明确拒绝，或公开、可配置的协商 deadline，
  每个 TransferId 一次终态，断连/取消/shutdown 与迟到结果幂等。
- **Aki 边界**：不直接修改上游，不自动签发 grant；回归仅验证无落盘、
  无 committed、无 grant、已有取消 admission 与 shutdown，未将拒绝终态
  标完成。M5-34 的单端文件拒绝验收保持未完成。负责人 Linductor；
  补跑条件为上游固定修复后双端策略矩阵与取消竞争回归。

## 2026-10-02：HEY-20261001-001 上游修复消费

Heyaki #13 已 closed/completed；#14 已合入 516815c，新参数
file_offer_timeout（0=默认30s）让未接收 offer 产生一次失败终态，接收前
不调度正文分块读取/发送；源文件元数据探测及校验不在此保证内。Aki 在
M5-40 单独固定此提交并适配；新独立 offer Debug/ASAN 各 86 assertions /
2 cases 通过，完整 Debug/ASAN/UBSAN 回环通过。PR #65 精确 head ae08acef
的 CI run 36895691566 七项全绿，已合入 084c39c；此原始 offer 有界终态
缺口关闭。两端安装版及后续会话保持分别仍归 M5-34/37~39 与 Heyaki #15。

首轮 ASAN 的消息拒绝→同会话文件 push 没有 failed，进一步观测到
paused=1 / linked=0，是上游明确保留的“拒绝后后续 push 破坏会话”边界。
旧复现永久 offered 的缺口和此断连停车不同；原测试严格要求 failed 或
可观察的 Paused+断连。上游修复不等于后续会话保持已验收，M5-34 仍未完成。

## HEY-20261002-001：策略拒绝后再次推送破坏会话

- 状态 Reported：[Heyaki #15](https://github.com/Linductor-alkaid/heyaki/issues/15)，
  上游 #14 已提及但此前未独立跟踪。关联 M5-34；负责人 Linductor。
- 516815c / Ubuntu 24.04 / GCC 13 / Aki ASAN；fresh profiles、零 grant、
  单 Executor / 两 borrowed Runtime。A basic=true，B=false；文本 TTL 拒绝
  后 A 的文件 push 被接纳，随后 offered→paused，会话快照最终断连。
  无落盘/无 grant；取消 parked transfer 同步被拒。暂停回调先于断连快照，
  直接读取可能短暂 linked=1；回归等待最终快照后断言断连。
- 影响：严格策略/旧端拒绝业务通道后，复用缓存关闭通道的下一次发送可能
  破坏整条身份会话；不等于首次独立 offer 的 deadline 修复失败。
- 期望：退役/替换关闭通道，后续 push 明确拒绝或有界失败，健康身份会话
  保持；book 中 Paused transfer 的取消语义需明确。验证重复/迟到、重连、
  shutdown 与两种拒绝序列；独立管理上游，Aki 不直接改依赖。
- Aki 回归保留 budget 内 Failed 或 Paused+最终断连可见，未声称保持会话已
  修复。补跑条件为上游固定后复验 ASAN/TSAN/双端策略矩阵。


## HEY-20261002-002：取消文件同步关闭会话后访问已释放状态

- 状态 Resolved：[Heyaki #16](https://github.com/Linductor-alkaid/heyaki/issues/16)。
  关联 M5-42 的 CI 门禁及 M5-34/40 的文件取消；负责人 Linductor/Heyaki 上游。
- 版本 516815cbfb76f93f60acd4b58e5b6a7976e4417f，Aki head
  c3eab1a41299c1bd46f10c254a01a7af0d789d3a；CI run 36979577214。
  ASAN job 110751103417、TSAN job 110751103502 的全量测试均 44/45，
  唯一失败为 test_basic_communication_loopback。TSAN 248 assertions /
  6 cases 虽通过，真实 heap-use-after-free 使进程失败，不标为通过。
- 原有“Cancelling an unanswered offer wins before expiry with one terminal”
  用例：新身份、零 grant、单 Executor/two borrowed Runtime；A basic=true、
  B=false，push→offered→到期前 public Node cancel。Nodes/observer owner
  均仍存活，未提前关闭 Aki；不属于应用 shutdown 误用。
- 同一 strand 内 cancel_transfer 获得 SenderState*，file_service.cpp:372
  send_abort→PeerSession send_frame/pump/fail/notify→Node teardown_peer_services
  →FileService handle_session_closed:466→senders_.clear；返回 :373 再写
  sender->terminal，ASAN/TSAN 均报告已释放 map 节点。同一线程同步重入。
  TSAN 后续还报告退役 FileService 的成员访问，须同时保留 service 的生命期。
- 影响：取消尚未接收的 offer 可能访问释放内存；本地曾通过不能替代本次
  两类 sanitizer 证据。与 #15 的通道拒绝后复用/断连停车分开跟踪。
- 期望最小修复：不跨 send/callback 持有 map 裸引用，先快照/退役状态或
  每次可重入调用后按稳定 TransferId 重新校验，保留当前 service 所有权；
  审查 receiver cancel/相邻 abort 路径。终态、book/磁盘清理幂等，结果明确。
- 验收：注入 send_abort 同步失败→teardown 的确定性回归，再跑取消先于
  deadline、迟到/重复取消、严格策略拒绝、重连/关闭的 Debug/ASAN/TSAN。
  2026-10-02 原始处置：Aki 不修改 pinned 源码、不压制报告或删测试；PR #68 的合并/新包交付
  暂缓。上游 master 当前仅新增 Executor pin 升级，未修改此 FileService
  路径。待上游修复后独立接入、重跑完整七项 CI 与双端验收。

## 2026-10-03：HEY-20261002-002 修复接入开始

Heyaki #17 已合入 7e9758a 并关闭 #16，上游十二项 CI 全绿。Aki 按 M5-43
独立升级 Heyaki，并经用户明确授权同步其 Executor pin 至 e236273。
原 ASAN/TSAN UAF 证据保留；Aki 侧真实取消回环和最新 head CI 尚待完成，
完成后再将反馈状态改为 Resolved。#15 / HEY-20261002-001 继续独立跟踪。

2026-10-03 依赖接入闭环：精确 Aki head
6eaf5ea0bce71c739e9e15389f95d1304c5d7600 的 [CI run 37124555173](https://github.com/Linductor-alkaid/Aki/actions/runs/37124555173)
七项 completed/success（Linux Debug/ASAN/UBSAN/TSAN、Windows Debug、
Ubuntu 20.04 deb、Windows setup）。[PR #69](https://github.com/Linductor-alkaid/Aki/pull/69)
Squash 合入 84b8c3cd2b259cb5f474b18516c93e84675afecd；远程/本地依赖分支
已删除，主目录 master 已 fast-forward 同步且干净。M5-43 Completed，
HEY-20261002-002 Resolved；已有第三方 UBSAN 对齐限制和 Heyaki #15 保留。
该结论只关闭取消重入修复的依赖接入，不关闭 M5-34/41/42 的桌面双端验收。

## HEY-20261006-001：Node 运行期不可启用或更新 relay enrollment

- **状态**：已核对 pinned API 与实现，未修改依赖；已提交
  [Heyaki #19](https://github.com/Linductor-alkaid/heyaki/issues/19)；
  关联 [DEC-028](../decisions/DEC-028-relay-cross-subnet.md) 决策 3。
- **可复现证据**：relay enrollment 只在 `Node::create` 的
  `initialize_relay()`（`third_party/heyaki/src/client/node.cpp:1011/1102`）
  读取 profile 首条 `auto_connect && !revoked` 记录；`include/heyaki/node.hpp`
  公开面无运行期启用、停用或重载 relay 配置的方法（`RelayNodeConfig` 仅经
  `NodeConfig::relay_override` 构造期传入）。`put_relay_enrollment` /
  `mark_relay_revoked` 写入后，已运行的 Node 不感知。
- **影响**：Aki 设置页完成 enrollment 或移除后，正在运行的进程无法连接或
  断开 relay，必须重启应用生效（DEC-028 v1 限制）。运行期重建 NodeSession
  需同步重建 adapter、双观察管道、LanNameBeacon、ReconnectCoordinator 钩子
  与对账 sweep（均持有裸指针），等价二次装配，Aki 侧不可接受。
- **期望语义**：公开有界的运行期 relay 配置更新入口（如
  `update_relay_config(optional<RelayNodeConfig>)`），内部按关闭序重建 relay
  客户端并保持既有会话/目录语义；或暴露受控的 Node 热重启边界。
- **最小能力建议**：允许在 enrollment 记录变化后触发 relay 控制面重连，
  结果与失败（token 拒绝、网络不可达、pin 不匹配）经既有
  `RelayNodeSnapshot.last_error` 可观测。
- **Aki 侧处理**：v1 按重启生效实现并如实披露；上游提供该能力后评估热
  生效接入与双端验证。
- **负责人及补跑条件**：Linductor；上游提供并固定公开 API 后，在
  HostRuntime 接入运行期切换并补注册/移除/失败/恢复测试。

## HEY-20261006-002：relay enrollment WSS 客户端无法借用宿主 executor

- **状态**：已核对 pinned 实现并登记（M7 端到端验证发现，Independent
  验证报告 2026-10-06）；已提交
  [Heyaki #20](https://github.com/Linductor-alkaid/heyaki/issues/20)；
  未修改依赖。
- **可复现证据**：`RelayEnrollmentWssTransportConfig`
  （`include/heyaki/relay_enrollment_client.hpp:34-43`）无 Runtime/executor
  注入字段；`make_relay_enrollment_wss_exchange` 经 `RelayWssClient::create`
  （`src/client/relay_enrollment_client.cpp:88`）内部
  `Runtime::create_owned` 自建 executor（与 `Node` 侧可借用
  `Runtime::create_borrowed` 形成对照）。Aki 侧
  `aki::heyaki::enroll_relay_profile` 因此在进程内运行第二 executor 实例
  约 ≤12s（transport 超时上界）。
- **影响**：违反 Aki EXEC-01「进程内 executor owner 唯一」的严格执行面；
  交换线程不在宿主 Executor 的监控/关闭视图内，宿主 shutdown 无法取消
  在途 enrollment 交换（只能等待其超时自然结束）。
- **期望语义**：enrollment transport 接受借用 Runtime 注入（与
  `NodeConfig.runtime` 同款非拥有指针），使交换运行在宿主 executor 上并
  进入其生命周期视图。
- **Aki 侧缓解（当前已实施）**：交换包在宿主 executor 的 `submit_auto`
  一次性任务内（结果/异常经 future 与 SetRelayStatus 可见）；HostRuntime
  关闭序有界等待该 future（≤12s 自然上界）。风险面收敛为「短暂脱离监控
  视图的第三方 worker」，不静默、不阻塞关闭。
- **负责人及补跑条件**：Linductor；上游提供借用注入后，将
  `relay_enrollment.hpp` 切换为借用形式并补关闭竞争测试（enroll 在途时
  shutdown）。

## HEY-20261006-003：PairingService 审计计数器跨线程无同步

- **状态**：已抑制收口（tsan-suppressions.supp，沿 M3-06 usrsctp 先例）；
  已提交 [Heyaki #21](https://github.com/Linductor-alkaid/heyaki/issues/21)；
  未修改依赖；待上游修复后移除抑制并复跑 tsan。
- **可复现证据**：[PR #70](https://github.com/Linductor-alkaid/Aki/pull/70)
  CI run 37504293005 tsan job 112408875988，test_host_runtime（184 断言
  全过）后 TSAN 报 1 处 data race：主线程
  `HostRuntime::set_local_pairing_password → Node::rotate_authorization_password
  → PairingService::rotate_password → PairingService::audit`
  （pairing_service.cpp:80 `++stats_.password_rotated`，8 字节写）与
  heyaki 内部 `schedule_expiry` 定时器 strand 的
  `Node::Impl::prune_peer_services → metrics_strand`（读）竞争。
- **根因**：`PairingService::stats_` 为普通计数器结构（非原子、非 strand
  串行）：公开方法同步路径写、内部定时器经 metrics 快照路径读，两个上下
  文无公共互斥。Aki 侧无契约违约——读路径是 heyaki 内部定时器，不在
  NodeSession「调用方串行化」面内。
- **影响**：诊断计数可能丢失个别增量（对 pairing 结果/授权无影响）；
  TSAN 门禁不可绿。
- **期望最小修复**：stats_ 计数器原子化（std::atomic）或 audit 调用
  一律经 pairing strand 派发；metrics 读侧同步取快照。
- **Aki 侧处理**：抑制表条目 `race:heyaki::PairingService::audit` /
  `race:heyaki::Node::Impl::metrics_strand`；修复后移除并复跑全量 tsan。
- **负责人及补跑条件**：Linductor；上游修复合入并升级 pin 后移除抑制、
  复跑 tsan 七项门禁。
