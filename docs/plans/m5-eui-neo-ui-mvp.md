# M5：EUI-NEO UI 与 MVP 验收

> 状态：In Progress
> 负责人：Linductor
> 所属计划：[Aki 实施总计划](aki-implementation-plan.md)
> 前置：M2、M3、M4、`DEC-005`（2026-09-26 已冻结 `Accepted`）。M3/M4 工作
> 项（`M3-01`~`M3-09`、`M4-01`~`M4-07`）均已完成并经收口审计，两里程碑保持
> In Progress 待防火墙放行入站 TCP / LAN 双端真机环境补跑关闭（与 M3-09/
> M4-07 记录同批）——沿 M4 文档创建先例，环境补跑不阻塞本里程碑文档与
> 设计先行工作项（`M5-01`），UI 实现工作项开工时复核 M3/M4 状态（`M5-02`
> 开工前已复核：全部工作项完成、仅防火墙/LAN 双端补跑待关闭，不阻塞）；
> 真实 Adapter、NodeSession、发现/消息/图片/传输/presence/重连管道与恢复
> 语义均已就绪
> 建议发布点：v0.5.0（MVP）
> 更新日期：2026-09-27（M5-05 完成同日）

## 目标

交付 EUI-NEO 主窗口与基础主题（`SCOPE-12`）及四页导航（Conversations /
Devices / Transfers / Settings），覆盖设备列表（`SCOPE-04` 展示面）、
会话与聊天窗口（`SCOPE-05`/`SCOPE-06`/`SCOPE-07` UI 面）、文件卡片与
Transfers 页（`SCOPE-08` UI 面），并按设计第 15 节逐项验收 MVP
（`SCOPE-01`~`SCOPE-12` 全边界）。

UI 只消费 Application State（`RULE-02`/`EXEC-03`：DoubleBuffer 一致快照
主线程排空，`app::requestUpdate()` 跨线程唤醒）；并发全部经 pinned
executor（`DEC-005` 并发边界：不使用 EUI-NEO `app::async`/`core::network`/
`audio`；`ExecutorOwner` 关闭编入 `DslAppConfig::onShutdown`，保持
`EXEC-01` 唯一 owner）。本里程碑不改变 M3/M4 已交付的网络、持久化与传输
语义，只接 UI 面。

## 范围与非目标

### 范围

- `M5-01` 设计先行与运行复核：[Aki UI 设计规范](../design/aki_ui_design.md)
  第 5 节要求的「与 pinned EUI-NEO 实际版本一致性复审」+ EUI-NEO 最小运行
  探针（窗口/compose 循环/主题逐项覆写实测——`RISK-2026-002` 运行复核
  收口：virtuallist 固定行高模型 vs 变高气泡列、dialog/toast open 状态
  持有、文件对话框只读能力）；UI 装配契约固化（设计第 9 节细化：AppState
  →视图模型派生、快照消费与唤醒、`onShutdown` 关闭序、主题档位覆写清单、
  渲染层验证策略）；偏差先更新设计/决策再合代码（M1-08 纪律）。
- `M5-02` EUI-NEO 接入与主窗口骨架：submodule 拉取登记（`DEC-005` pin
  `b9032a8a`）+ 锁文件 + configure 三方校验（沿 M3-01 fetch 纪律）；单一
  构建图 `add_subdirectory(third_party/EUI-NEO)` + `eui_neo_configure_app()`
  （构建开关 CACHE FORCE 按 `DEC-005` 冻结值：`EUI_DEPS_MODE=bundled`、
  apps/test-fixtures/install/modules OFF、glfw+opengl）；`aki_ui` 实体化
  （`eui::neo` 仅由 `aki_ui` 链接，`RULE-01`/`RULE-10`）；三栏布局壳 +
  四页导航路由 + `ui/theme` light()/dark() 逐项覆写；`ExecutorOwner` 关闭
  编入 `onShutdown` 钩子（`EXEC-01`）。
- `M5-03` 状态消费面与视图模型：DoubleBuffer 快照主线程排空 +
  `app::requestUpdate()` 跨线程唤醒（`EXEC-03`/`RULE-02`）；`ui/models`
  视图模型从 AppState 派生（设备/会话/消息/传输四域）；UI 只读不直写，
  操作经 Application 出站面（Manager 出站接口）下达。
- `M5-04` Devices 页（`SCOPE-04` + `SCOPE-02`/`SCOPE-03`/`SCOPE-10` 展示
  面）：设备列表（名称/类型/OS/连接方式/在线状态）、发现→信任操作面
  （Pending 确认/拒绝/Revoked 撤销）、Presence 与连接路径徽标
  （`Lan`/`P2p`/`Relay`/`Unknown` 视觉语义按 aki_ui_design 第 3 节）。
- `M5-05` Conversations 页与聊天窗口（`SCOPE-05`/`SCOPE-06`/`SCOPE-07`
  UI 面 + `SCOPE-08` 会话内文件卡片）：会话列表、消息历史
  （`virtuallist`，行高模型按 `M5-01` 复核结论）、文本发送、图片发送
  （文件对话框只读选取→hash-first 发起链路，`DEC-010`/`DEC-011`）、消息
  气泡/图片预览（`dialog`）、输入区、会话内文件卡片（进度组件与 Transfers
  页复用，`aki_ui_design` 第 4 节）。
- `M5-06` Transfers 页（`SCOPE-08` Transfers 页面）：传输任务集中列表与
  操作面（暂停/恢复/取消；含无会话行 `cancel_transfer` 直接终态入口
  （`DEC-013`⑥）与孤儿接收行 re-push 触发面登记）；接收根残留 GC 议题
  处置（`DEC-012`③ 登记的 M5 GC 议题：实现或显式延后并登记触发条件）。
- `M5-07` Settings 页与主题（`SCOPE-12`）：主题三选（跟随系统/浅/深——
  无系统主题检测 API 缺口按 `DEC-005` 处置：Aki 平台层查询或先交付浅/深
  两档并如实登记）；最小设置项（数据目录展示等）。
- `M5-08` MVP 全链路验收（设计第 15 节 / `SCOPE-01`~`SCOPE-12` 逐项）：
  双端真机链路（与 M3/M4 退出补跑同批环境）；文本/图片/文件 + 进度 +
  暂停/恢复/取消 + 历史/传输行重启恢复 + 断线恢复原会话继续可用；MVP
  验收清单逐项归档；M3 登记的补做条件复核（占位口令 verifier/secret
  backend/DeviceIdentity 元数据——真实口令流程随 M5）。
- `M5-09` 收口审计与退出证据归集（沿用 M1-08/M2-08/M3-09/M4-07 纪律）。

### 非目标

- 语音/视频通话、远程终端、屏幕控制、设备命令、Agent（`SCOPE-13`~
  `SCOPE-17`、`POST-01`~`POST-03`）。
- 群组/多设备会话（`POST-04`）。
- Android/移动端（`POST-05`）。
- CI 内 UI 渲染自动化（GLFW+OpenGL 渲染不进 CI）：UI 逻辑（视图模型/
  状态映射/指令出站）以网络无关单测承载，compose/渲染层本机手工验证 +
  证据归档，如实登记限制（`RULE-11`）。
- 内容寻址去重、断点续传语义扩展（既有延后项不变）。

## 设计与决策依据

- [Aki 设计方案](../design/aki_design.md)第 9 节（GUI 三栏与四页导航）、
  第 10 节（状态管理与线程边界）、第 14 节（`ui/` 目录与依赖方向）、
  第 15 节（第一阶段范围与 MVP 清单）。
- [Aki UI 设计规范](../design/aki_ui_design.md)：第 2 节（ZCode → EUI-NEO
  绑定映射：排印/间距圆角/语义色板/组件映射）、第 3 节（领域状态视觉
  语义）、第 4 节（页面与组件映射表）、第 5 节（落地与验证、启动前复审
  要求）；[ZCode Design System 原文](../design/zcode-design-system.md)。
- [DEC-005](../decisions/DEC-005-eui-neo-integration.md)（集成方式/构建
  开关/并发边界冻结/静态盘点与已知缺口）、[DEC-002](../decisions/DEC-002-layering-and-state-boundary.md)
  （分层与状态边界）、[DEC-008](../decisions/DEC-008-manager-routing-and-executor-tasks.md)
  （Manager 模式——UI 经 Application 出站面，不直触 Manager 内部）、
  [DEC-010](../decisions/DEC-010-image-message-contract.md)（图片消息面
  展示与发送链路）、[DEC-012](../decisions/DEC-012-receive-merge-bearing.md)/
  [DEC-013](../decisions/DEC-013-orphan-row-recovery.md)（传输终态语义/
  孤儿降级——Transfers 页操作语义依据）。
- M3 移交：发现来源分期（Relay/邀请链接/手动输入展示面）、占位口令
  verifier + secret backend + DeviceIdentity 元数据补做条件（M3 里程碑
  记录归档）；M4 移交：接收根残留 GC（`DEC-012`③）、孤儿接收行 re-push
  触发面、孤儿传输行/单侧到达 UI 兜底（M4-03/05/06 边角登记）。
- AGENTS.md Executor 强制规则与工程规范第 9 节；`EXEC-01`（onShutdown
  关闭序）/`EXEC-02`（第三方回调隔离——EUI-NEO 回调若有，同样只投递）/
  `EXEC-03`（DoubleBuffer 快照消费）/`EXEC-06`（可观测）/`EXEC-07`（句柄
  所有权）。

## 工作项

（条目的完整内容与边界见「范围与非目标」各 `M5-NN` 条；此处为勾选跟踪与
可验收结果锚点。）

- [x] `M5-01` 设计先行与运行复核（可验收：aki_ui_design 第 5 节一致性复审
  记录落档；EUI-NEO 最小运行探针实测结论——`RISK-2026-002` 运行复核收口；
  UI 装配契约固化进设计第 9 节；偏差先更新设计/决策再合代码）。
  （2026-09-26 完成：§5 一致性复审逐项落档（16 组件存在一致；§2.1 排印/
  §2.2 圆角尺寸默认档偏差确认→覆写清单；§2.4 深度锚点修正
  fieldVisuals().popupShadow；间距一致零覆写）；运行探针三项复核收口——
  virtuallist 固定行高模型实测确认（rowHeight 统一值锚点 + 渲染通过，
  变高气泡按组合策略承接）、dialog/toast 页面持有 + requestUpdate 唤醒
  重组实测（waker 原型 11 次开合转换全部拾取——EXEC-03 契约原型）、文件
  对话框只读满足发送选取（接收侧按接收根无需求）；**组合模型发现**：compose
  为保留模式事件触发（静态 UI 不重组，探针 12s 仅 2 次 compose）——装配
  契约关键输入；设计 §9.1「UI 装配契约」固化（视图模型派生/快照消费与
  唤醒/onShutdown 关闭序/主题覆写清单/渲染层验证策略）。详见验证记录。见
  下方 2026-09-26（M5-01）验证记录。）
- [x] `M5-02` EUI-NEO 接入与主窗口骨架（可验收：configure 三方校验通过、
  `aki_ui` 实体化构建图生效、三栏壳 + 四页导航可运行、`ui/theme` 逐项覆写
  留对照、`onShutdown` 关闭序经关闭路径验证）。
  （2026-09-27 完成：单一构建图接入（八项开关 CACHE FORCE，configure 通过，
  bundled 八件套零联网）；`aki_ui` 实体化（eui::neo 仅由 aki_ui 链接，测试
  exe 不链 eui 经 dumpbin 核查 0 命中）；宿主入口迁移 dslAppConfig()+compose()
  （/SUBSYSTEM:WINDOWS）——组合根抽离为 EUI-NEO 无关的 HostRuntime
  （app/lifecycle/host_runtime，设计 §9.1 首帧装配例外条款先行落档），
  首次 compose 惰性装配 + onShutdown 薄委托受控关闭；三栏壳 + 四页导航
  本机可运行（截图+日志证据归档）；ui/theme akiTheme() 逐项覆写 + 双份
  回归对照（test_ui_theme_values 独立抄录期望值 + GUI 运行日志对拍）；
  onShutdown 关闭序经 test_host_runtime 关闭路径验证（DOD-02 六项 +
  §8.3 钩子原序断言）+ GUI 会话日志（8 步钩子序 + fully_stopped）；debug/
  release 全量 ctest 40/40 零回归；DEC-014 落档（tsan 全图插桩 + CI 依赖
  集，覆盖声明五条）；DEC-005「影响与风险」验证项逐项回填。见下方
  2026-09-27（M5-02）验证记录。）
- [x] `M5-03` 状态消费面与视图模型（可验收：快照排空 + 跨线程唤醒路径经
  单测——机制契约面：注入回调于 executor 任务内的 owner drain 中触发，
  现行管线 drain 仅在主线程；四域视图模型派生有断言，UI 操作全经
  Application 出站面——边界由退出-3 grep 与单测共同锁定）。
  （2026-09-27 完成：`ui/models` 拆分为 EUI-NEO 无关独立目标 `aki_ui_models`
  （四域视图模型纯函数派生 + `consume_ui_state` 水位消费面 + `UiActions`
  注入出站面，测试 exe 直链——DEC-005「测试 exe 不链 eui」落地面）；发布点
  唤醒回调 `AppStateOwnerOptions::on_publish`（owner 上下文、publish 后同步
  调用、异常全捕获计数）经 `HostRuntime::ensure_assembled` 装配参数注入，
  GUI 宿主传 `app::requestUpdate()`（本机日志留首触发证据）；main_window
  占位页最小接线（四域计数消费展示 + Devices 页发现启停出站示范）。新单测
  test_ui_models（派生/水位去重/路径 mailbox 独立推进/发布→唤醒调用序/钩子
  异常收口/executor 任务内驱动 drain 的回调契约——机制契约面：注入回调于
  executor 线程触发，现行管线 drain 仅在主线程，78 断言）+
  test_ui_actions（页面→出站接口→Manager 泵→Fake Adapter SPI 通道，
  51 断言）；debug/release 全量 ctest 42/42 零回归；退出-3 grep 扩面全 0。
  见下方 2026-09-27（M5-03）验证记录。）
- [x] `M5-04` Devices 页（可验收：`SCOPE-04`/`SCOPE-02`/`SCOPE-03`/
  `SCOPE-10` 展示面逐项可演示——列表、信任操作面、presence/连接路径徽标）。
  （2026-09-27 完成：SPI 信任操作扩展（出站 `confirm_pairing`/`revoke_trust`
  + 入站第 12 方法 `on_pairing_completed`，DEC-006 映射 3 落地，真实/Fake
  对齐）；DeviceManager 信任三操作 + 配对结果路由（§4 固定转移边校验，
  非法转移拒绝可见）；[DEC-015](../decisions/DEC-015-per-device-connection-path.md)
  逐设备路径落地（退役全局摘要；初连路径补发、断连置 Unknown）；
  UiActions 信任三操作；Devices 页实体化（列表行/信任操作面/确认弹窗 mono
  指纹/presence·路径·信任徽标语义色/发现来源分期披露）；[DEC-016]
  (../decisions/DEC-016-pairing-password-verifier.md) 口令常量 + 真实
  verifier（取代 M3-03 占位假编码串，存量 profile 处置登记）；
  test_device_trust 76 断言 + 口令往返用例；debug/release 全量 ctest 43/43
  零回归；Devices 页本机截图归档。见下方 2026-09-27（M5-04）验证记录。）
- [x] `M5-05` Conversations 页与聊天窗口（可验收：`SCOPE-05`/`SCOPE-06`/
  `SCOPE-07` UI 面 + 会话内文件卡片逐项可演示——含文本/图片发送与消息
  历史滚动）。
  （2026-09-27 完成：Conversations 页实体化——会话列表（复用 M5-03
  ConversationView 派生 + 最后消息摘要/方向/投递徽标扩展）+ 聊天窗口
  （头部信任/路径徽标、§3 断连横幅、变高气泡列按「卡片自绘 +
  scrollview」承接（M5-01 复核结论实测成立）、文本/图片消息气泡与
  文件卡片（进度语义色，M5-06 Transfers 页复用形态）、输入区、图片
  预览弹窗、New chat（Trusted 设备选择）弹窗）；发送面页面形改造——
  wire id 生成收敛 `heyaki/adapter/wire_ids.hpp`（`NodeSession::
  new_transfer_id` 委托同入口，§6.1 同步）+ UiActions 增 id 生成器注入/
  send_image 绑定 hash-first 编排（DEC-010/DEC-011）；视图模型扩展
  MessageView（方向/投递态/媒体载荷 + TransferStore 消费侧 join，无传输
  行单侧到达兜底态）；唤醒缺口修复（本机 GUI 实测）——owner 增
  `on_update_submitted` 受理点唤醒钩子（§9.1 同步），发送结果不再滞留至
  下一次输入事件；文本发送 GUI 实测（real Adapter 无会话拒绝 → Failed
  徽标可见 + 列表摘要实时更新）；新断言 test_ui_models 116 + test_ui_
  actions 65；debug/release 全量 ctest 43/43 零回归；本机 GUI 截图 ×5 +
  运行日志归档（RULE-11）。见下方 2026-09-27（M5-05）验证记录。）
- [x] `M5-06` Transfers 页（可验收：传输集中列表与暂停/恢复/取消操作面
  可演示；接收根残留 GC 议题实现或显式延后并登记触发条件）。
  （2026-09-27 完成：Transfers 页实体化（`ui/pages/transfers_page`）——
  传输行=文件卡片共享形态（M5-05 ③ 抽出为 `ui/components/transfer_card`，
  §4 复用契约兑现；形状与语义色不变）+ 操作面 Pause/Resume/Cancel 按
  TransferView 状态门控派生（§7 固定边）经 UiActions 既有传输三接口；
  Paused 行 Cancel = DEC-013⑥ 无会话行直接终态入口 UI 触达（GUI 实测
  Paused 行经 Cancel 即转 Cancelled 中性态；admission 反馈可见性原证据
  存在页脚叠印缺陷，修正后 GUI 复验——见验证记录⑩）；孤儿
  接收行 re-push 触发面=页脚登记披露；接收根残留 GC 议题处置=显式延后
  （aki_design §11.1 同步，触发条件三则登记于本记录）；TransferView 增
  mime_type 透传；新断言 test_ui_models 135 + test_ui_actions 67；
  debug/release 全量 ctest 43/43 零回归；GUI 截图 ×2 + 运行日志归档
  （RULE-11）。见下方 2026-09-27（M5-06）验证记录。）
- [x] `M5-07` Settings 页与主题（可验收：主题三选按 `DEC-005` 缺口处置
  落地并如实登记，最小设置项可演示）。
  （2026-09-27 完成：Settings 页实体化——主题三选（跟随系统/浅/深）
  segmented（§4 映射）页面持有 UI 态 + 经 akiTheme()/akiSemanticColors()
  装配面生效（覆写清单/回归对照不变）；DEC-005 缺口走平台层查询路径——
  `app/lifecycle/system_theme` 平台条件编译单元（Windows
  AppsUseLightTheme；公开面仅 std 枚举 RULE-10；其余平台 Unknown 回落
  Light 页内披露）；主题选择会话级（schema v1 无设置表，不私自扩表，
  持久化登记为后续项）；最小设置项=数据目录（HostRuntime 装配面）+
  本地设备 id（快照）mono 只读展示；resolve 状态机抽 EUI-NEO 无关
  ui/theme/theme_mode.hpp（网络无关单测，test_ui_theme_values 62 断言）；
  debug/release 全量 ctest 43/43 零回归；深浅两档渲染对照截图 ×3 +
  运行日志归档（RULE-11）。见下方 2026-09-27（M5-07）验证记录。）
- [x] `M5-08` MVP 全链路验收（可验收：设计第 15 节清单 + `SCOPE-01`~
  `SCOPE-12` 逐项归档；M3 登记补做条件复核闭环；环境受限沿降级纪律）。
  （2026-09-27 完成（验收归集类，无产品代码变更）：SCOPE-01~12 逐项
  归档两态标注（本机已验证/双端待补跑）——证据锚点映射 M1~M4 里程碑
  验证记录与 M5-01~07 GUI 实测；验收基线复跑 debug/release 全量 ctest
  各 43/43（干净树）；本机补证：发现启停 GUI 实拍（start_discovery
  admitted）+ 文本发送/传输终态/主题设置引用既有归档；**发现一项 GUI
  缺陷**（Devices 页 Pending 行 Confirm 点击无响应，复现 2 新会话 ×3
  点击，相邻按钮正常——登记独立修复工作项，SCOPE-03 信任确认弹窗 GUI
  验收被该项阻塞，网络无关半边由 test_device_trust 承载）；M3 登记
  复核闭环：口令 verifier 闭合（DEC-016）/secret backend 再登记/
  DeviceIdentity 元数据再登记/kAkiPairingPassword 维持（DEC-016 移除
  条件未触发）；双端项显式降级（原因/负责人/补跑条件，退出-1 保持
  未勾选）。见下方 2026-09-27（M5-08）验证记录。）
- [ ] `M5-09` 收口审计与退出证据归集（可验收：审计记录 + 退出-1~5 证据
  齐备；工作项全部完成不自动关闭里程碑——关闭以收口审计为准）。

## 风险与阻塞

- **`RISK-2026-002` 运行复核（Mitigated → 收口）**：静态盘点 16 组件全部
  存在，运行复核挂 `M5-01`——virtuallist 固定行高模型对变高气泡列的适配、
  dialog/toast 页面持有 open 状态与单向数据流的配合、文件对话框只读能力
  对发送链路的满足度，以实测为准；缺口处置优先原语组合，确需上游能力或
  贡献时按工程规范以决策记录确认，不得修改 pinned 依赖。
- **防火墙/LAN 双端环境限制**：M3/M4 退出-1/3 补跑与 `M5-08` MVP 双端验收
  同批约束——沿 M3-09/M4-07 降级纪律（网络无关断言拆分、[skip] 显式 +
  补跑条件），不冒充已验证；补跑由负责人执行。
- **无系统主题检测 API**（`DEC-005` 已知缺口）：Settings「跟随系统」需
  Aki 平台层查询或先交付浅/深两档；如走平台层查询需平台条件编译单元
  （`RULE-10` 公开面仅 std 类型）。（M5-07 处置收口：走平台层查询路径——
  `app/lifecycle/system_theme`（Windows `AppsUseLightTheme`；公开面仅
  std 枚举）；非 Windows 平台/查询失败 Unknown 回落 Light 并页内披露，
  跨平台系统主题扩展（如 XDG portal，需 dbus 依赖）出现需求时再立决策。）
- **CI 无显示环境**：GLFW+OpenGL 渲染不进 CI——UI 逻辑层网络无关单测 +
  渲染层本机手工验证证据归档；不在 CI 宣称的检查不得写入 CI 断言
  （`RULE-11`）。
- **默认主题档位 ≠ aki_ui_design 第 2.1 节表**（`DEC-005` 静态盘点结论）：
  `ui/theme` 装配须逐项覆写并留回归对照，不得依赖上游默认档。
- **主线程 compose 与 executor 关闭顺序**：`onShutdown` 编入 `EXEC-01`
  后，窗口/GPU 设备销毁与 worker 回收次序需测试覆盖（关闭路径 DOD-02）。
- **UpsertDevice 整行替换对易失字段的覆写隐患**（`DEC-015` 关联发现的
  登记兑现，2026-09-27 收口批）：`apply_trust_transition`
  （app/application/device_manager.hpp:230-252）从最近已发布快照读-改-写
  整行 upsert——快照滞后于 owner 在途更新时可能覆写并发 presence 变化
  （连接中被重新发现可能 Online→Offline）；wire 节奏的
  PairingCompletedWork 结果被拒时仅计 handler_rejections 即丢弃（无延迟
  重排）。M5-04 按「用户操作节奏吸收」接受（记录②），后续工作项：结果
  丢弃时延迟重排或 drain 后重读，与该覆写隐患同批处置。
- pinned EUI-NEO 升级不属本里程碑；许可证检查（字体/图标/shader/assets）
  在正式发行前完成（设计第 9 节），登记于 `M5-09` 复核项。

## 测试与退出条件

- [ ] 退出-1：MVP 全链路双端验收——设计第 15 节清单逐项 +
  `SCOPE-01`~`SCOPE-12` 全边界（发现→信任→会话→文本/图片/文件→进度→
  暂停/恢复/取消→重启恢复→断线恢复）；环境受限时按 M3-09/M4-07 先例
  「部分验证 + 如实降级声明」处置并登记补跑条件。
- [ ] 退出-2：DOD-02 六项沿 UI 新增并发路径通过——正常完成、任务异常、
  提交拒绝、执行中取消、超时、shutdown（含 `onShutdown` 关闭序与跨线程
  唤醒路径）；UI 逻辑层网络无关单测通过。
- [ ] 退出-3：边界锁定——`RULE-01`/`RULE-02`（页面代码不持有 transport
  对象、UI 只消费状态变化）、`RULE-10`（公开面无 EUI-NEO/平台类型）、
  `DEC-005` 并发边界 grep（无 `app::async`/`core::network`/`audio` 引用、
  无自建线程）通过。
- [ ] 退出-4：debug/release 构建与全量测试通过；ASAN/UBSAN/TSAN 随 CI
  门禁；渲染层本机验证证据归档（截图/运行日志），不适用工具链记录限制
  与补跑条件。
- [ ] 退出-5：设计（第 9 节细化/第 14 节）、决策（UI 相关缺口如立新决策）、
  aki_ui_design 复审记录、总计划与里程碑状态同步；验证记录含可复现命令；
  环境受限项降级声明完整。

（自 `M5-01` 起按工程规范 6.1/6.3 追加。）

## 验证记录

- 2026-09-26（`M5-01` 完成；Windows 11 工作站（桌面会话）/ MSVC 2022
  BuildTools 14.44.35207 / CMake 4.1.0；负责人：Linductor；纯文档 + scratch
  探针，无产品代码——探针位于 `build/scratch/m5-probe/`（gitignored），
  standalone CMake 不入产品构建图，只读消费 pinned `third_party/EUI-NEO`
  @ `b9032a8a`）：
  - **① aki_ui_design §5 一致性复审**（逐项结论已回填
    [aki_ui_design](../design/aki_ui_design.md) §4/§5）：
    - 绑定映射 16 组件在 pinned v0.6.0 全部存在（components/components.h
      伞头 + components/ 目录 37 头文件逐一对照）——一致。
    - §2.1 排印偏差确认：`TypographyTokens` 默认档（theme.h:11-23：micro
      11/caption 12/hint 13/label 14/body 16/subtitle 20/title 22）≠ 本表
      （ui-2xs 9/ui-xs 10/ui-sm 12/ui-caption 13/ui-base 14/ui-lg 16/
      ui-xl 18）→ `ui/theme` 逐项覆写（清单入设计 §9.1）。
    - §2.2 间距一致（SpacingTokens tiny 4/compact 8/content 12/section 16/
      large 20/panel 24，theme.h:33-39——探针对拍零覆写）；圆角/尺寸部分
      偏差（radius.small 6→4、control.field 35→36、menuItem 34→28；
      theme.h:47-59）→ 覆写。
    - §2.4 深度锚点复核（2026-09-26 评审纠正）：v0.6.0 **存在**
      `theme::panelShadow`/`popupShadow` 独立函数（theme.h:266/:270，
      自上游 d28609fb 2026-04-28 即在）——本记录先前「无独立函数」断言
      与 pinned 源不符、已撤回；`fieldVisuals().popupShadow*`
      （theme.h:126-128/:211-215）为其合成字段、锚点属实。aki_ui_design
      §2.4 落点已恢复为 panelShadow/popupShadow（弹层与 Toast）。
  - **② 运行探针实测**（standalone CMake + `eui_neo_configure_app`，
    DEC-005 冻结开关 CACHE FORCE；configure 通过 42.9s、Debug 构建通过；
    日志证据 `m5-probe.log` 落盘 + flush，探针由外部超时终止——无编程式
    关窗 API，onShutdown 路径归 M5-02 关闭路径测试，如实声明）：
    - 窗口启动 + compose 循环：GLFW+OpenGL 窗口打开、首帧 compose、
      画面尺寸 720x520 落盘。
    - 主题逐项覆写实测（对拍落盘）：`typography.title 22→18 subtitle
      20→16 body 16→14 caption 12→13 hint 13→12 micro 11→10 | radius.small
      6→4 | control.field 35→36 menuItem 34→28`；间距六档与默认一致零
      覆写——与静态盘点结论吻合。
    - **RISK-2026-002 三项复核收口**：
      ① virtuallist 固定行高模型**实测确认**——`rowHeight(float)` 统一
      行高（virtuallist.h:35）、行定位 `index * rowHeight`（:148），变高
      内容被固定行高裁切；200 行探针数据渲染通过。M5-05 布局策略：消息
      气泡列按「卡片自绘 + scrollview」或行内分段组合承接（原语组合纪律），
      不修改 pinned 依赖。
      ② dialog/toast 页面持有状态**实测确认**——`dialog.open(bool)`
      （dialog.h:54）、`toast.visible(bool)+bindVisible(Signal)`
      （toast.h:43-47）；探针以页面持有原子状态 + 边沿检测驱动开合，
      requestUpdate 唤醒下 13s 内 11 次开合转换全部被重组拾取（日志
      transition 序列完整）——与单向数据流配合成立（页面模型持状态，
      compose 只读派生）。
      ③ 文件对话框只读能力**满足发送选取链路**——
      `eui::platform::openFileDialog`（platform.h:13；平台能力文档：支持
      多选/扩展名过滤、不支持目录/保存 :146）：图片发送 open 选取 →
      hash-first 发起链路成立；接收侧按接收根落盘无对话框需求。
    - **组合模型发现（装配契约关键输入）**：compose 为**保留模式、事件
      触发**——静态 UI 不重复重组（对照实验：无状态变化时 12s 仅 2 次
      compose）；跨线程 `app::requestUpdate()` 唤醒 → 主线程重组拾取页面
      持有状态新值（waker 原型：非主线程线程翻转原子状态 + requestUpdate，
      每次唤醒均触发重组并拾取）。此结论固化进设计 §9.1（快照消费与唤醒
      条款）。
  - **③ UI 装配契约固化**：设计 §9.1「UI 装配契约（M5 契约，M5-01）」
    五条——视图模型派生（四域纯函数派生 + 操作经出站面）、快照消费与唤醒
    （DoubleBuffer 主线程排空 + requestUpdate 跨线程唤醒 + compose 三不
    纪律）、onShutdown 关闭序（EXEC-01 序编入钩子 + 关闭路径测试归
    M5-02）、主题档位覆写清单（逐项数值 + akiTheme() 装配归 M5-02）、
    渲染层验证策略（RULE-11：渲染不进 CI + 本机证据归档口径）。实现偏差
    先更新本节再合代码（M1-08）。
  - 验证命令（可复现）：探针 configure
    `cmake -S build/scratch/m5-probe -B build/scratch/m5-probe/build -G
    "Visual Studio 17 2022"`；构建 `cmake --build
    build/scratch/m5-probe/build --target m5_probe --config Debug`；运行
    `cd build/scratch/m5-probe/build/Debug && ./m5_probe.exe`（日志
    `m5-probe.log` 随帧落盘；探针无自退出，验证时以超时终止收集）。
  - 限制与补跑条件：探针为 Debug 单配置、kill 终止（onShutdown 未在本
    探针触发——关闭序归 M5-02 关闭路径测试）；virtuallist 裁切行为与
    dialog/toast 视觉效果为日志+代码锚点证据，像素级视觉复核归 M5-05/07
    本机手工验证（截图归档口径，退出-4）。无产品代码变更：`git status`
    复核仅 docs/design/aki_ui_design.md、docs/design/aki_design.md、
    docs/plans/{m5,aki-implementation-plan}.md 与 scratch（gitignored）。
  - 同步：本里程碑（M5-01 勾选、状态 In Progress、本记录）、
    [aki_ui_design](../design/aki_ui_design.md)（§2.4 修正 + §4 注 + §5
    复审记录）、[aki_design](../design/aki_design.md) §9.1（装配契约）、
    总计划（当前状态条目 + M5 索引 In Progress）。

- 2026-09-27（`M5-02` 完成；Windows 11 工作站（桌面会话）/ MSVC 2022
  BuildTools 14.44.35207 / CMake 4.1.0；负责人：Linductor）：
  - **① 依赖接入（DEC-005/DEC-003）**：submodule `third_party/EUI-NEO` @
    `b9032a8a848f8d8cf096bb7711c626f9c051e0ce`（v0.6.0，`git submodule
    status` 实测）+ 锁文件（M0 起已登记，本项无变更）；configure 三方校验
    属锁文件纪律的既有校验（Aki lock ↔ checkout，`cmake/Dependencies.cmake`
    pinned 分支）——debug preset configure 通过，日志含
    `Dependency 'EUI-NEO' pinned at b9032a8a…` 与
    `Pinned dependency verification passed`；bundled 八件套
    （glfw/glad/tray/freetype/zlib/libpng/md4c/miniaudio）全部
    `Using bundled … source` 零联网（DEC-006「不静默联网」先例口径，
    EUI_DEPS_MODE=bundled 冻结）。无独立 fetch 脚本需求：submodule 经
    `git submodule update --init`（CI checkout `submodules: recursive`），
    其 bundled 第三方随 pinned 源码树分发，无网络拉取环节。
  - **② 单一构建图**：根 CMakeLists.txt `add_subdirectory(third_party/
    EUI-NEO)` + 八项开关 CACHE FORCE（`EUI_DEPS_MODE=bundled`、
    `EUI_BUILD_APPS/EUI_BUILD_USER_APPS/EUI_BUILD_TEST_FIXTURES`、
    `EUI_ENABLE_INSTALL/EUI_ENABLE_MODULES` 全 OFF、`EUI_WINDOW_BACKEND=
    glfw`、`EUI_RENDER_BACKEND=opengl`；EUI_ENABLE_TRAY 保持上游默认 ON，
    DEC-014）+ `eui_neo_configure_app(aki)`（框架 main 注入，
    /SUBSYSTEM:WINDOWS 实证——aki.exe 无控制台输出、GUI 会话正常开窗）。
    eui_neo 头文件 SYSTEM 标注（IDE 生成器限制同 executor 先例）+ 框架
    注入源 glfw_app_main.cpp 源级 /W0（上游告警不进第一方 /WX 门禁）。
  - **③ aki_ui 实体化**：ui/CMakeLists.txt 静态库（theme/aki_theme.cpp +
    pages/main_window.cpp），`eui::neo` 仅由 aki_ui 链接（RULE-01/RULE-10；
    eui include 全仓仅存在于 ui/ 四文件与根 main.cpp，grep 实证）；测试
    exe 不链 eui（tests/CMakeLists.txt 零 aki_ui 链接；aki_host_smoke.exe
    `dumpbin /SYMBOLS` 对 eui::/components::/glfw 符号 0 命中，console
    约定保持，DEC-005）。宿主组合根抽离为 `aki_host` 静态库
    （app/lifecycle/host_runtime.{hpp,cpp}，EUI-NEO 无关——GUI 钩子、
    console 驱动 aki_host_smoke、关闭路径测试三方共享）。
  - **④ 宿主入口迁移 + 启动/关闭配对**：根 main.cpp 重构为
    dslAppConfig()（纯配置，const 查询语义）+ compose() 钩子；启动触发点 =
    首次 compose 主线程惰性 `HostRuntime::ensure_assembled()`（设计 §9.1
    增补「首帧装配例外」与「启动↔关闭配对」两条款先行落档——M1-08 纪律，
    compose 三不纪律的唯一显式例外）；onShutdown 薄委托
    `shutdown_with_report()`（§8.3 钩子原序 + EXEC-01 步骤 2~5）。
  - **⑤ 三栏壳 + 四页导航**：ui/pages/main_window.{hpp,cpp}——导航栏
    (fixed 64) + 列表栏 (fixed 264) + 内容栏 (fill)，Conversations/Devices/
    Transfers/Settings 路由占位（页面模型持 UI 态，compose 只读派生）。
    本机可运行证据：aki.exe（Release）GUI 会话——`aki-run.log`
    （build/release/Release/，随帧落盘）：装配 ok 32/54/60ms（含恢复路径
    identity=loaded devices=1）、主题对拍落盘、screen 1080x720；
    截图 `build/scratch/aki-m5-02-window.png`（三栏壳 + 四页导航 + light
    主题渲染；右下 Windows 防火墙弹窗为系统级提示——aki_host_smoke ctest
    运行触发，非应用内容，属 M3/M4 已登记防火墙环境约束）；taskkill
    WM_CLOSE 优雅关窗 → onShutdown 完整执行。复现：`cmake --build
    --preset release --config Release && cd build/release/Release &&
    ./aki.exe`（关闭：正常点窗叉或 `taskkill /IM aki.exe` 优雅 WM_CLOSE）。
  - **⑥ ui/theme 逐项覆写 + 回归对照**：数值权威表
    ui/theme/aki_theme_values.hpp（EUI-NEO 无关）+ akiTheme()/akiSemantic
    Colors() 装配（Typography title 22→18 subtitle 20→16 body 16→14
    caption 12→13 hint 13→12 micro 11→10（label 14 保持）；Radius small
    6→4（card/overlay/control 锚定）；ControlSize field 35→36 menuItem
    34→28；间距六档零覆写锚定；§2.3 语义色板深浅两套 + 扩展语义色）。
    回归对照双份：tests/unit/test_ui_theme_values.cpp（设计文档独立抄录
    期望值 + 预混合公式复核，不链 eui）+ GUI 运行日志「上游默认 → 覆写值」
    对拍行（M5-01 探针同款）。
  - **⑦ onShutdown 关闭路径测试（DOD-02）**：tests/unit/
    test_host_runtime.cpp（console exe，链 aki_host 不链 eui）——六项沿
    宿主生命周期路径：正常完成（submit_auto 返回值 + Manager 泵 + 真实发现
    启停 + 快照本地身份行）、任务异常（future 上浮 + task_exception_count）、
    执行中取消（submit_cancellable + request_task_cancel 协作退出 +
    CancellationStatus）、提交拒绝（set_max_in_flight_tasks(1) 耗尽 →
    CapacityExhaustedException 即时就绪 + capacity_exhausted_count）、
    超时（completion_wait 预算耗尽如实记录——超时非干净关闭
    fully_stopped=false 但 EXEC-01 步骤 5 仍收敛 Stopped；config 级排队软
    超时已由 test_app_managers 沿泵路径覆盖）、shutdown（§8.3 钩子原序
    hook_sequence 8 步逐项断言 + 两 blocking worker 2/2 回收 + 写路径
    admit==completed 零丢失 + 幂等 + 关闭后提交显式拒绝）。窗口/GPU 销毁与
    worker 回收次序：worker 回收在 onShutdown 钩子内（EXEC-01 步骤 2/3）
    完成，GPU 设备销毁（renderBackend.reset()）在钩子返回后（框架
    glfw_app_main.cpp:594-604）——次序由 test_host_runtime（钩子内回收
    断言）+ GUI 日志（shutdown 返回行后框架收尾）双重承载。
  - **⑧ 单图符号冲突核对（DEC-006 sqlite 先例）**：dumpbin /SYMBOLS——
    eui_neo.lib 含 heyaki 栈符号（sqlite3_/usrsctp/rtc::）0 命中；
    heyaki 侧 lib 含 eui 栈符号（glfw/freetype/FT_Init/md4c_/stbi__）
    0 命中；Release 全量链接日志 LNK4006/重复符号 0；aki.exe
    /DEPENDENTS = 系统 DLL + libssl/libcrypto（OpenSSL 随宿主部署），
    全静态单 exe。
  - **⑨ assets 就位**：build/release/Release/assets/（icon/fonts/shaders
    等随 eui_neo_configure_app POST_BUILD copy 生成，GUI 会话消费实证——
    窗口图标与文本渲染正常）。
  - **⑩ CI 扩面（DEC-014）**：ci.yml Linux 四档安装 pinned 集成指南完整
    依赖集（libssl/libcurl4-openssl/libgl1-mesa/libegl1-mesa/libx11/
    libxext/libxrandr/libxinerama/libxcursor/libxi/libwayland/
    wayland-protocols/libxkbcommon/libglib2.0-dev；tray/Wayland 不降档）；
    `aki_apply_warnings` GNU+tsan `-Wno-error=tsan` 豁免（heyaki 先例同款，
    限定编译语言）；tsan 对 eui 栈全图插桩零豁免（DEC-014 机制探针 +
    覆盖声明五条）。**CI 门禁全绿证据未在本会话产生**（不建分支/不推送
    纪律）——随 M5-02 PR 首跑核实，如实登记。
  - **⑪ MinGW 受限（纪律记录）**：Aki CMakePresets 无 MinGW 档（M3-01 起
    w64devkit 受限，configure-only 未达成先例）；EUI-NEO 硬性要求
    GCC≥12 且 static runtime 探测失败即 FATAL（其 CMakeLists.txt:98-103）
    ——MinGW 被 EUI-NEO 阻断属 DEC-005 预期，本项不建立 MinGW preset、
    不冒充验证。
  - **⑫ 全量测试零回归**：debug 与 release 全量 ctest 各 40/40 通过
    （原 38 项语义零损失迁移——skeleton.app_runs/smoke.device_lifecycle/
    recovery.corrupt_db_clean_failure 改由 aki_host_smoke 驱动（同 argv
    契约、同输出标记），+test_host_runtime +test_ui_theme_values 两新项）。
    命令：`ctest --preset debug`（151.5s，100% passed 40/40）、
    `ctest --preset release`（156.3s，100% passed 40/40）。
  - **⑬ 边界 grep（退出-3）**：第一方线程创建（std::thread/jthread/async/
    CreateThread/pthread_create）0 命中（唯一命中为 test_app_state 注释
    行）；ui/+main.cpp 的 app::async/core::network/eui::network 0 命中；
    EUI-NEO include/类型越过 ui/（app/device/conversation/transfer/
    persistence/heyaki）0 命中。
  - **⑭ 设计与决策同步**：设计 §9.1 增补「首帧装配例外」「启动↔关闭配对」
    两条款（M1-08 先行）；[DEC-014](../decisions/DEC-014-eui-tsan-coverage.md)
    落档（Accepted）；DEC-005「验证方式」逐项回填。
  - 限制与补跑条件：CI Linux 四档门禁与 tsan 首跑证据随 M5-02 PR（本会话
    未推送）；Linux 真实 GCC tsan 编译/运行与 UI 运行期 tsan 证据按
    DEC-014 口径归后续会话（本机无 Linux 工具链）；非空库 GUI 装配耗时
    随 M5-08 双端验收复测；GUI 数据根为真实用户目录（%APPDATA%\aki，
    本机 GUI 会话已建立 identity/devices=1——应用正常形态，测试注入经
    HostRuntime 参数/aki_host_smoke argv 承载）。
  - 同步：本里程碑（M5-02 勾选、本记录）、
    [aki_design](../design/aki_design.md) §9.1（两增补条款）、
    [DEC-014](../decisions/DEC-014-eui-tsan-coverage.md)（新建）、
    [DEC-005](../decisions/DEC-005-eui-neo-integration.md)（验证方式回填）、
    总计划（当前状态条目 + 决策表 DEC-014）。

- 2026-09-27（`M5-03` 完成；Windows 11 工作站（桌面会话）/ MSVC 2022
  BuildTools 14.44.35207 / CMake 4.1.0；负责人：Linductor）：
  - **① 视图模型派生（设计 §9.1）**：`ui/models/view_models.{hpp,cpp}`
    纯函数四域派生——设备列表（DeviceStore × presence + 连接路径摘要入参；
    Store 无逐设备路径字段，摘要取 owner LatestMailbox 最新单值，逐设备
    路径如需扩展先立 DEC——头文件内登记）、会话列表（ConversationStore ×
    最后消息摘要：端点归属过滤 + Store 序倒序取最新，媒体预览带
    "[image] name" 标注）、消息流（MessageStore 按会话端点过滤 + 本地身份
    端点守卫，可见空态）、传输列表（进度 fraction（total==0 → 0 不除零）/
    方向/终态标志）。`aki_ui_models` 为 EUI-NEO 无关独立静态目标（纯
    std/aki 类型；ui/CMakeLists.txt；依赖方向 ui/models → Application，
    RULE-01），测试 exe 直链（DEC-005「测试 exe 不链 eui」落地面）。
  - **② 快照消费面（EXEC-03）**：`ui/models/ui_state_consumer.{hpp,cpp}` —
    `consume_ui_state(owner, watermark, view)` 单次有界消费：快照
    `load_snapshot_newer_than` 水位去重 + 连接路径
    `try_load_connection_path_newer_than` 独立水位（SetConnectionPath 不
    触发 Store 发布，路径推进不依赖快照水位——回填设备视图摘要）；新快照
    为提交点（先落水位再派生，无半更新视图）；槽位忙/无新数据返回 false
    留待下帧（覆盖式语义丢帧不丢状态）。水位与派生结果
    （UiConsumerWatermark/UiStateSnapshot）存页面模型（§9.1「页面持有
    UI 态」）；宿主在 compose 前调用 `HostRuntime::pump_state()`（主线程
    = owner 上下文的有界 drain，无等待无 IO）后再 consume——compose 内
    不等待、不轮询、不做 IO 保持。
  - **③ 跨线程唤醒接线（设计 §9.1）**：`AppStateOwnerOptions::on_publish`
    （`std::function<void()>`，EUI-NEO 无关，RULE-10）——发布点为
    `publish_if_dirty` 的 `snapshot_.publish()` 之后、owner 单写者上下文
    同步调用；异常全捕获计数 `publish_hook_failures`（不中断 drain，
    `publish_hook_calls` 成功计数——RULE-09/EXEC-06 可观测）；经
    `HostRuntime::ensure_assembled(data_root, wake)` 装配参数转发注入；
    GUI 宿主 main.cpp 传包装回调（首次触发留日志 +
    `app::requestUpdate()`），console/测试宿主传计数器。设计 §9.1 消费面
    装配细节先行增补（M1-08：aki_ui_models 目标/consume 水位/on_publish
    注入/UiActions 四点）。
  - **④ UI 出站面（DEC-008）**：`ui/models/ui_actions.{hpp,cpp}` —
    `UiActions` 注入接口（send_text/send_image、传输四接口、发现启停、
    ensure_conversation 绑定面）+ `make_ui_actions` 组合根绑定（直呼
    Manager 公开出站方法，返回值即泵入队 admission，拒绝可见）；页面只持
    UiActions 不持有 Manager/transport 对象（RULE-01/RULE-02）。信任判定
    操作未预建（DeviceManager 现无对应方法——grep trust/reject/revoke
    零命中，归 M5-04 补建，设计 §9.1 登记）。
  - **⑤ main_window 最小接线**：列表栏计数消费展示（"N devices · N
    conversations · N transfers"，派生自最近消费快照）；Devices 页
    Start/Stop Discovery 出站示范按钮（admission 结果写页面模型反馈文案
    ——页面持有 UI 态形态）；内容栏提示 "state consumption wired
    (M5-03)"。
  - **⑥ 测试**：test_ui_models（8 用例 78 断言：四域派生空态/终态/易变
    字段/端点归属；consume 水位去重/重派生/路径独立推进/重试语义；
    on_publish 发布→唤醒调用序——钩子内观察新序列号==发布后序列号、
    无变更 drain 不触发；钩子异常收口；executor 任务内驱动 owner.drain
    调用注入回调——跨线程为机制契约面：owner 上下文可落在 executor 任务
    上，回调于 executor 线程触发（现行管线 drain 仅在主线程
    host_runtime pump/quiesce）——DEC-014 覆盖声明第 2 条：CI ctest
    不跑渲染）+ test_ui_actions
    （1 用例 51 断言：会话/消息/发现/传输四域经 UiActions → Manager 泵 →
    Fake Adapter SPI 全通道 + 入快照断言 + 独立 owner 受控关闭）+
    test_host_runtime 扩展（HostRuntime 装配参数 wake 计数器：quiesce
    发布后触发断言）。
  - **⑦ 验证（可复现命令与结果）**：configure `cmake --preset debug`
    通过；debug 全量 `ctest --preset debug` → 100% passed 42/40+2
    （151s 量级）；release `cmake --build --preset release --config
    Release` 0 error 0 warning + `ctest --preset release` → 100% passed
    42/42；新二进制直跑 `test_ui_models.exe`（73 断言全过）、
    `test_ui_actions.exe`（51 断言全过）、`test_host_runtime.exe`（67
    断言全过）。GUI 本机会话（Release aki.exe）：aki-run.log 装配 ok
    65ms + `wake: on_publish fired -> app::requestUpdate()` 首触发证据 +
    onShutdown 关闭序完好（8 步 + fully_stopped）；截图
    `build/scratch/aki-m5-03-window.png`（列表栏消费计数 "1 devices ·
    0 conversations · 0 transfers" 可见——消费面装配展示可运行）。
  - 评审修正（2026-09-27）：⑥ 原「executor 任务内调用注入回调」用例仅把
    自构计数 lambda 提交到 executor，从未触发注入的 on_publish（未涉及
    AppStateOwner），证据失实——用例改为 executor 任务内真实驱动
    `owner.drain()`（该任务即本次 drain 的 owner 上下文，主线程不并发
    drain），断言回调于 executor 线程触发且新序列号已可见（73→78 断言）；
    「跨线程」如实限定为机制契约面，非现行调用路径。修正后复验：debug
    全量 `ctest --preset debug` 42/42 + 直跑 `test_ui_models.exe`
    78 断言全过（连续 20 次运行稳定）；上方 ⑦ 的 release 档与 GUI 会话
    证据对应修正前树，未复跑（改动仅单个 console 测试文件）。
  - **⑧ 退出-3 grep（本项扩面）**：自建线程 0；ui/+main.cpp 的
    app::async/core::network/eui::network 0；EUI include/类型越过 ui/ 0；
    ui/ 持 transport 类型（HeyakiAdapter/NodeSession 等）0；ui/ 直写
    Store（submit_update/post_event/drain）0；eui include 仅 main.cpp +
    ui/ 渲染面（models 无 eui——DEC-005 落地实证）。
  - 限制与补跑条件：CI 五档门禁随本工作项 PR 首跑（本会话未推送，如实
    登记）；TSAN 运行期 UI 证据按 DEC-014 口径归本机 Linux（渲染不进 CI，
    RULE-11）；UiActions 的 start_transfer 在无接收端环境仅断言入队
    admission 与 SPI 出站记录，wire 侧推进语义归 M4 既有回环/单测（不
    重复声明）。
  - 同步：本里程碑（M5-03 勾选、本记录）、
    [aki_design](../design/aki_design.md) §9.1（消费面装配细节增补）、
    总计划（当前状态条目）。无新决策记录（on_publish/UiActions 为设计
    §9.1 既有契约的具体化，未越契约边界）。

- 2026-09-27（`M5-04` 完成；Windows 11 工作站（桌面会话）/ MSVC 2022
  BuildTools 14.44.35207 / CMake 4.1.0；负责人：Linductor）：
  - **① SPI 信任操作扩展（DEC-006 映射 3，设计 §8.1 先行同步）**：出站
    `confirm_pairing(device)`（→ `NodeSession::pair_peer`，scope 冻结
    `{message.send, file.push:inbox}`；提交被拒=会话缺失/非
    pairing_restricted/重复 pending，admission false 可见）与
    `revoke_trust(device)`（→ `revoke_trust_grants`，无有效 grant 时
    false）；入站第 12 方法 `on_pairing_completed(device, success, detail)`
    （Node 上下文回调 → Adapter 有界校验 + sink 投递，EXEC-02；析构中和
    observer）。真实 Adapter（set_pairing_observer 构造登记）与
    FakeHeyakiAdapter（confirm 记录 + `inject_pairing_completed` +
    `queue_pairing_result`/`set_has_valid_grant` 测试配置面）对齐。口令
    处理不进 SPI 签名（DEC-016）。
  - **② DeviceManager 信任三操作 + 配对结果路由**：confirm → wire 面先行
    （pair_peer 提交 admission），信任状态推进经配对结果事件异步落地（不
    乐观改状态——DEC-006 映射 3 冻结顺序）；reject → 纯本地判定（Pending
    → Rejected 无 wire 面）；revoke → wire 撤销全部有效 grant + 本地
    Trusted → Revoked。行改写经快照读行 + 整行 UpsertDevice（M1-06 确认
    语义；快照滞后窗口由用户操作节奏吸收——操作面数据本就来自快照，登记）。
    PairingCompletedWork：success → Trusted、失败 → Rejected（不新增
    AppEvent 主路径类型，同 on_transfer_paused 先例）；未知设备/非法转移
    拒绝可见。
  - **③ DEC-015 逐设备路径落地**：DeviceStore 增
    `DeviceConnectionPathEntry` 向量 + `SetDeviceConnectionPath{device,
    path}`（未知 id 拒绝、同值幂等 no-op 不重复发布）；退役全局
    `SetConnectionPath`/`LatestMailbox`（owner 成员/访问器/统计同步移除，
    grep 残留 0）；初连路径补发（`PeerSessionEvents::on_connected` 增携带
    映射路径，宿主删除 M3-06 的 Lan 硬编码）；断连置 Unknown（DM 断连
    处理同批提交）。设计 §10.1/§8.3/§9.1 同批修订（M1-08）。
  - **④ DEC-016 口令常量 + verifier 真实化**：`kAkiPairingPassword` 冻结
    常量（26 标量 ≥8 策略下限；2026-09-27 收口订正，原文误记 20）；
    `converge_local_initialization` created
    分支以 `create_password_verifier` 生成真实 argon2id verifier（取代
    M3-03 占位假编码串——冻结调研探针实测对任何口令均拒绝）；存量 profile
    处置=删除 db/profile.sqlite 重建（本机 %APPDATA%ki 即存量，GUI/
    smoke 运行不受影响——verifier 仅配对时消费；不静默迁移，登记于
    DEC-016）。确认弹窗无口令输入框（aki_ui_design §3 规格）。
  - **⑤ UiActions + 视图模型扩展**：UiActions 增 confirm_pairing/
    reject_device/revoke_device 绑定（M5-03 make_ui_actions 形态）；
    DeviceView 增逐设备 connection_path（Store join）、
    fingerprint_available（公钥缺失显式不可用态——relay 来源防御）、
    can_confirm/can_reject/can_revoke（§4 转移边派生）。
  - **⑥ Devices 页实体化**：设备行（presence 圆点 Online success/Offline
    subtlest、名称/类型/OS/逐设备路径徽标（caption 中性，Unknown 显
    "--"）、信任语义色徽标（Pending warning/Trusted success/Rejected·
    Revoked destructive/Unknown subtlest）、mono 指纹列）+ Pending 行
    Confirm（弹窗 mono 指纹核对 + Confirm/Reject 按钮）/Reject 按钮 +
    Trusted 行 Revoke 按钮（§4 转移边控制可用性）+ 发现启停按钮与来源
    分期披露（"LAN only (Relay / invite link / manual input are
    registered follow-ups, M3-09)"——如实呈现）。截图
    `build/scratch/aki-m5-04-devices.png`（点击导航切页后实拍：本地身份
    行 hy1_ 56 字符 mono 指纹、Unknown 徽标、发现启停）。
  - **⑦ 测试**：新建 test_device_trust（4 用例 76 断言：配对结果路由
    Pending→Trusted/Rejected + 未知设备 handler 拒绝可见；三操作转移边
    （reject 无 wire 面/revoke SPI 记录/非法转移 updates_rejected/无
    grant revoke 无操作可见）；UiActions 信任通道（泵→Fake SPI→状态）；
    连接→换路→断连→重连全序列路径断言）；test_local_identity 增口令
    往返用例（DEC-016：kAkiPairingPassword↔create↔verify，他串拒绝、
    短串生成被策略拒）；既有测试同步（BridgeSink 等 6 处 sink 增第 12
    方法；path 断言改逐设备；on_connected 签名；StubAdapter 信任方法）。
  - **⑧ 验证（可复现命令与结果）**：`ctest --preset debug` → 100% passed
    43/43（+test_device_trust，test_heyaki_adapter 路径断言重写）；
    release `--config Release` 构建 0 error 0 warning + `ctest --preset
    release` → 100% passed 43/43；GUI 本机会话（Release aki.exe）：装配/
    onShutdown 关闭序完好（aki-run.log），Devices 页点击切页截图归档
    （aki-m5-04-devices.png）；退出-3 grep 七项全 0（自建线程/禁用面/EUI
    越层/ui 持 transport/ui 直写 Store/退役全局路径残留 0/kAkiPairing
    Password 引用 heyaki/adapter 2 文件 + tests 9 文件，均为合法引用）。
    ⑧ 订正（2026-09-27 收口批，第 3 轮评审发现）：退役全局路径注释残留
    实有 2 处（app_state_updates.hpp 重复词组、update_jobs.hpp 退役
    SetConnectionPath 旧口径），本批修正后「残留 0」成立；「仅 heyaki/
    adapter 两文件」为评审修正批（集成测试字面量替换）之前的口径。
  - 评审修正（2026-09-27）：DEC-016「测试同步」两条在原变更集漏执行，
    本日补齐并复验——
    **集成回环口令字面量替换**：8 个集成测试文件 19 处 `pair_peer`
    口令字面量（aki-loopback/ps/rec/img/msg/ra/full/send-pw）中 17 处
    成功面同批替换为 `aki::heyaki::kAkiPairingPassword`（grep 替换后旧
    字面量残留 0）；错误口令
    负例（test_discovery_pairing_loopback）改用显式非匹配值
    aki-invalid-pw-c/-d 并注明理由——真实 verifier 只接受冻结常量，历史
    right/wrong-password 命名在假 verifier 下无区分度（DEC-016 背景
    自认）。替换后 debug/release 全量 ctest 43/43 复验零回归（本机链路
    环境为既有 [skip] 降级路径，本次替换使补跑面语义就绪，不改变当前
    结果）。**created 分支 argon2id 创建耗时实测（DEC-016 影响与风险/
    验证方式③）**：一次性探针 build/scratch/verifier_probe/（.gitignore
    内，不入库）直测生产同参调用 `create_password_verifier(
    kAkiPairingPassword, PasswordHashParameters{})`（local_identity.hpp
    created 分支），Release 构建，本机（Windows 11 工作站/MSVC 14.44）
    连跑 5 次：创建 67/60/58/61/57 ms，正确口令 verify 全 MATCH
    （55-67 ms），encoded 前缀实测 `$argon2id$v=19$m=65536,t=2,p=1$`
    （与 DEC-016 m=64MiB/t=2 披露一致）。复现：`cmake -S
    build/scratch/verifier_probe -B build/scratch/verifier_probe/build
    && cmake --build build/scratch/verifier_probe/build --config
    Release --target verifier_probe &&
    build/scratch/verifier_probe/build/Release/verifier_probe.exe`。
    ④ 的存量 profile（本机 %APPDATA%\aki 走既有分支）声明不变；
    首启动耗时面以本条实测登记，⑧ 的 43/43 数据经复验仍然成立。
  - 收口批（2026-09-27，第 3 轮评审 should-fix 项）：①Fake revoke 去除
    合成 on_pairing_completed（真实 NodeSession revoke 从不触发配对观察
    器，合成事件必然被状态机拒绝、徒增 updates_rejected 噪声）；②集成
    回环 presence/发现断言改谓词内 drain+load 截止轮询（connected 事件
    回调先 submit 后自增计数，单次快照在 owner 应用前抢跑——CI run
    36278949737 asan 实测；本机防火墙降级路径未实跑，由 CI 实跑验证）；
    ③test_app_managers 路由用例标题 eleven→twelve；④④⑧ 记录数字订正
    （见上）；⑤UpsertDevice 整行替换覆写隐患兑现登记（「风险与阻塞」）；
    ⑥退役 SetConnectionPath 注释残留 2 处清理；⑦test_peer_sessions_
    loopback 持久化载体 :memory:→<data_root>/db/aki.db3（CI 第 5 轮
    run 36295393398 asan/tsan 同点暴露：轮询修复使测试首次跑通全链后，
    尾部重启恢复断言得 0 行——:memory: 载体无 schema、设备行从未落盘，
    M3-06 起从未实跑段，按 §11.1 ②③ 恢复组合先行修复）。debug 全量
    ctest 复验。
  - **⑨ 如实降级（M3-09 纪律）**：双端配对→信任全链路（pairing
    restricted → pair_peer → observer 结果 → Trusted 端到端）受本机防火墙
    拦截 TLS 入站限制未执行——网络无关半边全部验证（SPI 路由/状态机转移
    边/UiActions 通道/verifier 往返/Fake 注入路径）；补跑条件沿 M3-09
    登记（防火墙放行入站 TCP / LAN 双端真机，M5-08 双端验收同批）。
  - 限制与补跑条件：CI 五档门禁随本工作项 PR 首跑（本会话未推送）；存量
    profile 的配对在 M5-07 设置面前不可用（DEC-016 处置：删除重建）；
    fingerprint 渲染为 Windows CascadiaMono 实拍证据，Linux mono 路径随
    CI/本机 Linux 会话确认（DEC-014 口径）。
  - 同步：本里程碑（M5-04 勾选、本记录）、
    [aki_design](../design/aki_design.md)（§4 指纹合并展示、§8.1 信任
    SPI/第 12 sink 方法、§8.3 路由表、§9.1 消费面、§10.1 comm 映射）、
    [aki_ui_design](../design/aki_ui_design.md) §3（指纹=DeviceId 规范串、
    弹窗无口令框）、[DEC-006](../decisions/DEC-006-heyaki-api-contract.md)
    映射 3（展示形式增补）、[DEC-015](../decisions/DEC-015-per-device-
    connection-path.md)、[DEC-016](../decisions/DEC-016-pairing-password-
    verifier.md)（均新建 Accepted）、总计划（当前状态条目 + 决策表）。

- 2026-09-27（`M5-05` 完成；Windows 11 工作站（桌面会话）/ MSVC 2022
  BuildTools 14.44.35207 / CMake 4.1.0；负责人：Linductor）：
  - **① 会话列表（SCOPE-05）**：`ui/pages/conversations_page.{hpp,cpp}`
    落地列表栏会话行（复用 M5-03 `derive_conversation_views` 派生；扩展
    `LastMessageSummary.outbound`——投递徽标仅己方最后消息展示，§3）；
    行内绝对排版（名称/预览/时间/投递徽标/Disconnected·Archived 徽标）+
    透明点击面；Rejected/Revoked 远端行无点击面（§3 会话入口禁用）；
    New chat 弹窗 = Trusted 设备选择面（`UiActions::ensure_conversation`，
    幂等）。
  - **② 聊天窗口（SCOPE-06/07）**：头部（对端名 + 路径 caption 徽标 +
    会话态 + 信任语义色徽标）；§3 断连横幅（Disconnected → `warning` 底
    + 三角叹号 + 「连接断开，等待恢复」）；消息历史 = scrollview + 气泡
    card（wrapContentHeight）+ text(wrap)（M5-01 复核结论「卡片自绘 +
    scrollview」实测成立——变高列由内容列 wrapContent 度量，virtuallist
    固定行高模型不适用处按组合策略承接，不改 pinned 依赖）；气泡左右
    归属 = 己方 accent 右侧 / 对方 card+边框左侧（§4 surface 层级区分）；
    投递态图标（§2.6 码点：Queued 时钟/Sending paper-plane/Sent 单勾/
    Delivered 双勾 `success`/Failed 叹号 `destructive`，仅己方消息）；
    系统消息居中 caption。
  - **③ 会话内文件卡片（SCOPE-08）**：Image/Video/File 载荷渲染为
    card+progress+button 卡片（§4 映射，M5-06 Transfers 页复用同一形态
    与语义色）：方向箭头（§2.6 f175/f176）+ 文件名 + 大小/mime + 进度条
    + 态文案（Transferring `brand`/Paused `warning`/Failed `destructive`/
    Completed `success`/其余中性，§3）；TransferStore 消费侧 join（§6.1②
    按 TransferId；进度 fraction total==0 → 0 不除零）；无传输行 →
    `warning` "no transfer row (single-side arrival)" 兜底态（DEC-010/
    DEC-013 边角，不猜进度）；图片卡片带 Preview 按钮 → 预览弹窗
    （image+dialog，§4；出站本地路径已知时渲染 image 元素，接收侧/无
    路径显式 metadata 态——不以占位图冒充）。
  - **④ 文本/图片发送链路**：输入区 input+button（onEnter/发送按钮 →
    `send_draft`；图片按钮 → `eui::platform::openFileDialog` 只读选取
    （M5-01 复核满足）+ 扩展名→mime 映射 + `std::filesystem::file_size`
    → `send_image`）——对话框与文件 stat 仅在点击回调上下文（主线程
    事件处理）执行，compose 三不纪律不破。**发送面页面形改造（M5-05）**：
    (a) wire id 生成收敛 `heyaki/adapter/wire_ids.hpp`（local_identity.hpp
    先例：单头 inline 实现；`new_message_id`/`new_transfer_id` = 16 随机
    字节全零重抽 → `heyaki::to_string` 规范串；`NodeSession::
    new_transfer_id` 改为委托同入口，生成入口单一——§6.1 同步）；(b)
    `UiActions` 增 `new_message_id`/`new_transfer_id` 生成器绑定（页面不
    携带 wire 编码知识，RULE-10）+ `send_image` 签名改 `source_path`、
    绑定改编排层 `send_image_message_with_hash`（hash-first：先传输准入，
    消息等 stored_sha256 完成后经 TM 泵延续发出，DEC-010/DEC-011）。
    `send_text` 页面形态不变（id 经生成器取得后传入）。
  - **⑤ 唤醒缺口修复（本机 GUI 实测发现，§9.1 同步）**：现行管线
    Manager handler 跑在 executor 任务上、publish 只发生在主线程 drain
    （`HostRuntime::pump_state`）——M5-03 的 on_publish 唤醒仅在发布后
    触发，主循环无从得知「有待 drain 的更新」：GUI 发送文本后 admission
    反馈可见（点击自身触发重组）而消息行滞留至下一次输入事件。修复：
    `AppStateOwnerOptions` 增 `on_update_submitted`（`submit_update`/
    `submit_update_for` 受理成功后于提交者上下文同步调用——唯一的跨线程
    状态生产点；拒绝不触发；异常全捕获计数 `submit_hook_failures`，原子
    计数——多线程提交者）；`HostRuntime` 注入同一 wake 回调。GUI 复测：
    发送后气泡当帧可见。设计 §9.1 快照消费与唤醒条款先行修订（M1-08）。
  - **⑥ 视图模型扩展（`ui/models/view_models`）**：`MessageView` +
    `derive_message_views`（端点归属守卫、方向、投递态、文本/媒体载荷
    语义、TransferStore join 进度/状态、无传输行兜底态）；消息预算 4096
    内有界派生。页面模型 `ConversationsPageModel`：选中会话/输入草稿/
    消息视图派生缓存（水位×选中去重重派生）/滚动代数/弹窗 open 态/
    出站图片源路径登记（容量 64，RULE-09）。
  - **⑦ 滚动/弹窗契约（探针定形）**：pinned scrollview 运行期滚动状态
    按元素 id 持有（首次构建播种 offset、其后运行期所有）——「回到底部」
    以 `history_scroll_gen` 代数进位切换 scrollview id 表达（选中切换/
    新消息入流 +1；投递态原位更新不进位，不打断阅读位置）；用户滚动
    位置在两次代数进位之间由运行期保持；measure cache 以
    代数:条数:末条 id 为 contentKey。气泡 x 归属须在行内 stack 绝对定位
    （直接挂内容列会被列布局拉回左缘——本机实测）；dialog 背板/居中
    须显式 `.screen(窗口宽高)`（默认 800x600——实测）；背板关闭请求经
    `onOpenChange` 回写页面持有 open 态（单向数据流对称面）。
  - **⑧ 测试**：test_ui_models 扩展（MessageView 派生：方向/媒体 join
    进度 0.5/无传输行兜底/端点守卫；LastMessageSummary.outbound；新
    on_update_submitted 用例——受理触发/拒绝不触发/钩子异常全捕获计数
    /executor 任务内受理跨线程触发，共 116 断言 10 用例，直跑通过）；
    test_ui_actions 更新+扩展（send_image 新签名路由 hash-first 编排：
    TM 准入 + Fake 传输命令 + 无 IO 承载空 hash 延续不发消息（§6.1②，
    全链路归 test_transfer_send_path）；wire id 生成用例——
    parse_message_id/parse_transfer_id 规范性 + 唯一性，共 65 断言
    2 用例，直跑通过）；既有 43 项语义零损失。
  - **⑨ 验证（可复现命令与结果）**：configure `cmake --preset debug`
    通过；debug 全量 `ctest --preset debug` → 100% passed 43/43
    （179.6s）；release `cmake --build --preset release --config
    Release` 0 error 0 warning + `ctest --preset release` → 100%
    passed 43/43（170.5s）；直跑 test_ui_models.exe（116 断言）/
    test_ui_actions.exe（65 断言）/test_host_runtime.exe（67 断言）
    全过。
  - **⑩ GUI 本机会话（RULE-11 归档）**：Release aki.exe，GUI 数据根经
    `APPDATA` 注入 scratch 剖面 `build/scratch/m5-profile/`（真实
    `%APPDATA%ki` 未触碰）；演示数据经 scratch 种子探针
    `build/scratch/m5-demo-seed/`（gitignored，standalone CMake 直链
    Release 树 aki_persistence 静态库，不入产品构建图——M5-01 探针
    先例）写入受信对端/会话/消息/传输行。GUI 实测：会话列表（最后消息
    摘要/方向投递徽标/时间）、聊天窗口（气泡左右归属 + 投递语义色 +
    文件卡片 Completed `success` 100% / 恢复降级 Paused `warning` 72%
    ——DEC-013 重启降级边在 GUI 可见）、文本发送实测（real Adapter 无
    会话 → admission `hym1_` 规范 id 可见 + 消息行 Failed 徽标 + 列表
    摘要实时更新——发送/唤醒/消费/派生全链）、图片预览弹窗（无本地
    路径 metadata 态 + DEC-011 登记的「消息行重启重建 stored_sha256
    为空」在 GUI 呈现）、New chat 弹窗、onShutdown 关闭序完好（8 步 +
    fully_stopped=1 + workers 2/2 + db_drained=1）。截图 ×5 +
    aki-run-m5-05-final.log 归档 `build/scratch/`（会话内文件卡片
    Completed/Paused、文本发送 Failed 徽标、预览弹窗、New chat 弹窗）。
  - **⑪ 退出-3 grep（本项扩面，最终代码态）**：第一方线程创建 0；
    ui/+main.cpp 的 app::async/core::network/eui::network 0；EUI-NEO
    include/类型越过 ui/ 0；ui/ 直写 Store（submit_update/post_event/
    drain_updates）0；ui/ 持 transport 对象 0（ui_actions.hpp 注释提及
    NodeSession 为文档性引用）；eui include 仅 main.cpp + ui/pages 渲染
    面（models 无 eui）；test_ui_models.exe dumpbin 对
    eui::/components::/glfw 符号 0 命中（DEC-005 维持）。
  - 限制与补跑条件（沿 M3-09/M4-07/M5-04 ⑨ 降级纪律）：
    (a) 双端真机的文本 Delivered 回报、图片 wire 面（codec/文件本体）、
    断连→恢复横幅转换、传输暂停/恢复/取消操作面未在本会话执行——
    防火墙拦截入站 TCP / 无 LAN 双端；网络无关半边（视图派生、UiActions
    路由、hash-first 闸门、真实 Adapter 无会话拒绝路径）已验证；补跑归
    M5-08 双端验收同批。(b) GUI 会话中的图片发送按钮点击 → 原生文件
    对话框 → hash-first 闸门拒绝路径（无会话 → 传输行 Failed、
    message_id NULL、canonical `hyt1_` id）在 GUI 会话期间实际产生
    （DB 实证；点击来源无法精确归因——本机为使用者台面机，窗口自动化
    期间可能存在人工交互）——该行语义与闸门契约一致，作为 GUI 会话
    副产品如实登记。(c) 文件卡片 Failed/Cancelled 态、断连横幅、
    Archived 徽标为条件分支，本会话不可达（无真实断连）——Failed 态由
    test_ui_models 派生断言承载，横幅渲染随 M5-08。(d) 会话行/弹窗
    设备选择超视口滚动、图片本体（image 元素本地路径态）像素级复核随
    M5-06+/M5-08。(e) CI 五档门禁随本工作项 PR 首跑（本会话未推送，
    如实登记）；TSAN 运行期 UI 证据按 DEC-014 口径归本机 Linux。
  - 同步：本里程碑（M5-05 勾选、本记录）、
    [aki_design](../design/aki_design.md) §6.1（wire id 生成入口收敛）、
    §9.1（UiActions 发送面形态 + on_update_submitted 唤醒条款）、
    [aki_ui_design](../design/aki_ui_design.md) §3（无传输行兜底态）、
    §5（M5-05 落地记录）、总计划（当前状态条目）。无新决策记录：
    wire id 收敛/受理唤醒为 §6.1/§9.1 既有契约的修订与具体化（M1-08
    先改设计后合代码），未越契约边界。

- 2026-09-27（`M5-06` 完成；Windows 11 工作站（桌面会话）/ MSVC 2022
  BuildTools 14.44.35207 / CMake 4.1.0；负责人：Linductor）：
  - **① Transfers 页实体化（SCOPE-08）**：`ui/pages/transfers_page.{hpp,cpp}`
    —— 传输行（固定高 88，scrollview 容纳；行数预算 256，RULE-09）=
    文件卡片本体（方向箭头/文件名/大小·mime/进度条/状态文案，§4 共享
    形态）+ 方向·对端 caption（快照设备 join）+ 操作按钮纵排（状态门控）；
    页头计数 + 页脚登记披露 + 空态文案。无独立页面模型（无弹窗/草稿/
    滚动代数需求——滚动位置由 pinned scrollview 运行期状态保持；操作
    反馈经共享 `MainWindowModel::last_action_feedback`，RULE-09）。
  - **② 共享传输卡片抽取（§4 复用契约兑现）**：`ui/components/
    transfer_card.{hpp,cpp}`（命名空间 `aki::ui::widgets`——避让 EUI
    `::components` 限定名，aki::ui 内 `components::` 解析冲突实测规避）；
    M5-05 ③ 会话内文件卡片本体原样迁移（方向/文件名/大小·mime/进度/
    状态 + `format_bytes`/`transfer_state_color`（§3 语义色）），会话页
    气泡卡片改消费共享体（形状与语义色不变——M5-05 前瞻条款落地）；
    `TransferView` 增 `mime_type` 透传（行侧真实媒体标注，空 → 卡片
    `application/octet-stream` 兜底）。
  - **③ 操作面与 DEC-013⑥ UI 触达**：Pause/Resume/Cancel 按
    `TransferView::can_pause/can_resume/can_cancel` 门控（§7 固定边纯派生：
    Transferring→Pause、Paused→Resume、非终态→Cancel、终态全不可用——
    Queued→Paused/Negotiating→Paused 为 DEC-013 重启降级边，非用户动作
    面）；操作经 `UiActions` 既有传输三接口（M5-03 建面），返回值即泵
    入队 admission、拒绝经反馈行可见（RULE-09）。**Paused 行 Cancel =
    DEC-013⑥ 无会话行 `cancel_transfer` 直接终态写入的 UI 触达**（M4
    唯一出口）：GUI 实测 policy.pt Paused·72% 行经 Cancel 即转
    `Cancelled · 72%`（中性进度条、按钮集收起、反馈行
    "cancel admitted (hyt1_seedpolicy72pct001)"——发送/唤醒/消费/派生/
    渲染全链 + owner 状态机终态校验；该反馈行文案在归档截图底部与页脚
    登记披露行叠印不可辨读——页脚分行修正与复验见⑩，终态转换与全链
    断言不受影响）。
  - **④ 孤儿接收行 re-push 触发面登记（M4-05/06 移交项）**：接收行恢复
    按 DEC-013② 为 wire 事件驱动（对端重推/committed-窗口重发收敛），
    Aki 侧无 re-push 请求出站接口——本项以页脚登记披露呈现
    （"orphan receive rows: re-push trigger is a registered follow-up
    (M4 handover)"），不冒充可用动作；功能化触发条件=出现 re-push 出站
    接口决策（wire 面需先立 DEC）。
  - **⑤ 接收根残留 GC 议题处置（DEC-012③ 登记项；两向决策点）**：
    **显式延后**。理由：收敛路径已存在且运行——Completed 作业同源删除
    （DEC-012①）、Failed/Cancelled 触发 discard 幂等删除 + heyaki 失败/
    取消自清其接收侧残留（DEC-012③）、Paused 行恢复后经作业幂等重跑
    收敛（DEC-013②）；实现即时 GC 需扩 M2-06 启动清扫面/新增清扫作业
    （M3/M4 既有语义面），在残留无实证累积前属无验收对象的预防性复杂度。
    **实现触发条件（登记，任一出现即立项）**：(a) M5-08 双端验收实证接收
    根残留累积（heyaki 自清不成立/部分传输残片堆积）；(b) 接收根容量预算
    触顶需求出现；(c) 产品引入用户清理/删除传输残留动作需求。处置结论
    已同步 aki_design §11.1（清扫条目，M1-08 先行）。
  - **⑥ 测试**：test_ui_models 扩展（TransferView 操作可用性派生六态
    断言：Transferring/Completed/Paused/Queued/Failed 的
    can_pause/can_resume/can_cancel 组合 + mime 透传/兜底契约，135 断言
    10 用例直跑通过）；test_ui_actions 扩展（resume 入口经绑定面 → Fake
    Resume 命令记录——传输控制通道 Start/Pause/Resume/Cancel 全序锁定，
    67 断言 2 用例直跑通过）；无新增并发路径（DOD-02 六项无新增面——
    既有宿主生命周期覆盖维持，test_host_runtime 67 断言）。
  - **⑦ 验证（可复现命令与结果）**：configure `cmake --preset debug`
    通过；debug 全量 `ctest --preset debug` → 100% passed 43/43；
    release `cmake --build --preset release --config Release` 0 error
    0 warning（一次中途链接失败复测：aki.exe 被在跑 GUI 占用——关窗后
    全量重建通过，非代码缺陷）+ `ctest --preset release` → 100% passed
    43/43；直跑 test_ui_models.exe（135 断言）/test_ui_actions.exe
    （67 断言）全过。
  - **⑧ GUI 本机会话（RULE-11 归档）**：Release aki.exe，GUI 数据根经
    `APPDATA` 注入同一 scratch 剖面（真实 `%APPDATA%ki` 未触碰）；
    演示行经 scratch 种子探针增补（入站 report-q2.pdf Paused·45%——
    DEC-013 重启降级形态）。GUI 实测：四态行同屏（Completed `success`
    100% / Paused `warning` 72%·45% / Failed `destructive` / 经 UI 操作
    产生的 Cancelled 中性）、终态行按钮集收起、mime 真实标注
    （image/png、application/pdf）、DEC-013⑥ 取消全链（见 ③）、页脚
    登记披露、onShutdown 关闭序完好（8 步 + fully_stopped=1 +
    workers 2/2 + db_drained=1）。截图 ×2
    （aki-m5-06-transfers.png / aki-m5-06-cancel-clicked.png）+
    aki-run-m5-06-final.log 归档 `build/scratch/`。
  - **⑨ 退出-3 grep（本项扩面，最终代码态）**：第一方线程创建 0；
    ui/+main.cpp 的 app::async/core::network/eui::network 0；EUI-NEO
    include/类型越过 ui/ 0；ui/ 直写 Store（submit_update/post_event/
    drain_updates）0；ui/ 持 transport 对象 0；eui include 仅 main.cpp +
    ui/ 渲染面（ui/components 为 aki_ui 目标内单元，models 无 eui）；
    test_ui_models.exe dumpbin 对 eui::/components::/glfw 符号 0 命中。
  - **⑩ 页脚分行修正（2026-09-27 独立评审发现；同日修复并复验）**：
    归档证据 aki-m5-06-cancel-clicked.png 底部反馈文字 "cancel admitted
    (hyt1_seedpolicy72pct001)" 与页脚登记披露行叠印不可辨读——Transfers
    页经 main_window 以 y=0 全高调用，披露行原锚点（height-caption-
    section）与跨页反馈行完全重合，③ 与工作项摘要所记「admission 反馈
    可见」恰在拒绝场景不成立（渲染正确性缺陷）。修复：transfers_page
    披露行上移一行（caption 行高 + tiny 间距），列表底预留同步扩为两行
    页脚，反馈行保持 main_window 既有锚点不动。复验：debug 全量
    `ctest --test-dir build/debug --preset debug` → 100% passed 43/43
    零回归；GUI 本机会话（debug aki.exe，默认 %APPDATA% 数据根——M5-06
    原 scratch 剖面未留存、传输行不可复现，以跨页 sticky 反馈路径复验
    同一几何锚点）：Devices 页 Start Discovery admission 反馈置位后切
    Files 页，反馈行与披露行两行并存、各自可辨读
    （aki-fix-footer-1-nofeedback.png / aki-fix-footer-3-feedback-devices.png /
    aki-fix-footer-4-files-split.png + 页脚裁切，归档 `build/scratch/`；
    onShutdown 关窗退出）。补跑条件：传输行 Pause/Resume/Cancel 操作
    反馈与披露行同屏截图随 M5-08 双端验收补跑（两行行位由固定锚点派生，
    与列表行数无关；非空列表场景底部预留已同步扩为两行页脚）。
  - 限制与补跑条件（沿 M3-09/M4-07 降级纪律）：(a) Pause/Resume 的 wire
    侧效果（对端暂停确认/断点续传推进）需真实双端——本会话仅断言入队
    admission 通道与 UI 门控派生，wire 语义归 M4-05 既有单测与 M5-08 双端
    验收；(b) Cancelled/Failed 行的 .part discard 作业语义归 M4 既有单测，
    本页渲染不重复断言；(c) CI 五档门禁随本工作项 PR 首跑（本会话未推送，
    如实登记）；TSAN 运行期 UI 证据按 DEC-014 口径归本机 Linux。
  - 同步：本里程碑（M5-06 勾选、本记录）、
    [aki_design](../design/aki_design.md) §11.1（接收根 GC 显式延后 +
    触发条件登记，M1-08 先行）、[aki_ui_design](../design/aki_ui_design.md)
    §5（M5-06 落地记录：共享组件抽取 + 操作面门控/披露形态）、总计划
    （当前状态条目）。无新决策记录（GC 延后为 DEC-012③ 登记议题的两向
    处置之一，触发条件登记于本记录并被 aki_design §11.1 引用）。

- 2026-09-27（`M5-07` 完成；Windows 11 工作站（桌面会话）/ MSVC 2022
  BuildTools 14.44.35207 / CMake 4.1.0；负责人：Linductor）：
  - **① Settings 页实体化（SCOPE-12）**：`ui/pages/settings_page.{hpp,cpp}`
    —— 主题三选 `segmented`（§4 Settings 页组件映射；三段
    Follow system/Light/Dark，field 36 高，selection=页面持有 UI 态
    `MainWindowModel::theme_setting`）经既有 `akiTheme()/
    akiSemanticColors()` 装配面即时生效（§9.1 覆写清单与
    aki_theme_values 回归对照不变——仅档位选择；GUI 深浅两档全壳渲染
    对照实测）；最小设置项只读展示：数据目录（`HostRuntime::data_root()`
    装配面，std::string）+ 本地设备 id（快照
    `UiStateSnapshot::local_device`），mono caption（§2.1 mono 用于
    路径/技术内容）。
  - **② DEC-005 缺口处置 = 平台层查询路径**：`app/lifecycle/system_theme.
    {hpp,cpp}` 平台条件编译单元（persistence/storage/data_root.cpp 先例：
    平台分支隔离、公开面仅 std/aki 枚举，RULE-10）——Windows 查询
    `HKCU\...\Themes\Personalize\AppsUseLightTheme`（有界单次注册表
    读取，RegGetValueA，advapi32 仅 WIN32 链接）；缺失/类型不符/读失败/
    非 Windows 平台一律 `Unknown` → `resolve_effective_theme` 回落
    Light（不猜测），页内披露平台支持面（"unsupported platforms fall
    back to Light"）。DEC-005 冻结时登记的「无系统主题检测 API」缺口按
    其预设的平台层处置路径收口（里程碑风险节同步）。
  - **③ 主题三选语义（§9.1 Settings 页装配条款，M1-08 先行同步）**：
    `ThemeSetting = FollowSystem/Light/Dark` 页面持有；Light/Dark 直取、
    FollowSystem 即时重查（点击回调上下文的有界读取——非 compose 树内）；
    **主题选择为会话级**——不跨启动持久化（schema v1 无设置表，扩表属
    公开契约变更须先立决策，不私自扩表；页内披露 + 本记录登记，后续项）；
    启动初值 = FollowSystem 解析一次（组合根首帧装配期；GUI 日志证据
    "settings: theme follow system -> light (system=queried), data
    directory: ..."）。
  - **④ ThemeMode 迁出 + 解析状态机可测面**：`ThemeMode` 自
    aki_theme.hpp 迁至 EUI-NEO 无关的 `ui/theme/theme_mode.hpp`（连同
    `ThemeSetting`/`SystemTheme` 与 `resolve_effective_theme` constexpr
    纯函数——FollowSystem+Unknown 回落 Light 等），aki_theme.hpp 保留
    EUI 装配面（aki_ui_design §2 映射不受影响）；akiTheme()/语义色消费方
    零改动。
  - **⑤ 测试**：test_ui_theme_values 增「主题三选解析状态机」用例
    （Light/Dark 直取、FollowSystem×{Dark,Light,Unknown} 三路径——
    Unknown 回落 Light 断言；62 断言 7 用例直跑通过）；无新增并发路径
    （系统查询为点击回调/首帧装配内的有界同步读取，DOD-02 无新增面，
    宿主生命周期覆盖维持）。
  - **⑥ 验证（可复现命令与结果）**：configure `cmake --preset debug`
    通过；debug 全量 `ctest --preset debug` → 100% passed 43/43；
    release `cmake --build --preset release --config Release` 0 error
    0 warning + `ctest --preset release` → 100% passed 43/43（最终代码
    态复跑）；直跑 test_ui_theme_values.exe（62 断言）全过。
  - **⑦ GUI 本机会话（RULE-11 归档）**：Release aki.exe，GUI 数据根经
    `APPDATA` 注入同一 scratch 剖面（真实 `%APPDATA%ki` 未触碰）；
    深浅两档渲染对照截图 ×3（aki-m5-07-settings.png 跟随系统选中
    light、aki-m5-07-dark.png Dark 选中全壳深色、aki-m5-07-light.png
    Light 选中全壳浅色——切换即时生效，requestUpdate 唤醒拾取）+
    aki-run-m5-07-final.log（settings: theme follow system -> light
    (system=queried) 启动解析证据 + onShutdown 8 步关闭序）归档
    `build/scratch/`。
  - **⑧ 退出-3 grep（本项扩面，最终代码态）**：第一方线程创建 0；
    ui/+main.cpp 的 app::async/core::network/eui::network 0；EUI-NEO
    include/类型越过 ui/ 0；ui/ 直写 Store 0；ui/ 持 transport 对象 0；
    **system_theme 公开头无平台类型**（0 命中——平台 API 封死在
    system_theme.cpp 条件编译分支内，RULE-10）；eui include 仅 main.cpp
    + ui/ 渲染面。
  - 限制与补跑条件：(a) 主题选择跨启动持久化未实现（schema v1 无设置
    表；扩表须先立决策登记——本项如实降级为会话级并页内披露，触发条件
    = M5-08 后产品确认持久化需求）；(b) 非 Windows 平台的系统主题查询
    返回 Unknown（XDG portal 需 dbus 依赖，不引入——DEC-005 供应链
    纪律），回落 Light 页内披露；跨平台扩展出现需求时再立决策；(c) CI
    五档门禁随本工作项 PR 首跑（本会话未推送，如实登记）；深色档在
    Linux/GPU 栈的渲染复核按 DEC-014 口径归本机 Linux 会话。
  - 同步：本里程碑（M5-07 勾选、本记录、风险节处置收口）、
    [aki_design](../design/aki_design.md) §9.1（Settings 页与主题三选
    装配条款）、[aki_ui_design](../design/aki_ui_design.md) §5（M5-07
    落地记录）、总计划（当前状态条目）。无新决策记录（平台查询为
    DEC-005 缺口的预设处置路径之一，条款级同步；设置持久化留待决策）。

- 2026-09-27（`M5-08` 完成（验收归集，无产品代码变更）；Windows 11 工作
  站（桌面会话）/ MSVC 2022 BuildTools 14.44.35207 / CMake 4.1.0；负责人：
  Linductor）：
  - **① 验收基线复跑**：工作树干净（`git status` 0 项）；debug 全量
    `ctest --preset debug` → 100% passed 43/43；release `ctest --preset
    release` → 100% passed 43/43（无代码变更基线确认）。
  - **② MVP 清单归档（设计第 15 节 + `SCOPE-01`~`SCOPE-12`，两态标注：
    「已验证」/「本机已验证、双端待补跑」——不冒充已验证，工程规范 §4
    规则 3/4）**：
    - `SCOPE-01` 设备身份（Heyaki 密码学身份、无账户层）：**已验证**——
      M3-03（test_local_identity：身份跨重启稳定、公钥逐字节一致；
      aki_host_smoke 重启恢复断言）；DEC-016 口令 verifier 真实化
      （M5-04）；GUI：Settings 本地设备 id mono 展示
      （build/scratch/aki-m5-07-settings.png）。
    - `SCOPE-02` 设备发现（LAN/已知设备记录）：**本机已验证（发现 SPI/
      启停/事件字段）、去重/不重放未测、双端待补跑**——已验证锚点：
      Adapter 发现 SPI/注入→快照/事件面（test_heyaki_adapter，Fake
      注入）；真实 LAN 管道启停与 discovered 事件字段（method=LAN、对端
      公钥、Unknown 态、stop 后零新增——test_discovery_pairing_loopback，
      本机经 [skip] 受控退出点仅保发现/停止断言）；GUI 发现启停实拍
      build/scratch/aki-m5-08-discovery.png "start_discovery admitted"。
      **未测（2026-09-27 评审修正：原锚点「lan_discovery 单测」不存在
      ——ctest 43 项无该目标，tests/ 无直接测 LanDiscoveryPipeline 的
      单测，GUI 单次渲染亦不构成去重观察）**：lan_discovery.hpp:94-98
      的 trusted 跳过（已知设备记录不重放 discovered，§8.1）与 seen_
      去重（重复端点不重复合成）语义无任何测试断言。补测条件：新增
      LanDiscovery 管道单测（合成 endpoints 含 trusted/重复项，断言不
      重放/不重合成），随双端补跑一并执行；Relay/邀请链接/手动输入为
      M3-09 登记分期（GUI 已披露），双端真实发现（另一台 Aki 设备广播/
      被广播）随补跑。
    - `SCOPE-03` 信任建立（Unknown→Pending→Trusted/Rejected/Revoked +
      指纹确认）：**网络无关半边已验证、GUI 确认弹窗被缺陷阻塞、双端
      待补跑**——状态机（test_trust_state）+ 信任三操作 SPI/路由/转移边
      （test_device_trust 76 断言）+ UiActions 通道（test_ui_actions）；
      **缺陷登记（本轮验收发现）**：Devices 页 Pending 行 Confirm 按钮
      点击无响应（弹窗不出现、无任何状态变化；复现 2 个全新会话各 ≥2
      次点击 100% 复现；同行 Reject/邻行 Revoke/导航/其他页按钮均正常
      ——证据 build/scratch/aki-m5-08-confirm-dialog.png + 本会话操作
      序列日志）；伴随一次疑似错乱触发（Confirm 位置点击后出现 alpha 行
      "revoke admitted" 反馈）。处置：**独立修复工作项/MR**（嫌疑方向：
      retained-mode compose 下条件行内按钮的回调登记/元素复用），修复并
      复测后回填本清单该项 GUI 证据；双端 pair_peer wire 面随补跑。
    - `SCOPE-04` 设备列表（名称/类型/OS/连接方式/在线状态）：**已验证**
      ——M5-04（DEC-015 逐设备路径 test_device_trust + GUI 设备行/
      徽标/指纹列 build/scratch/aki-m5-04-devices.png、
      aki-m5-08-discovery.png 三行三信任态同屏）。
    - `SCOPE-05` 一对一 Conversation（路径无关模型 + List）：**已验证**
      ——会话模型 M1/M3（test_conversation_state、DEC-009 归属约定单测
      test_ui_models）；GUI 会话列表（最后消息摘要/方向/投递徽标，
      build/scratch/aki-m5-05-chat.png、aki-m5-08-discovery.png 左栏）
      + New chat 建会话入口（aki-m5-05-newchat.png）。
    - `SCOPE-06` 一对一文本消息（发送/接收/送达状态）：**本机已验证、
      双端待补跑**——发送路由/UI（test_ui_actions、GUI 实测：canonical
      hym1_ admission + Failed 徽标 + 列表实时更新，
      aki-m5-05-sent.png）；接收侧消息行（test_app_managers 收到路由）；
      双端 Delivered 回报/ack 链路随补跑（M3-07 先例降级）。
    - `SCOPE-07` 图片消息：**本机已验证、双端待补跑**——codec 往返/拒收
      （test_image_payload_codec）、SPI/Manager/编排闸门
      （test_heyaki_adapter、test_transfer_send_path、M4-03/06）；GUI
      发起面（文件对话框只读选取→hash-first 编排）M5-05 实测（闸门
      拒绝路径 GUI 会话 DB 实证）；双端 aki.image wire 面 + 对端展示随
      补跑。
    - `SCOPE-08` 文件传输（独立 Session/进度/暂停/取消 + Transfers 页）：
      **本机已验证、双端待补跑**——发送承载/接收合并/控制面/重启处置
      （test_transfer_send_path/receive_path/recovery/state，DEC-011/
      012/013，M4-02~06）；GUI Transfers 页四态行同屏 + DEC-013⑥ 无
      会话行取消直接终态实测（aki-m5-06-transfers.png、
      aki-m5-06-cancel-clicked.png）；双端 wire 侧暂停/恢复对端效果随
      补跑。
    - `SCOPE-09` 本地持久化（重启恢复）：**已验证**——M2-07/08
      （test_persistence_*、restart 恢复用例、 aki_host_smoke
      smoke.device_lifecycle）；GUI scratch 剖面跨会话恢复多次实测
      （M5-05/06/08 各次启动 devices/conversations/messages/transfers
      计数恢复 + DEC-013 非终态降级 Paused 行 GUI 可见，
      aki-m5-06-transfers.png）。
    - `SCOPE-10` Presence 与连接路径（LAN/P2P/Relay）：**状态面已验证、
      实时变化待补跑**——管道/映射/逐设备路径（test_peer_sessions_
      pipeline、DEC-015 test_device_trust 路径序列）；GUI 徽标展示
      （aki-m5-04-devices.png，未连接路径 "--"）；真实 presence Online/
      路径切换需双端会话（补跑；Relay 路径依赖 relay 接入登记项）。
    - `SCOPE-11` 断线恢复：**网络无关半边已验证、双端待补跑**——
      reconnect_loop 重连协调器单测（test_reconnect_loop 7 用例，
      EXEC-05 可中断切片/预算/取消）+ 会话模型路径无关（RULE-06 单测）；
      close_lan 合成断开回环与真实断连→恢复横幅转换（§3 warning 横幅
      已实现、条件分支 GUI 未触发——如实登记）随补跑。
    - `SCOPE-12` EUI-NEO 主窗口与基础主题：**已验证**——M5-02（三栏壳+
      四页导航 aki-m5-02-window.png、主题覆写双份回归对照）+ M5-07
      （主题三选深浅两档全壳对照 aki-m5-07-settings/-dark/-light.png +
      onShutdown 关闭序日志）。
    - 设计第 15 节链路清单逐环节映射：身份建立（SCOPE-01）→发现
      （SCOPE-02）→信任确认（SCOPE-03，GUI 弹窗被缺陷阻塞——见上）→
      会话（SCOPE-05）→文本/图片/文件（SCOPE-06/07/08）→进度→暂停/
      恢复/取消（SCOPE-08）→重启恢复（SCOPE-09）→断线恢复（SCOPE-11）
      ——逐环节证据同上；**全链路串联（双端）未执行**（退出-1 保持
      未勾选，见 ⑤ 降级声明）。
  - **③ 本机补证（本项新增归档）**：发现启停 GUI 实拍
    （aki-m5-08-discovery.png：Devices 页三行三信任态 + Start Discovery
    点击后 "start_discovery admitted" 反馈——真实 LAN 发现启动）；GUI
    数据根经 `APPDATA` 注入 scratch 剖面（真实 `%APPDATA%ki` 未触碰，
    演示行经 gitignored scratch 种子探针写入）；aki-run-m5-08-final.log
    （设置解析行 + onShutdown 8 步关闭序 fully_stopped=1）。
  - **④ M3 登记补做条件复核闭环（M3-09 ④ 归档项逐项结论）**：
    - (a) **占位口令 verifier：闭合**——DEC-016（M5-04）落地
      kAkiPairingPassword + 真实 argon2id verifier
      （heyaki/adapter/local_identity.hpp:125-129 create_password_
      verifier 接线；test_local_identity 口令往返用例；M5-04 评审修正
      argon2id 创建 57-67ms 5 连跑实测）；M3-03 占位假编码串已退役。
    - (b) **kAkiPairingPassword 维持（登记不变）**：DEC-016 移除条件
      「M5-07 设置面引入用户口令」未触发——M5-07 交付的最小设置项为
      数据目录展示等（M5 范围条款 :70-71），不含口令输入；配对口令仍为
      公开弱口令（DEC-016 安全语义披露维持），再登记至用户口令流程
      立项。
    - (c) **secret backend prefer_os_backend=false：再登记（触发条件）**
      ——代码现状 heyaki/adapter/local_identity.hpp:75（heyaki 加密文件
      回退承载，确定性配置）；M3-03 登记的「OS 钥匙串随 M5 设置面」未
      触发（同 (b)，M5-07 设置面未含 secret backend 项）；再登记触发
      条件：OS 钥匙串集成的安全/产品需求确认（届时经决策记录定
      prefer_os_backend 平台策略与用户口令流程）。
    - (d) **DeviceIdentity display_name/device_class/os_name/capabilities
      元数据占位：再登记（触发条件）**——代码现状 heyaki/adapter/
      lan_discovery.hpp:105-107（display_name 取 DeviceId 前 16 字符
      占位；class/os/capabilities 缺省——DEC-006 缺口条目：pinned
      LanPresence 不携带元数据）；再登记触发条件：heyaki 协议演进使
      LanPresence 携带元数据，或 RPC 能力查询可用（届时映射至
      DeviceIdentity 并同步 GUI 列）。
    - (e) **发现来源分期（Relay/邀请链接/手动输入）：维持 M3-09 登记**
      （GUI 分期披露维持，aki-m5-08-discovery.png 页脚可见）。
  - **⑤ 双端真机项显式降级声明（退出-1 保持未勾选）**：范围=文本
    Delivered 回报、图片/文件 wire 面与对端效果、断连→恢复横幅转换、
    传输暂停/恢复/取消 wire 侧、M3/M4 退出-1/3 同批补跑（含 CI tsan
    运行期证据）；**原因**：本机防火墙拦截入站 TCP、无第二台同网段
    Aki 设备（M3-09 起同批约束，网络无关半边已全部验证——见 ②）；
    **负责人**：Linductor；**补跑条件**：防火墙放行入站 TCP + LAN 双端
    真机（两台设备同网段运行 Release aki.exe），按 M3-09/M4-07 先例
    执行 M5-08 退出-1 全链路清单并归档证据。
  - **⑥ 同步**：本里程碑（M5-08 勾选、本记录）、总计划（当前状态条目
    含 PR #44 欠账补记）；aki_design §15 无变更（清单原文为准）。缺陷
    （Confirm 无响应）走独立修复工作项/MR，修复后回填 SCOPE-03 GUI
    证据并复跑全量 ctest。
