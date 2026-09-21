# Aki UI 设计规范

> 状态：Active
> 负责人：Linductor
> 更新日期：2026-09-21
> 上位设计：[Aki 设计方案](aki_design.md)第 9/10 节
> 实现基线：pinned `third_party/EUI-NEO`（组件与主题令牌以其本地源码为准）

## 1. 目的与来源

本文件固定 Aki 桌面客户端的 UI 设计语言，约束 `ui/theme`、`ui/components`、`ui/pages`
的实现，对应总计划 `SCOPE-12`。它回答"令牌如何选用、状态如何呈现、组件如何组合"，
不重复 EUI-NEO 的 API 细节。

规范来自三个来源，冲突时按以下优先级取舍：

1. [Aki 设计方案](aki_design.md)：三栏布局、四页导航、状态集合与状态边界（`RULE-02`）。
2. EUI-NEO pinned 源码（`components/theme.h`、`docs/布局.md`）：可用的设计令牌与布局
   原语，公开 API 优先于本地封装。
3. [ZCode Design System](https://github.com/zai-org/ZCode/blob/main/DESIGN.md)
   （zai-org/ZCode 仓库 `DESIGN.md`，2026-09-21 抓取）：交互密度、语义令牌、层级与
   组件使用纪律的参考基准。只借鉴其设计原则与规则，不引入其代码与素材。

EUI-NEO 没有现成的语义令牌层：`components/theme.h` 提供度量令牌
（`TypographyTokens`/`SpacingTokens`/`RadiusTokens`/`ControlSizeTokens`）与
`ThemeColorTokens`（background/primary/surface/surfaceHover/surfaceActive/text/border，
light()/dark() 两套）。Aki 在 `ui/theme` 之上补一层语义映射（第 3 节），页面与组件只
允许消费语义角色，不允许散落原始色值。

## 2. 设计原则

Aki 是长期运行的通信与设备协作工具，基准气质沿用 ZCode 的"calm, dense, operational"：

- 面向长时间会话与高信息密度，装饰让位于可读性；不用营销页式的大间距、大面积品牌色
  和装饰性渐变。
- 层级优先用文字层级与背景对比表达，其次边框，最后阴影；普通布局不依赖大阴影。
- 状态色只表达真实语义状态（在线、信任、送达、传输），不做视觉强调的 borrowed 色。
- 深浅两套主题同等对待：任何页面不允许只在单一主题下验证。
- 键盘可达、图标不单独承载语义、为更长的翻译文本预留布局容差（ZCode a11y/i18n 规则）。
- 动效克制：短促的 fade/slide 表达状态变化即可，无长弹簧动画。

## 3. 语义色板

EUI-NEO `ThemeColorTokens` 提供七个原始槽位。Aki 的语义角色固定映射如下，
`ui/theme` 负责实现这层映射：

| 语义角色 | EUI-NEO 令牌 | 用途 |
| --- | --- | --- |
| App Background | `background` | 三栏布局的窗口底色（导航栏、列表栏） |
| Surface | `surface` | 聊天气泡、消息卡片、文件卡片等内容容器 |
| Surface Hover / Active | `surfaceHover` / `surfaceActive` | 列表行、菜单项的悬停与按下 |
| Text Primary / Secondary / Muted | `text` 的不透明度梯度（0.98 / 0.72 / 0.55） | 正文、元信息、弱提示，参照 `pageVisuals()` 现有梯度 |
| Border | `border` | 分隔线、容器描边 |
| Brand / Accent | `primary` | 主按钮、选中指示、发送按钮；不作为整面填充 |

语义扩展色（EUI-NEO 未内置，`ui/theme` 中以固定色值定义深浅两套，随主题切换）：

| 语义角色 | Light | Dark | 用途 |
| --- | --- | --- | --- |
| `success` | 绿 22,163,74 | 绿 74,222,128 | `Trusted`、`Delivered`、`Completed`、在线 |
| `warning` | 琥珀 217,119,6 | 琥珀 251,191,36 | `Pending` 信任确认、`Paused`、`Negotiating` |
| `destructive` | 红 220,38,38 | 红 248,113,113 | `Rejected`/`Revoked`、`Failed`、破坏性操作 |
| `neutral` | 灰（取 `text` 55% 透明度） | 同左 | `Unknown` 信任、离线、`Cancelled`/`Queued` |

使用纪律（继承 ZCode Color Usage Rules）：

- 页面与组件只允许引用上述语义角色；禁止在页面代码里写一次性色值。
- 品牌色克制使用：用于主操作与选中态，永不做整页背景。
- 状态色不得挪作装饰（不用 success 绿去"提亮"一个普通区块）。
- 状态不允许仅用颜色编码，必须伴随图标或文字（第 6 节）。

## 4. 文字排印

EUI-NEO `TypographyTokens` 是唯一字号来源（micro 11 → hero 42，含 lineGap 系列），
Aki 固定角色映射，页面按角色取值：

| 语义角色 | 令牌 | 应用位置 |
| --- | --- | --- |
| 页面标题 | `title`(22) / `heading`(24) | 各页头部标题 |
| 区块标题 | `subtitle`(20) / `cardTitle`(19) | 分组标题、卡片标题 |
| 正文 / 消息正文 | `body`(16) | 聊天消息正文、设备详情字段 |
| 控件文字 | `option`(15) / `label`(14) | 按钮、导航项、列表行主文字 |
| 元信息 | `caption`(12) / `hint`(13) | 时间戳、OS/连接路径副行、占位符 |
| 微标签 | `micro`(11) | 徽标、计数、传输速率 |

规则：

- 层级尽量用"同一字号 + 字重 + 颜色梯度"表达（ZCode：size 与 color hierarchy 是独立
  决策），不要为每个层级发明新字号。
- `DeviceId`、公钥指纹、文件路径、传输速率/字节数、快捷键提示使用等宽字体渲染
  （ZCode：mono only where content is inherently technical）。
- 界面文字必须容忍翻译变长：不依赖固定宽度截断作为唯一布局手段。

## 5. 间距、圆角与尺寸

- 间距只用 `SpacingTokens` 档位（tiny 4 / small 6 / compact 8 / control 10 / content 12
  / section 16 / large 20 / panel 24），对应 ZCode 4px 基频节奏：4 紧凑图标间距、
  8 控件内边距、12 列表行、16 卡片内边距、20-24 区块内边距。
- 圆角遵循 ZCode 的"可见圆角容器嵌套递减"原则，落地到 `RadiusTokens`：一级容器
  `card`(12)，弹层 `popup`(10)，内嵌控件 `control`(8) 及以下；同级容器同圆角；
  `full`(999) 仅用于胶囊形（presence 圆点、pill 徽标）。
- 控件高度用 `ControlSizeTokens`：输入 `field`(35)，菜单行 `menuItem`(34)，紧凑控件
  `compact`(28)，进度条 `progress`(14)；不发明新的高度体系。
- 布局原语用 EUI-NEO Row/Column/Stack/Flow + `SizeValue::fixed/wrapContent/fill`
  （`docs/布局.md`）；三栏结构 = Row[导航 Column(fixed) + 列表 Column(fixed 或可调
  fixed) + 内容 Column(fill)]。

## 6. 状态的视觉语义

领域状态（设计第 3~7 节）到视觉的映射固定如下；状态机语义变更必须先改设计文档，
再同步此处：

| 领域状态 | 视觉 |
| --- | --- |
| `PresenceState::Online / Offline` | 实心/空心圆点（●/○，`full` 圆角），在线 `success`，离线 `neutral`；旁注 "Last seen ..." |
| `TrustState::Unknown` | `neutral` 徽标 "未信任"，操作按钮为"信任设备"主操作 |
| `TrustState::Pending` | `warning` 徽标 + 确认弹窗（展示名称、类型、DeviceId、公钥指纹） |
| `TrustState::Trusted` | `success` 徽标 "已信任"，可进入会话 |
| `TrustState::Rejected / Revoked` | `destructive` 徽标 "已拒绝 / 已撤销"，会话入口禁用 |
| `DeliveryState::Queued / Sending` | `neutral` 时钟图标 |
| `DeliveryState::Sent` | `neutral` 单勾 |
| `DeliveryState::Delivered` | `success` 双勾 |
| `DeliveryState::Failed` | `destructive` 感叹号 + 重试操作 |
| `ConversationState::Disconnected` | 会话头部显示 `warning` "连接断开，等待恢复" |
| `TransferState` | 文件卡片 + `progress` 进度条；`Transferring` 品牌色进度，`Paused` `warning` + 继续/取消，`Failed` `destructive` + 重试，`Completed` `success` + 打开/定位，`Cancelled` `neutral` |
| 连接路径 LAN / P2P / Relay | 会话头部 `caption` 徽标（`neutral`），路径切换不产生新会话（`RULE-06`） |

## 7. 页面与组件映射

三栏布局与四页导航按设计第 9 节执行（左栏导航：Conversations、Devices、Transfers、
Settings）。EUI-NEO 组件能力盘点（对应总计划 `RISK-2026-002` 的初步盘点；M5 启动前
用 pinned 版本实际运行验证，偏差记入里程碑文档）：

| Aki 界面元素 | EUI-NEO 组件 | 备注 |
| --- | --- | --- |
| 左侧导航栏 | `navbar` | fixed 宽度，四项 + 图标 |
| 列表栏（会话/设备/传输） | `scrollview` + `virtuallist` | 大消息历史与设备列表用虚拟化 |
| 会话头部 | `text` + 徽标组合（`card`/`text`） | 设备名 + OS/路径/Presence 副行 |
| 消息气泡 | `card` + `text` 组合 | 无现成气泡组件，左右对齐用布局区分，己方/对方用 `primary` 弱填充 vs `surface` |
| 图片消息 | `image` | 点击看大图用 `dialog` |
| 文件卡片 | `card` + `progress` + `button` | 会话内嵌 + Transfers 页复用同一组件 |
| 输入区 | `input` + `button` | 多行输入用 `input` 的 multiline 能力，M5 验证 |
| 信任确认弹窗 | `dialog` | 展示公钥指纹（mono），主/次按钮层级 |
| 传输完成/失败提示 | `toast` | 状态色图标 + 文字 |
| 右键菜单 | `contextmenu` | 会话/设备/传输行操作 |
| 设置页 | `switch` / `dropdown` / `segmented` / `checkbox` | 主题切换（light/dark）用 `segmented` |
| 后续 Agent 消息卡 | `markdown` + `button` | 设计第 13 节，非 MVP |

组件缺口处理：缺的组件优先用基础原语组合（如气泡 = card+text）；确需引入 EUI-NEO
新能力或上游贡献时，按工程规范以决策记录确认，不得 fork 改动 pinned 依赖。

## 8. 落地范围

- `ui/theme`：语义色板（第 3 节）+ 令牌默认值装配（`theme.h` 的 light()/dark() +
  `ThemeMetricTokens`），深浅主题一键切换。
- `ui/components`：消息气泡、文件卡片、状态徽标、设备列表行等组合组件。
- `ui/pages`：Conversations / Devices / Transfers / Settings 四页 + 三栏外壳。
- 实施节奏按总计划：M1 交付无 UI 冒烟宿主，M5 集成 EUI-NEO 并按第 15 节验收
  `SCOPE-12`；本规范在 M5 启动前复审一次（对照 pinned EUI-NEO 实际版本）。

## 9. 参考

- ZCode Design System：`https://github.com/zai-org/ZCode`（`DESIGN.md`，2026-09-21
  抓取）。借鉴：语义令牌纪律、文字角色表、4px 间距节奏、圆角嵌套递减、克制阴影与
  动效、a11y/i18n 规则。未借鉴：其 Tailwind/shadcn 技术栈与具体色值（Zai 主题色板）。
- EUI-NEO：`third_party/EUI-NEO/components/theme.h`、`components/` 组件清单、
  `docs/布局.md`、`docs/模块.md`。
