# Aki UI 设计规范（ZCode Design System 采用与 EUI-NEO 绑定）

> 状态：Active
> 负责人：Linductor
> 更新日期：2026-09-22
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
  EUI-NEO `theme::panelShadow/popupShadow` 只用于弹层与 Toast。
- 动效：短促 fade/zoom/slide，主工作区无弹簧动画（上游 "Motion"）。
- 三栏 workspace 布局（上游 "Workspace layout"：独立 frame、4px 可调间隙、
  frame 不计圆角层级）对应 Aki 设计第 9 节三栏：导航栏(fixed) + 列表栏
  (fixed) + 内容栏(fill)，EUI-NEO Row/Column 组合实现。

### 2.5 主题模式与无障碍

- Settings 提供浅色/深色跟随系统三选（上游 "Theme Modes"）；两套主题同等
  验证。
- 键盘导航一等公民、状态不得仅用颜色编码、容忍翻译变长（上游
  "Accessibility and Internationalization"）。

## 3. Aki 领域状态的视觉语义

状态机语义以[设计文档](aki_design.md)第 3~7 节为准；视觉全部使用第 2.3 节
语义色 + 图标/文字双重编码：

| 领域状态 | 视觉 |
| --- | --- |
| Presence Online / Offline | 实心/空心圆点（`full`），Online `success`，Offline `subtlest`，旁注 Last seen |
| Trust Unknown / Pending | `subtlest`/`warning` 徽标；Pending 触发信任确认弹窗（mono 展示公钥指纹） |
| Trust Trusted | `success` 徽标，可进入会话 |
| Trust Rejected / Revoked | `destructive` 徽标，会话入口禁用 |
| Delivery Queued/Sending/Sent/Delivered/Failed | 时钟 / 单勾（中性）/ 双勾 `success` / `destructive` + 重试 |
| Conversation Disconnected | 会话头部 `warning` 横条"连接断开，等待恢复"（恢复不新建会话） |
| Transfer 各态 | 文件卡片 + 进度条：Transferring `brand`，Paused `warning`，Failed `destructive`+重试，Completed `success`，Cancelled/Queued 中性 |
| 连接路径 LAN / P2P / Relay | `caption` 中性徽标，路径切换不产生新会话 |

## 4. 页面与 EUI-NEO 组件映射

三栏布局与四页导航按设计第 9 节；左栏导航 Conversations / Devices /
Transfers / Settings。组件选型（`RISK-2026-002` 的盘点基线，M5 用 pinned 版本
实际运行复核）：

| Aki 界面元素 | EUI-NEO 组件 | 上游规则约束 |
| --- | --- | --- |
| 左侧导航栏 | `navbar` | 选中态用对比而非品牌填充 |
| 列表栏 | `scrollview` + `virtuallist` | 行高用 `ui-base` 节奏，悬停 `hover`/选中 `selected` |
| 会话头部 | `text` + 徽标 | 元信息 `text-subtle`，路径/指纹 mono |
| 消息气泡 | `card` + `text` 组合 | 一级容器 `rounded-xl`(12)，己方/对方区分靠 surface 层级 |
| 图片消息 | `image` + `dialog` | 预览弹窗属批准的 `rounded-xl` 例外 |
| 文件卡片 | `card` + `progress` + `button` | 会话内与 Transfers 页复用同一组件 |
| 输入区 | `input` + `button` | 主输入壳可用批准的 `rounded-2xl` 例外 |
| 弹窗 / 右键菜单 / Toast | `dialog` / `contextmenu` / `toast` | 弹窗 `2xl`、菜单壳 `lg`、菜单项 `md`、Toast `2xl`+`shadow-lg` |
| 设置页 | `segmented` / `switch` / `dropdown` | 主题三选（跟随系统/浅/深） |

缺口处理不变：优先原语组合；确需上游能力或贡献时按工程规范以决策记录确认，
不得修改 pinned 依赖。

## 5. 落地与验证

- `ui/theme`：第 2 节映射的常量实现（字号表、间距/圆角/尺寸覆写、深浅两套
  语义色、shadow 档位），提供 light()/dark() 装配。
- `ui/components` / `ui/pages`：按第 3/4 节消费语义名实现；code review 按上游
  "Do / Don't" 清单与"实现指导"检查。
- M5 启动前复审本文件与 pinned EUI-NEO 实际版本的一致性；偏差记入里程碑
  文档。

## 6. 来源与许可

- ZCode Design System：`https://github.com/zai-org/ZCode`，`DESIGN.md`
  @ `872ad960de7ec172591f7e1952f7849229f94521`，归档于
  [zcode-design-system.md](zcode-design-system.md)；色值取自同仓库
  `packages/ui/src/styles.css` 默认 light/dark 主题（`@theme` 与 `.dark` 块）。
- 上游许可 Apache-2.0；归档文件保留来源与许可头部，升级时整篇替换正文并
  更新 commit。
