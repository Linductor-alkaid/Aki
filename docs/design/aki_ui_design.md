# Aki UI 设计规范（ZCode Design System 采用与 EUI-NEO 绑定）

> 状态：Active
> 负责人：Linductor
> 更新日期：2026-10-07
> 权威约束：[ZCode Design System](zcode-design-system.md)（直接采用，见下）
> 上位设计：[Aki 设计方案](aki_design.md)第 9/10 节
> 实现基线：pinned `third_party/EUI-NEO`

## 1. 采用声明

经用户确认（2026-09-22），Aki 的前端 UI 设计约束**直接采用** ZCode 的
`DESIGN.md`，原文归档为 [docs/design/zcode-design-system.md](zcode-design-system.md)
（来源 zai-org/ZCode @ `872ad960`，Apache-2.0）。本文件不复述其规则，只做两件事：

1. 宣告约束地位：`ui/theme`、`ui/components`、`ui/pages` 的一切 UI 变更，
   **先遵循 ZCode Design System，再考虑新造视觉规则**；违反其规则（如绕开
   `text-ui-*` 字号体系、使用一次性色值、随意圆角/阴影）按上游同款定性——
   是设计系统缺陷，不是风格偏好。
2. 把上游为 Web/Tailwind 栈写的令牌与类名**绑定**到 Aki 的 EUI-NEO 实现上
   （第 2 节）。绑定关系变更需先改本文件，再改 `ui/theme`。

## 2. ZCode 约束 → EUI-NEO 绑定映射

以下映射由 `ui/theme` 实现为常量；页面与组件只允许消费本表的语义名，
不允许直接使用魔法数值。

### 2.1 排印（对应上游 "Typography"）

`text-ui-*` 强制字号体系按像素直接落地，基础字号 `--ui-font-size = 14px`
（Aki 的 Settings 可整体缩放该变量，不改根字号）：

| 语义名 | px | 主要用途 |
| --- | ---: | --- |
| `ui-xl` | 18 | 一级阅读标题 |
| `ui-lg` | 16 | 二级阅读标题 |
| `ui-base` | 14 | 正文、消息、按钮、工作区标题（默认） |
| `ui-caption` | 13 | 紧随正文一级的辅助文案 |
| `ui-sm` | 12 | 次要信息、帮助文本、Tooltip、行内代码 |
| `ui-xs` | 10 | 徽标、快捷键、计数、弱元数据 |
| `ui-2xs` | 9 | 仅限图轴刻度类"家具"，禁用于内容（受限例外） |

EUI-NEO `TypographyTokens` 的默认档位不作为 UI 依据；`ui/theme` 以上表覆写。
字重层级（h3-h6 同字号靠 weight 区分）、mono 只用于技术内容（DeviceId、公钥
指纹、路径、速率）等规则按上游原文执行。

### 2.2 间距 / 圆角 / 尺寸（对应上游 "Spacing / Radius / Sizing"）

- 间距：4px 基频，4/8/12/16/20-24 档位 → EUI-NEO `SpacingTokens`
  （tiny 4 / compact 8 / content 12 / section 16 / large 20 / panel 24），
  禁用档外值。
- 圆角嵌套递减规则照上游执行，px 值绑定：
  `rounded-2xl`=16（仅上游批准的例外：主输入壳、Toast、品牌图标底板）、
  `rounded-xl`=12（一级容器）、`rounded-lg`=8（基础控件/菜单壳）、
  `rounded-md`=6、`rounded-sm`=4（最小）、`full`=胶囊/圆专用 →
  覆写 EUI-NEO `RadiusTokens` 至同值。
- 尺寸：图标 12/14/16/20/24（默认 16）；控件高 24/28/32/36 →
  `ControlSizeTokens`（field=36、menuItem=28 起步按上游基线校准），
  不发明新高度体系。

### 2.3 语义色板（对应上游 "Color Palette"，值取自上游 styles.css 默认主题）

`ui/theme` 提供深浅两套 `ThemeColorTokens` + 扩展语义色，值固定为上游
Tailwind 默认调色板取值（hex→归一化在实现时完成）：

| 语义角色 | Light | Dark |
| --- | --- | --- |
| background | `#fafafa` neutral-50 | `#171717` neutral-900 |
| card / popover / menu | `#ffffff` | `#262626` neutral-800 |
| menu-hover | `#f5f5f5` | `#0a0a0a` neutral-950 |
| surface / surface-hover | 3% / 5% fg 叠加 | 5% / 10% 白叠加 |
| border | fg 10% | 白 10% |
| text primary / subtle / subtlest | `#404040`，60% / 40% | `#e5e5e5`，60% / 30% |
| brand | `#38bdf8` sky-400 | `#0ea5e9` sky-500 |
| accent | `#f0f9ff` sky-50 | sky-950 50% 叠加 |
| primary / primary-foreground | `#0a0a0a` / `#fafafa` | `#fafafa` / `#0a0a0a` |
| success | `#16a34a` green-600 | `#22c55e` green-500 |
| warning | `#ca8a04` yellow-600 | `#eab308` yellow-500 |
| destructive | `#dc2626` red-600 | `#ef4444` red-500 |
| hover / selected | fg 10% 内叠加 | 白 10% |

使用纪律照上游 "Color Usage Rules"：只用语义令牌；brand 克制、永不做整面
背景；状态色只表达真实状态；页面背景/卡片/浮层分层不得混用。
注意上游 `primary` 是黑/白（非品牌蓝），主按钮默认 `bg-primary` 而非 brand。

### 2.4 组件 / 深度 / 动效 / 布局

- 按钮五变体（primary/outline/secondary/ghost/destructive）与"不要把所有操作
  升级为 primary"、输入框"calm not glowing"、菜单密集行、Tabs 选中用对比而非
  品牌填充等规则照上游 "Components" 一节执行，落点为 EUI-NEO
  `button`/`input`/`contextmenu`/`tabs` 等组件的参数组合。
- 深度：背景对比 + 边框优先，`shadow-md` 仅浮层、`shadow-lg` 仅 Toast →
  EUI-NEO 侧落点为 `theme::panelShadow`/`popupShadow`（theme.h:266/:270，
  只用于弹层与 Toast——`popupShadow` 经 `fieldVisuals().popupShadow*`
  字段合成，theme.h:126-128/:211-215）。
- 动效：短促 fade/zoom/slide，主工作区无弹簧动画（上游 "Motion"）。
- 三栏 workspace 布局（上游 "Workspace layout"：独立 frame、4px 可调间隙、
  frame 不计圆角层级）对应 Aki 设计第 9 节三栏：导航栏固定 64，列表栏按
  窗口逻辑宽度在 168~264 内调整，内容栏填满余量。此约束使默认窗口在
  2× 缩放设备上仍能容纳主面板控件；页内固定宽度控件以内容栏宽度为上限。

### 2.5 主题模式与无障碍

- Settings 提供浅色/深色跟随系统三选（上游 "Theme Modes"）；两套主题同等
  验证。
- 键盘导航一等公民、状态不得仅用颜色编码、容忍翻译变长（上游
  "Accessibility and Internationalization"）。

### 2.6 图标资源与应用图标

- **界面图标字体**：pinned EUI-NEO 自带
  `assets/Font Awesome 7 Free-Solid-900.otf`，经 `text(...)`/`button(...)`
  的 `.icon(codepoint)` 消费（上游 DSL.md「图标」节；FontAwesome 码点必须
  配套该字体，不依赖系统兜底）。Aki 不引入第二套界面图标来源；图标尺寸按
  第 2.2 节 12/14/16/20/24（默认 16）。码点登记如下（2026-09-27 对
  pinned v0.6.0 捆绑字体 cmap 逐一验证存在；新增图标必须同法验证并登记）：

| 语义点 | 图标（FA7 Solid 名） | 码点 |
| --- | --- | --- |
| 导航 Conversations / Devices / Transfers / Settings | comments / network-wired / arrow-right-arrow-left / gear | `f086` / `f6ff` / `f0ec` / `f013` |
| 信任 Unknown / Pending / Trusted / Rejected / Revoked | circle-question / clock / circle-check / circle-xmark / ban | `f29c` / `f017` / `f058` / `f05a` / `f05e` |
| 投递 Queued / Sending / Sent / Delivered / Failed | clock / paper-plane / check / check-double / circle-exclamation | `f017` / `f1d8` / `f00c` / `f560` / `f06a` |
| 失败重试 | arrow-rotate-right | `f01e` |
| 传输 收/发方向 / Paused | arrow-down-long / arrow-up-long / circle-pause | `f175` / `f176` / `f28b` |
| 路径 LAN / P2P / Relay | network-wired / link / tower-broadcast | `f6ff` / `f0c1` / `f519` |
| 操作 发送 / 附件 / 图片 / 发现开始·停止 | paper-plane / paperclip / image / magnifying-glass·stop | `f1d8` / `f0c6` / `f03e` / `f002`·`f04d` |
| 会话断连横幅 | triangle-exclamation | `f071` |
| 高级折叠展开/收起 | chevron-down / chevron-right | `f077` / `f054` |
| Presence 圆点 | 维持第 3 节 rect 几何实现（实心/空心），不换字体图标 | — |

- **应用图标（程序图标）**：经用户确认（2026-09-27）采用 Heyaki 图标。
  资产由 `scripts/make_app_icons.py` 自 pinned heyaki
  `docs/icon/heyaki-transparent.png` 生成（溯源与许可见
  [assets/icons/README.md](../../assets/icons/README.md)）：
  `assets/icons/aki-icon.png`（256px 窗口/任务栏图标，`dslAppConfig
  .iconPath` 消费）与 `assets/icons/aki-icon.ico`（16~256 多尺寸，Windows
  exe 内嵌 `IDI_APP_ICON`——消费侧覆写 `EUI_NEO_APP_ICON_RESOURCE` 属性，
  替换上游默认 EUI-NEO 图标；不改 pinned 依赖）。

## 3. Aki 领域状态的视觉语义

状态机语义以[设计文档](aki_design.md)第 3~7 节为准；视觉全部使用第 2.3 节
语义色 + 图标/文字双重编码：

| 领域状态 | 视觉 |
| --- | --- |
| Presence Online / Offline | 实心/空心圆点（`full`），Online `success`，Offline `subtlest`，旁注 Last seen |
| Trust Unknown / Pending | `subtlest`/`warning` 徽标；Unknown 行提供连接入口，进入 pairing_restricted 后转 Pending；确认弹窗以 mono 展示公钥指纹（DeviceId 规范串）并以掩码输入对端设备的本机配对口令（DEC-018） |
| Trust Trusted | `success` 徽标，可进入会话 |
| Trust Rejected / Revoked | `destructive` 徽标；会话入口按连接事实开放（DEC-021） |
| Delivery Queued/Sending/Sent/Delivered/Failed | 时钟 / 单勾（中性）/ 双勾 `success` / `destructive` + 重试 |
| Conversation Disconnected | 会话头部 `warning` 横条"连接断开，等待恢复"（恢复不新建会话） |
| Transfer 各态 | 文件卡片 + 进度条：Transferring `brand`，Paused `warning`，Failed `destructive`+重试，Completed `success`，Cancelled/Queued 中性 |
| 媒体消息无传输行（§6.1 单侧到达边角，M5-05） | 文件卡片以 `warning` 文案 "no transfer row (single-side arrival)" 兜底态显式呈现，无进度条——不猜测进度、不以占位冒充（DEC-010/DEC-013）；重启恢复后非终态传输行降级 Paused（DEC-013），按 Paused `warning` 呈现 |
| 连接路径 LAN / P2P / Relay | `caption` 中性徽标，路径切换不产生新会话 |

受限会话已有控制连接，应显示实际 LAN/Relay 信令路径；其消息发送仍受
Heyaki 授权约束。四态文案按 grant 签发方解释：本机输入对端口令后显示
「对方已信任本机」，本机签发 grant 后显示「本机已信任对方」
（[DEC-022](../decisions/DEC-022-link-before-trust-and-grant-direction.md)）。
Settings 修改本设备密码弹窗以**整个窗口**为定位容器，滚动页面不改变中心。
在线但未建链的设备提供「连接」操作；「验证密码」只在已建链的 Pending
设备显示。UI 任务未入队时弹窗保持打开，并在输入框附近展示重试提示；
Adapter 拒绝和异步验证失败都在设备行显示，断连不隐藏已发生的失败。

## 4. 页面与 EUI-NEO 组件映射

三栏布局与四页导航按设计第 9 节；左栏导航 Conversations / Devices /
Transfers / Settings。组件选型（`RISK-2026-002` 的盘点基线，M5 用 pinned 版本
实际运行复核）：

| Aki 界面元素 | EUI-NEO 组件 | 上游规则约束 |
| --- | --- | --- |
| 左侧导航栏 | `rect` 点击面 + `text` 图标/标签组合（沿 `navbar` 的导航语义） | 64px 窄栏内图标和标签纵排；选中态用 primary 对比而非品牌填充；四图标按 §2.6 已登记的捆绑字体码点 |
| 列表栏 | `scrollview` + `virtuallist` | 行高用 `ui-base` 节奏，悬停 `hover`/选中 `selected`（virtuallist 为固定行高模型——`rowHeight` 统一值，M5-01 实测确认；变高气泡列按第 5 节复审结论组合） |
| 会话头部 | `text` + 徽标 | 元信息 `text-subtle`，路径/指纹 mono |
| 消息气泡 | `card` + `text` 组合 | 一级容器 `rounded-xl`(12)，己方/对方区分靠 surface 层级；滚动视口内不设会裁切的阴影 |
| 图片消息 | `image` + `dialog` | 预览弹窗属批准的 `rounded-xl` 例外 |
| 文件卡片 | `card` + `progress` + `button` | 会话内与 Transfers 页复用同一组件 |
| 输入区 | `input` + `button` | 主输入壳可用批准的 `rounded-2xl` 例外 |
| 弹窗 / 右键菜单 / Toast | `dialog` / `contextmenu` / `toast` | 弹窗 `2xl`、菜单壳 `lg`、菜单项 `md`、Toast `2xl`+`shadow-lg` |
| 设置页 | `segmented` / `switch` / `dropdown` | 主题三选（跟随系统/浅/深），中文/英文切换 |
| 设置页中继/TURN 区（M7） | `input` × N + `secureInput`（令牌/凭据）+ `button` | URL/CA 路径/状态行 mono（技术内容）；状态行只读派生自快照，compose 无 IO；错误用 `destructive` |
| 设置页高级折叠区（M8） | `button`（ghost）+ chevron 图标双态 + 条件内容 | 折叠态以 chevron-down/right 图标编码（非仅颜色）；内容渲染由页面持有 UI 态驱动 |

设备页左栏列出对端名称（有备注时优先显示本机备注），右栏只显示选中设备
的身份、连接和信任详情及备注编辑。会话列表的名称与时间、聊天头部名称与
连接信息须分配独立空间；可变长中文标签按剩余宽度约束。

缺口处理不变：优先原语组合；确需上游能力或贡献时按工程规范以决策记录确认，
不得修改 pinned 依赖。

## 5. 落地与验证

- `ui/theme`：第 2 节映射的常量实现（字号表、间距/圆角/尺寸覆写、深浅两套
  语义色、shadow 档位），提供 light()/dark() 装配。
- `ui/components` / `ui/pages`：按第 3/4 节消费语义名实现；code review 按上游
  "Do / Don't" 清单与"实现指导"检查。
- M5 启动前复审本文件与 pinned EUI-NEO 实际版本的一致性；偏差记入里程碑
  文档。
- **M5-01 一致性复审记录（2026-09-26，pinned v0.6.0 @ `b9032a8a`；探针实测
  见 M5 里程碑 M5-01 验证记录）**：
  - 第 2 节绑定映射 16 组件在 pinned v0.6.0 全部存在
    （components/components.h 伞头导出；navbar/scrollview/virtuallist/text/
    card/image/dialog/progress/button/input/contextmenu/toast/segmented/
    switch/dropdown + 徽标为 text 组合）——一致。
  - §2.1 排印：`TypographyTokens` 默认档（micro 11/caption 12/hint 13/
    label 14/body 16/subtitle 20/title 22）≠ 本表（9/10/12/13/14/16/18）
    ——**偏差确认，`ui/theme` 逐项覆写**（探针实测覆写值落盘）。
  - §2.2 间距：`SpacingTokens` tiny 4/compact 8/content 12/section 16/
    large 20/panel 24 与本表**一致**（探针对拍零覆写）；圆角/尺寸默认档
    存在偏差（radius.small 6→4、control.field 35→36、menuItem 34→28）——
    覆写。
  - §2.4 深度锚点复核（2026-09-26 评审纠正）：本记录先前「v0.6.0 无
    `panelShadow/popupShadow` 独立函数」断言与 pinned 源不符、撤回——
    两独立函数存在（theme.h:266/:270，自上游 d28609fb 2026-04-28 即在）；
    `fieldVisuals().popupShadow*`（theme.h:126-128/:211-215）为其合成
    字段、锚点属实。§2.4 落点已恢复为 `panelShadow`/`popupShadow`
    （弹层与 Toast）。
  - §3/§4 状态视觉与页面映射：语义色字段名与 `ThemeColorTokens`
    （background/primary/surface/surfaceHover/surfaceActive/text/border +
    metrics）对齐；扩展语义色（success/warning/destructive/brand 等）不在
    上游结构内，由 `ui/theme` 以扩展常量承载（上游 Color + 自有语义名）。
  - 组合模型（M5-01 探针实测，装配契约见设计 §9）：**compose 为保留模式、
    事件触发**——静态 UI 不重复重组（探针 12s 仅 2 次 compose）；页面持有
    状态 + `app::requestUpdate()` 跨线程唤醒 → 主线程重组拾取新状态
    （探针 waker 线程翻转 dialog/toast 状态 + requestUpdate，13s 内 11 次
    开合转换全部拾取）。dialog `open(bool)`/toast `visible(bool)+
    bindVisible(Signal)` 页面持有形态确认。
  - 文件对话框：`eui::platform::openFileDialog` 只读打开（平台能力文档：
    不支持目录/保存）——满足发送选取链路（图片发送 open 选取 + hash-first
    发起），接收侧按接收根无对话框需求——满足度确认。

- **M5-05 落地记录（2026-09-27，Conversations 页与聊天窗口；本机 GUI 实测
  与探针定形）**：
  - §4 组件映射逐项落地：会话列表=scrollview 行（行内绝对排版 + 透明点击
    面，非 virtuallist——预算 256 行有界）、消息气泡=card(wrapContentHeight)
    + text(wrap) 组合（M5-01 复核结论「卡片自绘 + scrollview」实测成立：
    变高气泡列由 scrollview 内容列 wrapContent 度量，嵌套 wrap 布局引擎
    原生支持）、图片消息=image+dialog、文件卡片=card+progress+button、
    输入区=input+button（图标按钮 FA 码点 §2.6）。
  - 探针定形实测结论（本机 GUI 会话）：① scrollview 内容列为纵排布局、
    掌管子元素 x——气泡左右归属（己方 accent 右侧 / 对方 card 左侧，§4
    surface 层级区分）必须在行内 stack 绝对定位，直接把 card 挂进内容列
    会被列布局拉回左缘；② pinned scrollview 运行期滚动状态按元素 id 持有
    （首次构建播种 offset、其后运行期所有）——「回到底部」以滚动代数进位
    切换 scrollview id 表达（选中切换/新消息入流 +1），用户滚动位置在两次
    代数进位之间由运行期保持；③ dialog open 态页面持有 + requestUpdate
    唤醒重组拾取（M5-01 waker 契约）在预览/新建会话弹窗复验成立。
  - 会话头部断连横幅（§3 Conversation Disconnected）以 `warning` 底色 +
    三角叹号图标 + 「连接断开，等待恢复」文案落地；信任徽标 Trusted
    `success` / Rejected·Revoked `destructive`。此处“会话入口禁用”为
    M5-05 历史落地记录；现行契约按 DEC-021/DEC-022 的连接事实门控。

- **M5-06 落地记录（2026-09-27，Transfers 页；组件复用契约兑现）**：
  - §4「文件卡片 | card + progress + button | 会话内与 Transfers 页复用
    同一组件」实体化为共享单元 `ui/components/transfer_card`（命名空间
    `aki::ui::widgets`——避让 EUI `::components`）：卡片本体（方向箭头/
    文件名/大小·mime/进度条/状态文案）+ `format_bytes`/传输态语义色
    （§3）随迁，形状与语义色自 M5-05 ③ 原样迁移；会话气泡卡片与
    Transfers 行共同消费。
  - Transfers 页操作面：Pause/Resume/Cancel 按 TransferView 状态门控
    （§7 固定边派生：Transferring→Pause、Paused→Resume、非终态→Cancel；
    Paused 行 Cancel = DEC-013⑥ 无会话行直接终态入口的 UI 触达，GUI
    实测 Paused 行经 Cancel 即转 Cancelled 中性态）；admission 拒绝经
    反馈行可见；孤儿接收行 re-push 触发面 = 页脚登记披露（无对应
    Manager 出站接口，不冒充可用动作——M5-04 分期披露同款形态）。

- **M5-07 落地记录（2026-09-27，Settings 页与主题三选；§4 Settings 页
  segmented 映射落地）**：
  - 主题三选（跟随系统/浅/深）= `segmented` 三段 + 页面持有 UI 态
    （`ThemeSetting`），生效经 `akiTheme()/akiSemanticColors()` 装配面
    （§2 映射与覆写清单不变——仅档位选择，aki_theme_values 回归对照
    维持）；深浅两档全壳渲染实测（GUI 截图对照）。
  - 上游 `SegmentedStyle` 深色档选中文字与 indicator 同为 primary（白上
    白不可见，GUI 实测）——选中文字覆写 §2.3 primary-foreground 配对
    （页内 style 覆写，非 ui/theme 档位变更）。

- **M5-10 视觉修整（2026-09-28）**：四个导航项消费 §2.6 已登记的 FA7
  Solid 码点，保留文字标签；列表栏摘要改为当前页对应计数，移除开发阶段
  占位语，标题、摘要和分隔线按字号/间距档位重新排布。Devices 操作按钮、
  Transfers 页说明和 Settings 主题三选在窄窗口内按可用宽度布局，空态与
  恢复提示可换行；GUI 在 1080×720 与 1600×900 物理窗口（本机 2×
  缩放）及深浅两档复核。pinned EUI-NEO `ButtonStyle` 深色 primary 按钮
  默认文字仍用浅色，造成白底白字；Aki 所有 primary 按钮在消费侧显式
  覆写 `primary-foreground`（图标按钮覆写 iconColor），不改上游。
  - 「跟随系统」= 平台条件编译单元 `app/lifecycle/system_theme`
    （Windows `AppsUseLightTheme` 用户偏好；其余平台/查询失败 Unknown
    回落 Light 并页内披露）；主题选择会话级（不跨启动持久化，登记披露）。
  - 最小设置项（只读展示）：数据目录（HostRuntime 装配面）与本地设备 id
    （快照）以 mono caption 呈现（§2.1 mono 用于路径/技术内容）。
  - 页脚分行修正（2026-09-27 独立评审发现）：Transfers 页经 main_window
    以 y=0 全高调用，登记披露行原锚点（height-caption-section）与跨页
    反馈行（main_window 页尾同一锚点）完全重合——上条 GUI 归档证据
    （aki-m5-06-cancel-clicked.png）中反馈文字与披露文字叠印不可辨读，
    「admission 拒绝经反馈行可见」恰在拒绝场景不成立。修复：披露行上移
    一行（caption 行高 + tiny 间距），列表底预留同步扩为两行页脚，反馈行
    锚点不动；GUI 重验反馈行与披露行两行并存、各自可辨读
    （aki-fix-footer-4-files-split.png 及页脚裁切归档 `build/scratch/`）。

## 6. 来源与许可

- ZCode Design System：`https://github.com/zai-org/ZCode`，`DESIGN.md`
  @ `872ad960de7ec172591f7e1952f7849229f94521`，归档于
  [zcode-design-system.md](zcode-design-system.md)；色值取自同仓库
  `packages/ui/src/styles.css` 默认 light/dark 主题（`@theme` 与 `.dark` 块）。
- 上游许可 Apache-2.0；归档文件保留来源与许可头部，升级时整篇替换正文并
  更新 commit。
- Heyaki 应用图标：源图取自 pinned heyaki `docs/icon/heyaki-transparent.png`
  （@ `e114508a`，v1.0.1-38-ge114508），MIT（`third_party/heyaki/LICENSE`）；
  Aki 侧生成物、溯源与再生成见 [assets/icons/README.md](../../assets/icons/README.md)。
- Font Awesome Free Solid（界面图标字体）：随 pinned EUI-NEO assets 分发，
  许可以 EUI-NEO 仓库内标注为准（发行前资产许可审计登记项，锁文件
  `used_by` 已注）。

### 2026-10-01：图片与本地文件呈现（M5-38/39，DEC-026）

图片气泡默认显示 `image` 的 Contain 缩略图，最高 240px（4px 基频），
点击预览打开整窗居中的现有 dialog。文件名、传输状态和保存路径同时可见，
未归档显示“正在保存文件”，归档文件缺失显示“本地文件已不存在”。
本地路径使用 ui-sm 和 mono；聊天卡片换行显示完整路径，Transfers 固定行
在路径过长时显示尾段并提供“复制路径”和“打开文件夹”。操作按钮为 secondary，
不额外增加卡片层级。Transfers 行高由原文件信息区、路径行和控件令牌相加，
路径区按排印和控件令牌扩展。打开文件夹仅交给系统文件管理器，不执行收到的文件。

## 2026-10-02：会话列表管理（M5-42 / DEC-027）

选中会话头部提供文字按钮“置顶/取消置顶”和“移除会话”；离线历史也可
选中操作。置顶列表行以“已置顶”文字标识，列表按组和新消息接受顺序排序。
移除保留历史，不使用确认弹窗；提交拒绝显示反馈，快照隐藏后清空选择。
会话名、预览和时间分行，时间容忍中文完整日期并换行；消息气泡时间
同样换行且为己方送达标识预留独立空间。列表断开状态并入预览前缀，
与送达图标分开。发送按钮在无连接路径时禁用，Enter 明确反馈断开；
可以保留草稿。全部字号/间距/圆角/颜色沿令牌。
日期与中英文规则以 DEC-027 为准，重绘时按本地时区计算。

## 2026-10-06：中继服务器与 TURN 高级设置（M7，DEC-028）

Settings 页新增两个区块，随滚动视口纵向排布，全部字号/间距/圆角/颜色沿
令牌。中继区块：标题 + 快照只读状态行（mono；未注册 / 已连接:url /
已注册待重启）+ 可选 `destructive` 错误行；未注册时展示注册向导（地址
input、租户 input、准入令牌 secureInput、注册 primary 按钮、CA 证书路径
input 与用途说明），已注册时展示移除按钮与重启生效说明。注册/移除经
UiActions 出站面（组合根绑 HostRuntime），静态校验失败在
last_action_feedback 可见；注册网络结果异步经 RelayStatus 状态行呈现，
token/凭据为会话内秘密，提交后即 clear_secret 擦除草稿。TURN 高级区块：
主机/端口/用户名 input + 凭据 secureInput + 保存按钮；凭据不回填（保存时
须重新输入），端口校验 1-65535。两区块变更均重启生效（HEY-20261006-001），
文案如实披露。Devices/会话路径徽标沿用既有 ConnectionPath 标签，Relay
路径无需新增资产。

## 2026-10-07：中继高级设置折叠（M8-01，DEC-028 决策 11 阶段 1）

未注册态中继区主视图收拢为「地址 input + 注册 primary 按钮」一行；租户/
准入令牌/证书路径收拢进新增的高级折叠区（默认收起）。折叠开关 =
`ui/components/fold.hpp` 组合单元：ghost 按钮承载 chevron-down（展开）/
chevron-right（收起）图标双态（§2.6 登记码点 `f077`/`f054`，2026-10-07
以最小 cmap 解析器对 pinned 捆绑 FA7 Solid 实证存在，映射码位 2929），
开合为页面持有 UI 态（`settings_relay_advanced_open`），onClick 仅翻转
标志、重组拾取，compose 无 IO。注册必填校验失败时自动展开折叠区并反馈
缺失字段（用户直接落在待填字段上，状态非仅颜色编码）。高级区内租户 +
令牌同行、证书独占一行，令牌仍走 secureInput；token 制部署能力零删减。
阶段 2（上游密码准入落地后）主视图换为地址 + 密码，租户由 Adapter 层
落默认值。TURN 高级区本阶段不变。
