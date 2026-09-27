# DEC-016：配对口令常量与本地 verifier 真实化

> 状态：Accepted
> 日期：2026-09-27
> 负责人：Linductor
> 冻结里程碑：M5-04（Devices 页信任操作面；真实口令流程随 M5-07 设置面）
> 替代/被替代：取代 M3-03 的占位 verifier（heyaki 自身测试同型的假编码串，
> local_identity.hpp 头注自认「DEC-006 未冻结口令处理」）

## 背景与问题

DEC-006 映射 3 冻结了 `Pending→Trusted ← pair_peer + set_pairing_observer`，
但未冻结口令处理。M5-04 实现信任确认面时必须回答 `pair_peer` 的 password
提交值与本端 verifier：heyaki 的 `pair_peer` password 由**目标端**对其本地
profile verifier 验证（pairing_service evaluate→verify_password）——它是
「目标端授权口令」而非两端即时约定的 PIN。探针实测（pinned heyaki vendored
libsodium，`crypto_pwhash_str_verify` 直验）：M3-03 占位串
`"$argon2id$v=19$m=65536,t=2,p=1$aki$aki"` 是假编码，对任何候选口令全部
拒绝——沿用它的任何 `pair_peer` 提交都无法通过验证，Pending→Trusted 永久
不可达（M3-09 登记的 right/wrong-password 回环用例在假 verifier 下也无
区分度）。

## 决策

- **口令常量**：新增冻结常量 `kAkiPairingPassword`（≥8 个 Unicode 标量，
  满足 `PasswordSecurityPolicy.minimum_unicode_scalars=8`；置于
  `heyaki/adapter/local_identity.hpp`，与 `kAkiApplicationId` 同点）。
  信任确认弹窗**无口令输入框**（aki_ui_design §3：弹窗=mono 公钥指纹
  确认）；`pair_peer` 提交值即该常量，在 heyaki/adapter→DeviceManager
  内部传递，不进 SPI/UiActions 签名。
- **verifier 真实化**：`converge_local_initialization`（首次 initialize_local，
  created 分支）的占位 verifier 替换为
  `create_password_verifier(kAkiPairingPassword, PasswordHashParameters{})`
  生成的真实 argon2id verifier（pinned api.md 确定性示例同型："Real apps
  prompt for the password; this example pins one for determinism"）。
- **存量 profile 处置（不静默）**：verifier 修正只走 created 分支，不触达
  存量数据目录（含本机 %APPDATA%\aki）。MVP 处置声明：删除
  `db/profile.sqlite` 重建（新 verifier 生效）；或经
  `set_password_verifier`+`password_generation` 升级（M5-07 设置面）。
  存量 profile 上的配对将以可见失败告终（detail 经配对结果事件投递），
  不得静默。
- **安全语义披露（MVP 接受）**：固定口令=公开弱口令——目标端口令验证通过
  即自动签发 SignedTrustGrant（被动端无二次用户确认），开源常量意味着任何
  LAN 端可提交正确口令。既有缓解：per-session attempt budget + per-peer
  指数退避（heyaki pairing 默认开启）。移除条件：M5-07 设置面引入用户
  口令（弹窗改造/存储面）后删除本常量，本决策标记 Superseded。
- **测试同步**：集成回环测试的口令字面量（aki-loopback-pw 等）同批替换为
  `kAkiPairingPassword`；新增网络无关单测锁定
  常量↔`create_password_verifier`↔`verify_password` 往返（正确口令 MATCH、
  他串拒绝）。

## 备选方案

- **弹窗加口令输入框（否决）**：口令是目标端本地 verifier 授权口令而非两端
  即时约定 PIN；M5-07 前两侧均无用户口令，输入串与任何 verifier 无定义
  关系——把固定常量能确定性完成的事转嫁为新增失败面（错配→unauthenticated
  →Rejected），且违反 aki_ui_design §3 弹窗规格（仅指纹确认）、需扩
  UiActions/DeviceManager 契约并在 M5-07 再改一次。
- **原样沿用 M3-03 占位串、verifier 不动（否决）**：探针实测任何提交都
  被拒，配对永久失败；回环用例的 right/wrong-password 区分度 vacuously
  true。
- **M5-04 只改本地信任态、跳过真实 pair_peer（否决）**：违反 DEC-006 映射 3
  冻结映射，M5-08 双端 MVP 闭环不可达。

## 影响与风险

- verifier 生成在启动恢复段主线程同步执行（§11.1 ②），argon2 m=64MiB、
  t=2 的创建耗时仅 created 分支承担——首启动耗时实测登记（M5-04 验证
  记录）。
- 端到端 Pending→Trusted（双端配对全链路）受防火墙环境限制，沿 M3-09
  降级纪律：网络无关半边（verifier 往返、SPI/DM/UiActions 通道、状态机
  转移边）本项验证；双端真链路归 M5-08 与 M3-09 补跑条件。
- 固定口令的安全弱点在 M5-07 收敛前如实存在于 MVP（本决策披露）。

## 验证方式

M5-04 实施：①单测 kAkiPairingPassword↔create_password_verifier↔
verify_password 往返（正确 MATCH/他串拒绝/短串生成被策略拒绝）；②集成
回环口令字面量统一替换后既有用例语义保持；③首启动（created 分支）耗时
实测登记；④存量 profile 处置路径以本记录为准在 M5-04 验证记录复述；
⑤双端 Pending→Trusted 归 M5-08/M3-09 补跑（不冒充已验证）。

## 关联文档和工作项

- [DEC-006](DEC-006-heyaki-api-contract.md)（映射 3 信任/配对契约——本决策
  补其口令处理挂起项）
- [Aki 设计方案](../design/aki_design.md)§8.1（SPI 信任操作）、§4（信任确认）
- [Aki UI 设计规范](../design/aki_ui_design.md)§3（确认弹窗=指纹，无口令框）
- [M3 里程碑](../plans/m3-heyaki-integration.md)（M3-03 占位 verifier 登记、
  M3-09④ 真实口令随 M5 设置面）
- [M5：EUI-NEO UI 与 MVP 验收](../plans/m5-eui-neo-ui-mvp.md)（`M5-04`/
  `M5-07`/`M5-08`）
