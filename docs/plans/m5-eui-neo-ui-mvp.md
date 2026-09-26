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
> 更新日期：2026-09-27

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
- [ ] `M5-03` 状态消费面与视图模型（可验收：快照排空 + 跨线程唤醒路径经
  单测，四域视图模型派生有断言，UI 操作全经 Application 出站面——边界由
  退出-3 grep 与单测共同锁定）。
- [ ] `M5-04` Devices 页（可验收：`SCOPE-04`/`SCOPE-02`/`SCOPE-03`/
  `SCOPE-10` 展示面逐项可演示——列表、信任操作面、presence/连接路径徽标）。
- [ ] `M5-05` Conversations 页与聊天窗口（可验收：`SCOPE-05`/`SCOPE-06`/
  `SCOPE-07` UI 面 + 会话内文件卡片逐项可演示——含文本/图片发送与消息
  历史滚动）。
- [ ] `M5-06` Transfers 页（可验收：传输集中列表与暂停/恢复/取消操作面
  可演示；接收根残留 GC 议题实现或显式延后并登记触发条件）。
- [ ] `M5-07` Settings 页与主题（可验收：主题三选按 `DEC-005` 缺口处置
  落地并如实登记，最小设置项可演示）。
- [ ] `M5-08` MVP 全链路验收（可验收：设计第 15 节清单 + `SCOPE-01`~
  `SCOPE-12` 逐项归档；M3 登记补做条件复核闭环；环境受限沿降级纪律）。
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
  （`RULE-10` 公开面仅 std 类型）。
- **CI 无显示环境**：GLFW+OpenGL 渲染不进 CI——UI 逻辑层网络无关单测 +
  渲染层本机手工验证证据归档；不在 CI 宣称的检查不得写入 CI 断言
  （`RULE-11`）。
- **默认主题档位 ≠ aki_ui_design 第 2.1 节表**（`DEC-005` 静态盘点结论）：
  `ui/theme` 装配须逐项覆写并留回归对照，不得依赖上游默认档。
- **主线程 compose 与 executor 关闭顺序**：`onShutdown` 编入 `EXEC-01`
  后，窗口/GPU 设备销毁与 worker 回收次序需测试覆盖（关闭路径 DOD-02）。
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

