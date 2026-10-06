# M7：中继跨网段通信

> 状态：In Progress
> 负责人：Linductor
> 所属计划：[Aki 实施总计划](aki-implementation-plan.md)
> 前置：M3（LAN 发现/信任/会话主链路；Relay 发现分期项由本里程碑接续）、
> M5（设置页/UiActions/状态消费面惯例已冻结）、
> [DEC-028](../decisions/DEC-028-relay-cross-subnet.md)（2026-10-06 Accepted）
> 建议发布点：v0.7.0
> 更新日期：2026-10-06（创建）

## 目标

基于 Heyaki relay 控制面实现跨网段设备通信：设置页配置中继服务器
（wss 地址 + 租户 + bootstrap token + 可选 CA），enrollment 持久化后重启
生效，设备自动登录 relay 并把同租户在线端点合入既有发现目录，经同一
`on_device_discovered`/配对/信任/消息/文件链路通信；跨网段数据面由 ICE/TURN
（coturn，设置页高级区静态凭据）兜底；relay 运行态在设置页可见。交付对应
SCOPE-02 的 Relay 来源与 SCOPE-10 的 Relay 路径展示。

## 范围与非目标

**范围**（依据 DEC-028 决策 1~9）：

- `NodeSession`：`production_lan_configuration()`（automatic）、
  `Options.ice_servers` 透传 `path_policy_override`、`connect_peer`（通用
  建链）、`EndpointView.via_relay` + relay 公钥回落、`relay_status()` 运行态。
- enrollment 包装（`heyaki/session/relay_enrollment.hpp`）与设置页向导
  （注册/移除/状态展示，重启生效提示）；移除 = `mark_relay_revoked`。
- 发现/建链解锁：diff 映射 `DiscoveryMethod::Relay`；`begin_pairing` 与
  重连钩子改 `connect_peer`；`start_discovery` 接受 Relay method。
- ICE/TURN 一期：`ice-servers.txt` 解析/写入（`set_language` 先例）+ 设置页
  高级区单 TURN 服务器；静态长期凭据。
- AppState 易失 `RelayStatus` + `SetRelayStatus` 更新类型 + UI 消费派生。
- 文档：DEC-028、设计 §4/§8.1 同步、部署指引（引用上游 deployment/coturn，
  Aki 不部署 relay）。

**非目标**：

- relay 服务器与 coturn 的部署运营（heyaki 自身范围；Aki 仅文档指引）。
- 注册热生效（HEY-20261006-001；v1 重启生效并如实披露）。
- 多 relay 管理与多租户切换 UI（单 enrollment；upstream 取首条
  auto_connect 记录）。
- 邀请链接与手动输入 Device ID 发现来源（继续分期，SCOPE-02 余项）。
- TURN HMAC 短期凭据（coturn use-auth-secret）与 TURN/TCP（vendored
  libjuice 仅 TURN/UDP；需 libnice 后端时另行决策）。
- relay pin（leaf 证书 SHA-256）设置项（CA 路径已覆盖自签部署；pin 随
  运维需求再评估）。
- `force_turn_data_path` 强制中继调试开关。

## 设计与决策依据

- [DEC-028](../decisions/DEC-028-relay-cross-subnet.md)（本里程碑冻结决策）
- [Aki 设计方案](../design/aki_design.md)§4（发现来源）、§8.1（SPI）、
  §8.3（装配）、§10（状态边界）
- [DEC-006](../decisions/DEC-006-heyaki-api-contract.md)（映射 2/5）、
  [DEC-015](../decisions/DEC-015-per-device-connection-path.md)（路径）、
  [DEC-018](../decisions/DEC-018-pairing-password-entry.md)（口令入站面）
- 上游参考：`third_party/heyaki/docs/skill/heyaki-integration/references/relay.md`、
  `docs/deployment.md`、`deploy/coturn/`、`apps/demo/m4_matrix_node.cpp`
  （TURN 凭据先例）

## 工作项

- [x] `M7-01` 会话层解锁（DEC-028 决策 1/6）：`production_lan_configuration`
  （automatic + 生产节奏参数）替换组合根 `fast_lan_configuration`；Options
  增 `ice_servers`（→ `path_policy_override`，`validate_peer_path_policy`
  前置校验，失败装配可见）；`connect_peer` 包装 `Node::connect`；
  `EndpointView.via_relay` 与 relay 公钥回落；`relay_status()`（aki/std
  公开面：enabled/state/url/tenant/last_error 数值或枚举名）。
- [x] `M7-02` enrollment 包装（决策 2/4）：`heyaki/session/relay_enrollment.hpp`
  ——`enroll_relay_profile(LocalProfile&, RelayEnrollRequest, error&)`（内部
  WSS 交换，token 用后擦除）、`revoke_relay_enrollment`、
  `relay_enrollment_view`（url/tenant/revoked/generation）；URL `wss://`
  前缀校验、tenant 非空校验、CA 文件存在性前置拒绝。
- [x] `M7-03` 发现与建链解锁（决策 5/6）：`diff_lan_discovery` 按
  `via_relay` 映射 Relay method 与 `relay:` 前缀；纯 relay 条目公钥校验
  不再跳过；`HeyakiNodeAdapter::begin_pairing` 与宿主重连 `try_reconnect`
  钩子改 `connect_peer`；`start_discovery` 接受 LanDiscovery/Relay。
- [x] `M7-04` AppState relay 状态（决策 8）：`RelayStatus`（易失、不持久化）
  + `SetRelayStatus` 更新类型 + owner apply（整体替换幂等）；`ui_state_consumer`
  经 `UiStateSnapshot.state.relay` 直读（AppState 全量携带，无需新派生）。
- [x] `M7-05` HostRuntime 接线（决策 2/4/7/8）：启动解析 `ice-servers.txt`
  注入 Options；`enroll_relay`/`remove_relay`（`submit_auto` 一次性任务，
  结果经 `SetRelayStatus` 回传 + token 擦除）；`set_turn_server`（异步写 +
  在途 future 互斥，`set_language` 同款）；sweep 周期采样 `relay_status`
  变化提交；装配后首推当前 enrollment 状态。
- [x] `M7-06` 设置页（决策 2/4/7）：中继区块（地址/租户/token secureInput/
  CA 路径 + 注册按钮 + 状态行 + 移除按钮 + 重启生效提示）与 TURN 高级区
  （host/port/username/credential secureInput + 保存）；`UiActions` 出站面、
  i18n 中英词条、`main.cpp` 组合根绑定；沿 aki_ui_design 令牌与组件映射
  （落地记录见该文档 2026-10-06 节）。
- [x] `M7-07` 测试（Independent-Verification-Agent 执行）：DEC-028 验证方式
  ①~⑦ 单测/集成（diff relay、公钥回落、connect_peer 映射、enrollment
  失败路径、SetRelayStatus apply、ice 文件解析/写入互斥、UI 动作路由）。
  2026-10-06 IVA 实测：新增 test_relay_integration（16 用例/131 断言）与
  test_host_relay_config（1 用例/34 断言），扩展 test_app_state/
  test_host_runtime/test_heyaki_node_adapter；对抗用例捕获 ICE 端口前缀
  解析缺陷（"3478abc"/"+5"/"0x10" 被静默矫形）已修复并复验；本地 debug
  全量 ctest 47/47。
- [ ] `M7-08` 回归验证：全量 ctest（debug/release）+ ASAN/UBSAN 零回归；
  LAN 主路径（发现/配对/消息/文件/断线恢复）在 automatic 模式下行为不变。
  （debug 47/47 已过，2026-10-06；release/ASAN/UBSAN 待 CI 门禁证据。）
- [ ] `M7-09` 收口审计：实现与设计 §4/§8.1/§10、DEC-028、总计划 SCOPE-02/
  SCOPE-10 状态、UI 设计规范 §4/§5 落地记录同步；真实 relay + coturn 双端
  跨网段联调证据或如实降级记录（补跑条件：测试 relay 环境 + 双网段真机）。

## 风险与阻塞

- **重启生效 UX**（HEY-20261006-001）：设置页文案如实披露；不构建临时
  网络栈重建。
- **automatic 回归面**：无 enrollment/无 ICE 时与 lan_only 行为等价性依赖
  上游 `select_signaling_route`/`default_peer_path_policy` 语义，M7-08 全量
  回归背书。
- **真实 relay 联调环境**：进程内 `RelayServer` 集成测试可先行（上游
  `m3b_relay_test.cpp` 同型）；跨网段真机联调依赖部署环境，缺失时按
  工程规范 §4 记录限制与补跑条件，不冒充已验证。
- **relay 目录租户隔离与 256 上限**：部署指引如实说明；非 Aki 可控。

## 测试与退出条件

- 退出-1：DEC-028 验证方式 ①~⑦ 全部通过（进程内可验证部分）。
- 退出-2：M7-08 全量回归零失败；CI 七项全绿。
- 退出-3：真实 relay 环境（自建 `heyaki-relay` + 可选 coturn）双设备跨网段
  「注册 → 重启 → 发现 → 配对 → 文本/文件 → Relay 路径徽标」闭环证据；
  环境缺失时整项不勾选，记录补跑条件。
- 退出-4：文档同步矩阵闭合（设计/决策/计划/UI 规范/反馈台账交叉引用）。

## 验证记录

### 2026-10-06：M7-01~M7-07 实现与本机验证

- 环境：Ubuntu 24.04 / x86_64 / GCC 13.3 / CMake 3.28.3，debug preset
  （`build/debug`）。
- 实现：`heyaki/session/{runtime_node,relay_enrollment,ice_config}` 、
  `heyaki/adapter/{lan_discovery,heyaki_node_adapter}`、
  `app/state/{app_state,app_state_updates,app_state_owner}`、
  `app/lifecycle/host_runtime.{hpp,cpp}`、`ui/models/ui_actions.hpp`、
  `ui/pages/{settings_page.cpp,main_window.hpp,main_window.cpp}`、
  `ui/i18n.hpp`、根 `main.cpp`；文档 DEC-028、设计 §4/§8.1、UI 规范
  §4/2026-10-06 节、总计划（当前状态/里程碑索引 M7）、M3 交叉引用、
  反馈台账 HEY-20261006-001。
- 测试（Independent-Verification-Agent 编写并执行，两轮）：新增
  `tests/unit/test_relay_integration.cpp`（16 用例/131 断言：diff relay
  映射 6 用例、enrollment 校验/不可达失败/撤销 4 用例、ICE 解析/序列化/
  校验 6 用例）、`tests/unit/test_host_relay_config.cpp`（1 用例/34 断言：
  预写文件装配 1/1、turn_server 预填、异步 enroll 失败路径 <2s、
  remove_relay 无记录拒绝、set_turn_server 覆写、关闭零写失败）；扩展
  `test_app_state.cpp`（SetRelayStatus 首推/幂等/替换，+26 断言）、
  `test_host_runtime.cpp`（+94 行：装配报告 0/0、relay 首推基线、静态
  拒绝矩阵、合法写入轮询文件内容）、`test_heyaki_node_adapter.cpp`
  （`start_discovery(Relay)` 真实 adapter 接受 + InviteLink 仍拒绝）。
- 缺陷发现与修复（IVA 对抗用例）：`parse_ice_servers_content` 端口
  token 的 `std::stoul` 前缀解析接受 `3478abc`/`+5`/`0x10`——改为纯
  数字校验后复验（含前导零仍合法对照），test_relay_integration 16/16。
- 本机 debug 全量 ctest：**48/48 通过**（45 既有 + 3 新增，零回归）；
  `borrowed_runtime_shutdown_performed == false` 关闭断言保持。

### 2026-10-06：进程内 relay 端到端（DEC-028 决策 10/验证方式 ⑧）

- 新增 `tests/integration/test_relay_e2e_loopback.cpp`（单用例/99 断言；
  LABELS integration）：进程内 `RelayServer`（借用 Runtime + OpenSSL
  自签证书含 SAN IP:127.0.0.1 + `RelayDatabase` 种 bootstrap token）
  → 双端 `aki::heyaki::enroll_relay_profile` 真实 WSS + Ed25519 挑战 +
  token 准入 → 记录 pin 持久化对拍（独立 `X509_digest` 复算逐字节相等）
  → 双 NodeSession（纯 relay 拓扑：production 配置 + `lan.enabled=false`）
  60s 内 login ready → A 目录见 B（relay_visible、公钥 32B）→
  `diff_lan_discovery` 产 `DiscoveryMethod::Relay` + `relay:` 前缀 →
  `connect_peer` → pairing_restricted → 单侧口令 `pair_peer` → 双侧
  authenticated → `send_text` 入站 + ack → 受控拆除（双 Node/server/
  借用 runtime `!executor_shutdown_performed`）。4 次运行（3 直跑 + 1
  ctest）全过，单次 ~17s。
- **缺陷发现与修复（该轮 IVA 实测）**：enrollment 记录原不持久化任何
  login 信任基——自签 relay 注册成功但重启语义登录必败
  （`wss_tls_verification_failed`；上游 profile 记录无 ca_file 字段）。
  修复：`relay_certificate_pin`（证书文件首证书 DER SHA-256）随记录以
  `relay_pin` 持久化，登录期 TOFU pin 校验替代链校验（DEC-028 决策 2
  修订；有意不拒 CA:TRUE——上游 quickstart 产物即 CA:TRUE 服务端证书，
  私 CA 错配在登录期 `wss_tls_pin_mismatch` 可见）。配套：
  aki_heyaki 链 OpenSSL（heyaki/ 目录显式 find_package，imported
  target 不跨兄弟目录）。
- **上游缺口登记**：HEY-20261006-002（enrollment WSS transport 无法
  借用宿主 executor，内部自建 owned runtime ≤12s；Aki 以 executor 任务
  包裹 + 关闭序有界等待缓解）。
- 修复后全量 ctest **48/48**；`relay_certificate_pin` 函数面错误路径
  （不存在/非 PEM 文件）已覆盖。
- 未验证（环境限制，如实降级）：真实跨网段双端（外部 relay + coturn、
  两网段真机）——补跑条件：自建 relay（上游 quickstart）+ 双网段真机
  + TURN 3478/49160-49200 放行；负责人 Linductor。对应 M7 退出-3 的
  「真实环境」半边，未执行不随项勾选。
- release 本机构建通过；ASAN/UBSAN/TSAN 归 CI 七项门禁（M7 退出-2 待
  CI 证据）。
