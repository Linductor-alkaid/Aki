# DEC-005：EUI-NEO 集成方式

> 状态：Accepted
> 日期：2026-09-26
> 负责人：Linductor
> 冻结里程碑：M5 开始前（提前于 M4-06 会话冻结——调研与静态验证已完成）
> 替代/被替代：无（总计划暂定默认值表同条目转正）

## 背景与问题

M5（EUI-NEO UI 与 MVP 验收）开工前必须冻结 UI 框架集成方式、构建开关与
并发边界；`RISK-2026-002`（组件能力与三栏布局匹配度）需静态盘点收口。

## 决策

- **集成方式**：pinned submodule（`third_party/EUI-NEO` @ `b9032a8a`，
  v0.6.0，Apache-2.0——[DEC-003](DEC-003-dependency-locking.md) 已锁，锁
  文件已登记）+ 单一构建图 `add_subdirectory(third_party/EUI-NEO)` +
  `eui_neo_configure_app()` 接入 aki exe（GLFW+OpenGL 默认后端，无需
  Vulkan SDK/SDL2 系统依赖）；`eui::neo` 仅由 `aki_ui` 链接。不用 WebView、
  FetchContent、Release SDK、包管理器（备选否决见下）。
- **构建开关冻结**（Aki 侧 CACHE FORCE 显式写死，不依赖上游默认值）：
  `EUI_DEPS_MODE=bundled`、`EUI_BUILD_APPS/EUI_BUILD_USER_APPS/
  EUI_BUILD_TEST_FIXTURES=OFF`、`EUI_ENABLE_INSTALL/EUI_ENABLE_MODULES=OFF`、
  `EUI_WINDOW_BACKEND=glfw`、`EUI_RENDER_BACKEND=opengl`。子目录消费面
  单一 static target `eui_neo`（alias `eui::neo`，cxx_std_17 PUBLIC，可被
  Aki C++20 消费；其 C++17 限目录作用域）。
- **并发边界冻结**：Aki **不使用** EUI-NEO `app::async`/`core::network`/
  `audio`（框架线程池仅在 beginTask 懒启动，不调用即不建线程）——业务异步
  全走 pinned executor（AGENTS 强制并发基础设施）；跨线程唤醒经公共 API
  `app::requestUpdate()`（原子标志 + GLFW postEmptyEvent，线程安全）——
  executor 侧发布 AppState（EXEC-03 通道）后唤醒，compose 在主线程排空；
  `ExecutorOwner` 关闭编入 `DslAppConfig::onShutdown` 钩子（主窗口 GPU 设备
  销毁前回调，满足 EXEC-01「非 worker 线程 shutdown(true)」）。无 Executor
  能力缺口，无需 9.4 台账。
- **RISK-2026-002 静态盘点结论**：aki_ui_design.md §4 映射表 16 组件在
  pinned v0.6.0 全部存在并经 components/components.h 公开导出；theme tokens
  （Typography/Spacing/Radius/ControlSize/双色板/shadow）齐备可覆写（默认
  档位 ≠ §2.1 表，须逐项覆写）。已知缺口（不阻塞冻结）：无系统主题检测
  API（Settings「跟随系统」需 Aki 平台层查询或先交付浅/深两档）；剪贴板
  仅内部面（input 粘贴自动可用）；文件对话框仅只读打开（满足 MVP 发送
  选取，接收侧按接收根无需保存对话框）；virtualList 固定行高模型（变高
  气泡列 M5 运行复核）；dialog/toast 需页面持有 open 状态（与单向数据流
  一致）。

## 备选方案

- **WebView（CEF/WebView2）**：设计 §9 已否（组件化 C++ 模型直接对应）；
  第二技术栈 + 独立进程模型 + 宿主生命周期复杂度。维持否决。
- **FetchContent**：configure 期网络拉取且上游示例 GIT_TAG 漂移，违反
  DEC-003 pinned + configure 校验纪律与 DEC-006「不静默联网」先例。否决。
- **Release SDK / find_package**：二进制 provenance 不可审计，三 sanitizer
  preset 需重造 SDK（沿 DEC-006 否决 heyaki SDK 的同一理由）；assets/许可
  审计失去源码面。否决，保留为未来分发形态选项。
- **vcpkg/Conan**：与 DEC-003 纪律不一致。否决。
- **`EUI_BUILD_SHARED=ON`**：单 exe 全静态更简，无收益。否决。
- **使用 EUI-NEO 自带 app::async/core::network 承载业务并发**：框架池对
  Executor 生命周期与监控不可见，违反 EXEC-03；仅在 Aki 用法面任务数为 0
  （不调用即不建线程）可接受——写为决策禁用条款。

## 影响与风险

（实现归 M5 首工作项；本记录冻结方式与边界。）必须验证/先决项：

- **运行复核**（冻结调研只完成 configure + `eui_neo.lib` Debug 编译——
  MSVC 19.44/CMake 4.1.0、8 项 bundled 第三方零联网命中、render backend
  resolved=opengl；未运行任何窗口/渲染程序，如实声明）：pinned v0.6.0
  三栏布局与 16 组件在 Windows 实机逐项运行复核（RISK-2026-002 关闭条件）；
  §2 令牌覆写实际可覆写性逐项验证。
- **CI 扩面**：Linux 三档需系统依赖（mesa/X11/Wayland/xkbcommon/glib2
  或 `-DEUI_ENABLE_TRAY=OFF`、libcurl——非 Windows REQUIRED）；EUI 内部
  第三方 target 不继承第一方 -Werror 门禁需实测；tsan 档对 eui_neo 插桩
  与否须决策并记录。
- **MinGW**：EUI-NEO 硬性要求 GCC≥12 且 static runtime 探测失败即 FATAL；
  Aki MinGW preset 本就 executor configure-only 受限，被阻断属预期，按纪律
  记录。
- **宿主入口重构**：`eui_neo_configure_app` 注入框架 main（/SUBSYSTEM:
  WINDOWS）；现 console 宿主迁移为 dslAppConfig()+compose() 钩子；无显式
  onStart 钩子——ExecutorOwner 启动/装配/Node 启动的触发点（如首次 compose
  惰性初始化）须明确并与 onShutdown 组成关闭路径测试（DOD-02 六项）；
  测试 exe 不链 eui 保持 console。
- **跨线程契约**：requestUpdate 唤醒路径纳入 tsan；EXEC-03 通道只在主线程
  compose 内排空；禁用面（app::async/core::network/audio）列入 review
  检查项。
- **单图符号冲突**：沿 DEC-006 sqlite 先例，链接后 dumpbin/nm 核对 heyaki
  栈与 eui 栈无重复符号。
- **资产与许可**：EUI assets POST_BUILD copy 到 exe 旁（CI/测试运行目录
  就位确认）；其 3rd/ 与 assets/ 发行前审计为 DEC-003 保留项，不因本决策
  豁免。
- **上游默认漂移**：开关 CACHE FORCE 不依赖上游 CMakeLists 现状。

## 验证方式

冻结调研（2026-09-26，负责人 Linductor）：集成指南/锁文件/submodule 状态
（b9032a8a v0.6.0 实测）、子目录 CMake 消费面、框架线程/唤醒/关闭钩子源码、
组件与 theme 头文件逐项核对为静态证据；configure + eui_neo.lib 编译为动态
证据（调研会话内执行，通过）。

**M5-02 回填（2026-09-27，「影响与风险」验证项逐项履行；详见 M5 里程碑
M5-02 验证记录）**：

- **运行复核**：M5-01 探针（窗口/compose/主题覆写/waker 唤醒）+
  M5-02 GUI 宿主本机会话（aki.exe Release：装配 ok 32/54/60ms、主题对拍
  落盘、三栏壳+四页导航渲染截图
  `build/scratch/aki-m5-02-window.png`、taskkill WM_CLOSE 优雅关窗 →
  onShutdown 完整 8 步钩子序 + fully_stopped=1 workers 2/2，日志
  `build/release/Release/aki-run.log`）；RISK-2026-002 运行复核收口。
- **CI 扩面**：[DEC-014](DEC-014-eui-tsan-coverage.md)（tsan 全图插桩零
  豁免 + Linux 三档完整系统依赖集 + 覆盖声明五条）；ci.yml 已落地，
  CI 门禁全绿证据随 M5-02 PR（本会话未建分支/未推送，如实声明）。
- **MinGW**：Aki presets 无 MinGW 档（M3-01 起 w64devkit 受限）；EUI-NEO
  硬性要求 GCC≥12 + static runtime 探测失败即 FATAL（其 CMakeLists.txt:
  98-103），被阻断属预期——M5-02 按纪律记录，不建立 MinGW preset。
- **宿主入口重构**：dslAppConfig()+compose() 钩子迁移完成；启动触发点 =
  首次 compose 惰性装配（HostRuntime，设计 §9.1 首帧装配例外条款）；
  onShutdown 组成关闭路径测试（test_host_runtime，DOD-02 六项 + §8.3
  钩子原序断言）+ GUI 本机日志证据；测试 exe 不链 eui 保持 console
  （aki_host_smoke/test_host_runtime 经 dumpbin /SYMBOLS 核查 eui/glfw
  符号 0 命中）。
- **跨线程契约**：requestUpdate 唤醒路径设计锚定（M5-01 waker 11/11 实测；
  M5-03 状态消费面接入后随 tsan 运行期证据归档——DEC-014 口径）；EXEC-03
  通道主线程排空（HostRuntime quiesce/load_state_snapshot 单线程形态）；
  禁用面（app::async/core::network/audio）grep 0 命中（退出-3）。
- **单图符号冲突**：dumpbin /SYMBOLS 核对——eui_neo.lib 含 heyaki 栈符号
  （sqlite3_/usrsctp/rtc::）0 命中；heyaki 侧 lib 含 eui 栈符号
  （glfw/freetype/FT_Init/md4c_/stbi__）0 命中；Release 全量链接
  LNK4006/重复符号告警 0；aki.exe 依赖闭包=系统 DLL+OpenSSL 双 DLL
  （dumpbin /DEPENDENTS），全静态单 exe。
- **资产与许可**：EUI assets POST_BUILD copy 就位确认
  （build/release/Release/assets/ 随 eui_neo_configure_app(aki) 生成，
  icon/fonts/shaders 齐备）；3rd/ 与 assets/ 发行前审计仍为 DEC-003
  保留项（M5-09 复核）。
- **上游默认漂移**：八项开关 CACHE FORCE 写死于根 CMakeLists.txt
  （configure 日志：bundled glfw/glad/tray/freetype/zlib/libpng/md4c/
  miniaudio 全 bundled、render resolved=opengl）。

RISK-2026-002 运行复核随 M5-01/M5-02 收口（见上）。

## 关联文档和工作项

- [Aki 设计方案](../design/aki_design.md)§9（GUI）、§8（应用结构）、§14
- [DEC-003](DEC-003-dependency-locking.md)（submodule+锁文件纪律）、
  [DEC-006](DEC-006-heyaki-api-contract.md)（单一构建图/开关冻结/静态冻结
  调研先例）、[DEC-009](DEC-009-appstate-write-path.md)（EXEC-03 通道）
- [Aki UI 设计规范](../design/aki_ui_design.md)（组件映射与令牌约束的
  权威约束面）
- [Aki 实施总计划](../plans/aki-implementation-plan.md)（`SCOPE-12`、
  M5、RISK-2026-002）
- [M5：EUI-NEO UI 与 MVP 验收](../plans/aki-implementation-plan.md)（实现
  与运行复核）
