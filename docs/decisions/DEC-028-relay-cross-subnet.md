# DEC-028：基于 Heyaki Relay 的跨网段通信接入

> 状态：Accepted
> 日期：2026-10-06
> 负责人：Linductor
> 修订：2026-10-06 增补决策 10（测试专用消费 heyaki::relay，进程内端到端
> 验证面）；2026-10-07 增补决策 11（中继接入「地址 + 密码」分阶段简化，
> 修订决策 2/7 的主路径形态——token 流程降级为高级路径全程保留）；
> 其余条款不变
> 冻结里程碑：M7 开工编码前（SCOPE-02 Relay 发现来源 / SCOPE-10 Relay 路径展示）
> 前置：[DEC-006](DEC-006-heyaki-api-contract.md)（映射 2/5 与 lan_only 分期）、
> [DEC-015](DEC-015-per-device-connection-path.md)（路径状态模型）、
> [DEC-018](DEC-018-pairing-password-entry.md)（口令入站面先例）
> 关联工作项：M7-01~M7-09（[m7-relay-cross-subnet.md](../plans/m7-relay-cross-subnet.md)）、
> M8-01~M8-06（[m8-relay-simple-enrollment.md](../plans/m8-relay-simple-enrollment.md)）

## 背景与问题

SCOPE-02 把发现来源设计为「局域网发现、已知设备记录、Relay、邀请链接或手动
输入」五类，M3 仅落地 LAN（`start_discovery` 非 LanDiscovery 一律拒绝；运行期
`connectivity_mode` 硬编码 `lan_only`；`relay_override`/`path_policy_override`
恒为空），跨网段设备互相不可见。用户需求：在设置页配置中继服务器地址后，Aki
自动发现同在该中继下的所有设备，并沿用既有配对/信任流程通信。

上游 pinned heyaki 的 relay 能力已齐备（本决策附录 A 为核实摘要）：

- relay 是**控制面**：bootstrap token 准入 + Ed25519 挑战签名注册、在线端点
  目录（publish/query，租约 45s/心跳 15s）、WSS/TLS 1.3 信令转发；**数据面
  不经 relay**——消息与文件走 WebRTC DataChannel（直连或 TURN 兜底）。
- 客户端 enrollment 记录持久化在 heyaki profile store
  （`RelayEnrollmentRecord`），`Node::create` 时 `initialize_relay()` 读取首条
  `auto_connect && !revoked` 记录自动连接；**运行期无更新 relay 配置的 API**。
- `endpoints()` 返回 LAN + relay 合并目录；`connect()` 按
  `ConnectivityMode` 自动选路（automatic：LAN 优先、relay 兜底）；
  `pair_peer`/消息/文件对 relay 信令会话无任何限制。

## 决策

1. **连通模式切 `automatic`**（LAN 优先、relay 兜底）：生产配置以
   `aki::heyaki::production_lan_configuration()` 替代
   `fast_lan_configuration()`（后者保留为测试快速配置）。无 enrollment 时
   relay 分支自然关闭（`initialize_relay` 返回 disabled），automatic 与
   lan_only 在纯 LAN 网络行为一致（ICE 未配置服务器时仅 host 候选）。
   不选 `relay_only`：保留 LAN 直连主路径，避免 LAN 回归面。
2. **注册（enrollment）走设置页一次性向导**：字段 = 中继地址（`wss://` URL）、
   租户名（默认 `aki`）、bootstrap token（`secureInput` 掩码）、可选
   证书文件路径。**证书语义（2026-10-06 实测修订）**：公网 CA relay 留空
   即系统信任 + 主机名校验；自签部署须提供 relay 服务端使用的证书文件
   本体（上游 quickstart 的 relay.crt）——enrollment 交换用它做链 + 主机名
   校验，其 SHA-256 以 `relay_pin` 随 `RelayEnrollmentRecord` 持久化，
   登录起 TOFU pin 校验替代链校验（profile 记录无 ca_file 字段，pin 是
   唯一可持久化信任基——进程内端到端实测发现纯 ca_file 路径重启后登录
   必败 `wss_tls_verification_failed` 后定型）。执行 =
   `HostRuntime::enroll_relay` 提交 executor 任务（`submit_auto`，一次性有界
   ——WSS transport 超时上界 connect 5s + handshake 5s + close 2s），任务内
   调 `aki::heyaki::enroll_relay_profile`（`heyaki/session/relay_enrollment.hpp`
   包装，RULE-10 封死 heyaki 类型），成功写 heyaki profile 的
   `RelayEnrollmentRecord`。token 用后即擦除（凭据纪律，沿 DEC-018）。
   已知上游限制：交换内部自建 owned runtime（HEY-20261006-002），Aki 以
   executor 任务包裹 + 关闭序有界等待缓解。
3. **注册变更重启生效（v1 明确限制）**：`Node::create` 一次性读取 enrollment
   且无运行期更新 API；Aki 组合根内 LanNameBeacon/PeerSessionPipeline/
   ReconnectCoordinator/对账 sweep 均捕获 `NodeSession` 裸指针，运行期重建
   网络栈等于二次装配，风险不可接受。UI 在注册成功与移除后提示「重启 Aki 后
   生效」。上游能力缺口登记 `HEY-20261006-001`（见反馈台账）。
4. **移除 = 标记撤销**：`mark_relay_revoked(relay_url, generation)`（profile
   持久化），同样重启生效。不做多 relay 管理：v1 单条 enrollment（upstream
   `initialize_relay` 只取首条 auto_connect 记录），设置页展示当前记录与
   运行态。
5. **发现：合并目录单一观察管道**：`EndpointView` 增 `via_relay` 来源标记，
   `public_key` 在无 LAN 条目时回落取 `relay->identity_public_key`（纯 relay
   条目不再被误判「非 Aki 广播」跳过）；`diff_lan_discovery` 按 `via_relay`
   映射 `DiscoveryMethod::Relay` 与 endpoint 前缀 `relay:`。**不建第二条观察
   管道**——LAN 与 relay 条目本就合并在 `endpoints()`，diff 幂等/回落语义
   （M5-11）原样复用。`start_discovery` 接受 LanDiscovery 与 Relay 两种
   method（同一管道启停），其余仍拒绝（邀请链接/手动输入继续分期）。
6. **建链：通用 `connect()` 取代 `connect_lan` 发起**：
   `NodeSession::connect_peer` 包装 `Node::connect`（automatic 自动选路）；
   `begin_pairing` 降级链与 ReconnectCoordinator 的 `try_reconnect` 钩子改用
   之。`connect_lan` 保留（LAN 语义显式入口 + 既有测试）。对账 sweep 的
   `endpoint_visible` 判定天然覆盖 relay 条目（合并目录）。
7. **TURN/ICE 纳入一期**：跨网段打洞失败须 TURN 兜底（relay 不转发数据）。
   配置 = 设置页高级区单 TURN 服务器（host/port/username/credential，静态
   长期凭据；coturn 同端口兼 STUN），持久化为数据根下 `ice-servers.txt`
   （每行 `kind host port [username credential]`，`set_language` 同款
   `submit_auto` 异步写 + 在途 future 互斥；无设置表 schema 变更）。启动时
   解析注入 `NodeSession::Options.ice_servers` →
   `path_policy_override`（`default_peer_path_policy(automatic)` + 服务器列
   表，`validate_peer_path_policy` 前置校验）。HMAC 短期凭据（coturn
   use-auth-secret）与 TURN/TCP（需 libnice 后端）明确延期：vendored
   libjuice 仅 TURN/UDP。
8. **relay 运行态可观测（AppState 易失字段）**：新增更新类型
   `SetRelayStatus`（SetPresence 先例，部分更新），AppState 增易失
   `RelayStatus{enrolled, relay_url, tenant, connection(int), last_error}`——
   不持久化、不做领域状态机（配置状态非信任域）。写入点：装配后首推、
   enrollment 任务完成回调（成功/失败细节）、5s 对账 sweep 周期采样
   `NodeSession::relay_status()`（连接态 ready/degraded/failed/stopped 变化
   才提交，水位去重由快照 diff 吸收）。UI 经 `consume_ui_state` 派生展示。
9. **中继服务器部署不在 Aki 范围**（沿 M3 非目标）：文档引用上游
   `docs/deployment.md`/`deploy/coturn/` 与 `heyaki-relay` quickstart；
   Aki 侧仅提供注册入口与运行态展示。
10. **测试专用消费 `heyaki::relay` 库目标（2026-10-06 修订补充）**：为使
   relay 接入具备进程内端到端验证（enrollment 成功路径、relay 发现、
   经 relay 信令的配对与消息—— otherwise 全部依赖外部环境），M7 集成
   测试允许**仅测试目标**链接 `heyaki::relay`（`src/relay` 内部头）并经
   OpenSSL API 生成测试自签证书（上游 `tests/unit/m3b_relay_test.cpp`
   同型）。边界与理由：
   - DEC-006「只链接 `heyaki::client`」冻结的是**产品面**消费；测试面
     扩展不改变产品依赖图。`heyaki_relay` 库目标本就无条件存在于单一
     构建图（`HEYAKI_BUILD_APPS=OFF` 只排除可执行程序），链接它不新增
     编译单元或外部依赖（OpenSSL 经 heyaki 传递）。
   - `RelayServer::create(config, Runtime*)` 接受借用 Runtime——测试内
     与双 NodeSession 共用同一测试 executor（worker 名互异，EXEC-01
     纪律不破）。
   - 风险：`src/relay` 头非 SDK 公开面，上游升级可能变。缓解：pinned
     lock 纪律 + 消费面收敛在单个测试文件；升级破坏时按依赖升级流程
     处理，不阻塞产品面。
   - 测试纪律：LABELS "integration"、独立临时目录、有界等待 + 环境失败
     [skip] 降级先例、token/证书只存在于临时目录。
11. **中继接入「地址 + 密码」分阶段简化（2026-10-07 增补）**：产品目标为
   设置页中继区主路径只出现「中继地址 + 注册密码」两项——租户、
   bootstrap token、证书文件退出主视图，TURN 概念退出用户视野（机主侧
   对应上游 `--init` 密码引导提案）。分三阶段落地：阶段 1（UI 收拢 +
   决策/台账同步）无上游依赖；阶段 2/3 分别依赖上游两项能力
   （[HEY-20261007-001](../heyaki_feedback/ledger.md) 密码准入 + 注册结果
   回传 leaf 指纹、[HEY-20261007-002](../heyaki_feedback/ledger.md) relay
   下发短时效 ICE 配置）。
   - **阶段 1（Accepted）**：设置页新增折叠组件（`ui/components/fold.hpp`
     ——EUI-NEO 无 disclosure 组件，chevron 码点按 UI 规范 §2.6 登记并经
     捆绑字体 cmap 实证）：未注册主视图 = 地址 + 注册按钮，租户/token/
     CA 收拢进「高级中继设置」折叠区（默认收起，必填校验失败自动展开）；
     token 制部署能力零删减（高级区回归面）。配套：enrollment wrapper 的
     bootstrap_token 改为原位擦除（全部退出路径，凭据纪律对调用方可断言
     ——阶段 2 密码擦除断言测试的同型缝合点）。
   - **阶段 2（暂定默认值，冻结于开工前；负责人 Linductor）**：上游密码
     准入落地并 pin 后：主视图换为 URL + 密码（secureInput，DEC-018 纪律，
     断言测试沿 token 原位擦除同型）；租户不出现在 UI（上游落默认租户；
     若要求显式等于默认租户则由 Adapter 层自动填充）；信任基 = 注册交换
     实际呈现的 leaf 证书 SHA-256 以 `relay_pin` 持久化（决策 2 的 pin
     语义不变，来源从「用户提供的 ca_file」变为「注册交换所见证书」）。
     **TOFU 首连窗口如实披露**：安全假设为「地址 + 密码的送达渠道可信」；
     首次连接被 MITM 时攻击者可完成准入并被锚定为 pin（注册结果回传的
     指纹来自同一 TLS 连接，不提供额外保证），设置页文案与部署文档按此
     表述；公网 CA relay 与严格自签部署继续走高级模式（系统信任根 /
     显式 ca_file）。
   - **阶段 3（暂定默认值，冻结于开工前；负责人 Linductor）**：上游 ICE
     下发落地并 pin 后：relay 凭据自动参与选路，`ice-servers.txt` 降级为
     高级覆盖（合并/优先级跟随上游语义）；设置页展示当前生效 ICE 配置与
     来源（静态文件 / relay 下发，决策 8 可观测同款精神）；短时效凭据的
     续期触发与过期时进行中 allocation 的存活语义随上游定型后在本决策
     补记。
   - 原 token 流程降级为高级路径**全程保留**（多租户、严格证书分发部署）；
     本修订不改变决策 2/7 已落地行为的语义，执行链（executor 任务、重启
     生效披露）不变。

## 备选方案

- **B（否决）：`relay_only` 模式**——关闭 LAN 组播，纯跨网段更可控，但放弃
  LAN 直连主路径（同网段吞吐/延迟最优路径），LAN 全部既有行为成为回归面；
  `automatic` 下 relay 兜底语义已满足需求。
- **C（否决）：运行期重建 NodeSession 使注册即时生效**——需拆除并重建
  adapter/两观察管道/beacon/重连协调器/sweep 全部持有裸指针的组件，等价于
  二次 `ensure_assembled`，关闭顺序与竞态风险远超收益（注册是每设备每
  relay 一次的低频运维动作）。
- **D（否决）：relay 发现单建观察管道**——`endpoints()` 本就是合并目录，
  两条管道对同一目录轮询两次并各自 diff，引入跨管道 seen 集一致性负担。
- **E（否决）：ICE 服务器存 SQLite 设置表**——schema v1 无设置表，扩表属
  公开契约变更须独立决策；`ui-language.txt` 平面文件先例已覆盖单值配置，
  ICE 列表为低频静态配置，平面文件 + 解析校验足够，避免为低价值场景扩
  schema。多服务器高级需求由文件多行格式预留（UI v1 编辑单条）。
- **子备选（否决）：enrollment 阻塞 UI 点击回调执行**——WSS 交换上界 ~12s，
  超出点击上下文「有界平台查询」纪律（§9.1 三不；注册表读先例为毫秒级）。

## 影响与风险

- **必须同步**：`heyaki/session/runtime_node.hpp`（Options/EndpointView/
  connect_peer/relay_status/production 配置）、新
  `heyaki/session/relay_enrollment.hpp`、`heyaki/adapter/lan_discovery.hpp`
  （diff relay 映射）、`heyaki/adapter/heyaki_node_adapter.hpp`
  （begin_pairing/start_discovery）、`app/state/app_state*.hpp`
  （RelayStatus + SetRelayStatus + owner apply）、
  `app/lifecycle/host_runtime.*`（ice-servers.txt 读写、enroll/remove、
  sweep 采样、装配注入）、`ui/models/ui_actions.*`、
  `ui/pages/settings_page.cpp`、`ui/i18n.hpp`、`main.cpp` 绑定、
  `ui/models/ui_state_consumer.hpp` 派生。
- **lan_only → automatic 回归面**：LAN 发现/配对/命名广播/断线恢复须全量
  回归（M7-08）；无 enrollment + 无 ICE 配置时行为等价性由上游
  `select_signaling_route`/`default_peer_path_policy` 语义背书并在测试覆盖。
- **重启生效 UX 缺口**：登记为 v1 限制（设置页文案如实披露），上游 API 演进
  后（HEY-20261006-001）再评估热生效。
- **TURN 静态凭据**：长期凭据轮换依赖运维（runbook 范围）；HMAC 短期凭据
  延期并登记于 M7 非目标。
- **relay 目录上限/租户隔离**：同租户在线端点 ≤256（上游
  `endpoint_query_max_results`），跨租户不可见——多团队部署需多 relay 或
  同租户约定，部署文档如实说明。
- Executor 无能力缺口：enrollment 走 `submit_auto`（一次性有界任务），ICE
  文件写沿 `set_language` 先例，无新通信组件，无需 9.4 台账。

## 验证方式

M7 实施并验证：①`diff_lan_discovery` relay 条目单测（Relay method/`relay:`
前缀/公钥回落/租约消失回落 Offline）；②`EndpointView` 纯 relay 条目公钥
32B 回落；③`begin_pairing`/重连钩子走 `connect_peer` 的映射单测（Fake +
真实 NodeSession 双节点 loopback）；④enrollment 包装失败路径（token 拒绝/
URL 非法/CA 缺失 → error 可见、profile 无记录）；⑤`SetRelayStatus` owner
apply（整体替换、幂等）；⑥ice-servers.txt 解析（合法/非法行/空文件）+
写入互斥；⑦全量 ctest 零回归 + ASAN/UBSAN；⑧进程内 relay 端到端（决策 10
增补）：`RelayServer` + bootstrap token → 双 `NodeSession` 经
`enroll_relay_profile` 真注册 → relay 目录互见（`DiscoveryMethod::Relay`
diff）→ `connect_peer`/配对/文本消息经 relay 信令闭环（数据面 127.0.0.1
host 候选，无需 TURN）；⑨真实跨网段双端联调（依赖外部 relay + coturn
环境；缺失时按 §4 纪律记录限制与补跑条件，不冒充已验证）。

## 关联文档和工作项

- [Aki 设计方案](../design/aki_design.md)§4（发现来源）、§8.1（SPI）、
  §8.3（装配）、§10（状态边界）
- [M7：中继跨网段通信](../plans/m7-relay-cross-subnet.md)（M7-01~M7-09）
- [M8：中继接入简化](../plans/m8-relay-simple-enrollment.md)
  （M8-01~M8-06，决策 11 分阶段落地）
- [M3](../plans/m3-heyaki-integration.md)验证记录③（发现来源分期——Relay
  部分由本决策接续）
- [DEC-006](DEC-006-heyaki-api-contract.md)（映射 2 发现/映射 5 路径）
- [DEC-020](DEC-020-lan-name-beacon.md)（LAN 名称广播——relay 对端名称沿
  配对/持久化路径，不经组播）
- `docs/heyaki_feedback/ledger.md`（HEY-20261006-001：Node 运行期不可更新
  relay enrollment；HEY-20261007-001/002：决策 11 阶段 2/3 上游前置）

## 附录 A：上游 relay 能力核实（pinned third_party/heyaki）

- `apps/relay/main.cpp` 独立可执行 `heyaki-relay`（`HEYAKI_BUILD_APPS`）；
  配置 `src/relay/relay_config.hpp` + `docs/configuration.md`：TLS 1.3/WSS
  单监听 8443、SQLite 持久化、bootstrap token 准入（无用户名密码）。
- 客户端：`RelayNodeConfig`（`include/heyaki/node.hpp:255`，`wss://` URL +
  tenant + 可选 pin/CA）；`enroll_relay_profile`
  （`include/heyaki/relay_enrollment_client.hpp:73`，同步阻塞 WSS 交换，
  成功写 `ProfileStore::put_relay_enrollment`）。
- 目录/信令：登录后心跳周期 `endpoint_publish` + 空 `endpoint_query`，同租户
  在线端点合入 `endpoints()`（`src/client/node.cpp:1747/1771`）；信令转发与
  LAN 同构（`LanSignalingMessageKind`，node.cpp:1479-1493）。
- 选路：`select_signaling_route`（node.cpp:8356）；`default_peer_path_policy`
  （node.cpp:8258）lan_only 禁 srflx/TURN、automatic 全开；ICE 注入
  `NodeConfig::path_policy_override`（`PeerPathPolicy::ice_servers`）。
- 数据面：`docs/skill/heyaki-integration/references/relay.md`「Payload never
  rides it」；TURN 部署参考 `deploy/coturn/`（REST HMAC 凭据先例
  `apps/demo/m4_matrix_node.cpp:391-423`）。
