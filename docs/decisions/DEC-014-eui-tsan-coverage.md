# DEC-014：EUI-NEO 栈的 sanitizer 覆盖与 CI Linux 系统依赖

> 状态：Accepted
> 日期：2026-09-27
> 负责人：Linductor
> 冻结里程碑：M5-02（EUI-NEO 接入与主窗口骨架，随 CI 扩面实施）
> 替代/被替代：无（补全 [DEC-005](DEC-005-eui-neo-integration.md)「影响与
> 风险 → CI 扩面」中"tsan 档对 eui_neo 插桩与否须决策并记录"的挂起项）

## 背景与问题

DEC-005 把 EUI-NEO 以单一构建图 `add_subdirectory(third_party/EUI-NEO)` 接入
后，Aki 的三个 sanitizer preset（asan/ubsan/tsan）经 CMakePresets cache 级
`CMAKE_<LANG>_FLAGS` 全图注入——eui_neo 与其 bundled 第三方
（glfw/freetype/libpng/zlib/glad/md4c/yyjson/miniaudio）是否会随之插桩、
CI Linux 三档需要哪些系统依赖、覆盖声明口径是什么，必须先冻结再动 CI
（DEC-005 影响与风险节挂起项）。

## 决策

- **全图插桩，零豁免**：tsan（及 asan/ubsan）档对 eui_neo 与 bundled 第三方
  保持现状机制自动生效的全图编译+链接插桩，不引入任何按目标剥离或 CI 专属
  开关。机制实证：cache 级 `CMAKE_CXX_FLAGS`/`CMAKE_C_FLAGS` 语义生成器无关
  （Windows/MSVC VS 生成器 configure 探针：eui_neo/eui_zlib/eui_md4c/glad/
  glfw/freetype/png_static/探针 exe 八目标 vcxproj AdditionalOptions 全部携带
  `-fsanitize=thread -fno-omit-frame-pointer`；EUI-NEO 全部 CMake 无任何
  sanitizer 处理/flag 剥离逻辑）；与 DOD-03 + DEC-006「全图插桩」口径一致
  （heyaki vendored C 栈显式补 `-fsanitize=thread`、M2 为 sqlite3.c 专门补
  C Flags——方向一贯「能插尽插」）。
- **CI Linux 三档安装完整系统依赖集**（pinned 集成指南
  `third_party/EUI-NEO/docs/集成指南.md` Ubuntu 完整集）：`libcurl4-openssl-dev
  libgl1-mesa-dev libegl1-mesa-dev libx11-dev libxext-dev libxrandr-dev
  libxinerama-dev libxcursor-dev libxi-dev libwayland-dev wayland-protocols
  libxkbcommon-dev libglib2.0-dev`（+ 既有 `libssl-dev`）。libcurl 非 Windows
  为 REQUIRED（3rd/dependencies.cmake）不可选；tray 与 GLFW Wayland 保持上游
  默认 ON，**不采用** CI 专属 `-DEUI_ENABLE_TRAY=OFF` /
  `GLFW_BUILD_WAYLAND=OFF` 降档（避免 CI 图与产品图第三种形态分叉，保留
  tray SNI 与 Wayland 后端的 CI 编译覆盖）。
- **覆盖声明口径（五条，落档为准）**：
  1. **插桩面**：tsan/asan/ubsan 经 CMakePresets cache 级全局 C/CXX FLAGS
     （+`HEYAKI_SANITIZER` 联动）全图插桩，图内所有目标（含 eui_neo、
     bundled glfw/freetype/libpng/zlib/glad/md4c/yyjson/miniaudio、heyaki
     vendored 栈、executor、vendored sqlite）均以 `-fsanitize` 编译链接，以
     构建日志核验（M3-01 同口径）。
  2. **执行面**：CI ctest 为 console 测试（测试 exe 不链 eui，DEC-005），eui
     栈覆盖=编译+链接插桩、**无运行期执行**；不得声明「UI/渲染经 TSAN 验证」。
  3. **运行期 UI 证据**：requestUpdate 跨线程唤醒/DoubleBuffer 主线程排空/
     onShutdown 关闭序（窗口/GPU 设备销毁与 worker 回收次序）的 tsan 运行期
     证据走本机 Linux 真实显示环境运行 UI，复现命令+日志归档（退出-4「渲染层
     本机验证证据归档」口径），CI 不宣称。
  4. **上游竞态**：抑制表登记制（`cmake/tsan-suppressions.supp`），上游修复
     后移除条目；第一方（`aki::` 及 app/heyaki/persistence/transfer/ui/
     main.cpp/tests/）竞争必修不抑制。
  5. **平台限定**：TSAN 覆盖声明限 Linux CI；Windows 为 MSVC debug 门禁
     （RISK-2026-003 解除口径延续）。
- **GCC `-Wtsan` ×第一方 `-Werror`**：沿 heyaki 先例（HeyakiProjectOptions
  对 GNU + thread sanitizer 照抄 `-Wno-error=tsan`，限定诊断不升错）应用于
  Aki 第一方告警函数；eui/bundled 目标自身无 `-Werror`（结构核实），不受影响。

## 备选方案

- **A（否决）：CI tsan 档豁免 eui 栈（不插桩 eui_neo/bundled）**——cache 级
  全局 flags 无法按目标剥离，须把整个 preset 重构为逐 target 注入并改 heyaki
  栈语义，破坏 DEC-006 全图插桩口径；且 TSAN 混合插桩/非插桩代码产生不可判读
  的假阳/漏报，M5 最需要 tsan 看住的跨线程面（compose/唤醒/关闭序）证据失真。
- **B（否决，保留为回退）：CI 专属 `-DEUI_ENABLE_TRAY=OFF` +
  `GLFW_BUILD_WAYLAND=OFF` 省 glib/wayland 依赖**——在 DEC-005 冻结开关之外
  制造 CI 图与产品图第三种形态分叉，须按 RULE-11 声明差异，且失去 tray SNI
  后端与 Wayland 后端的 CI 编译覆盖；X11/mesa/curl 反正必装，边际节省≈0。
  若 runner 滚动更新中 glib/wayland dev 引发构建失败，可临时降档但须登记
  差异+移除条件。
- **C（否决）：tsan 档不构建 eui**——破坏「CI Linux 三档为全量构建主路径」
  （DEC-006）与 M5-02 configure 三方校验验收面。

## 影响与风险

- 本调研在 Windows/MSVC 仅验证 flag 传播机制与 configure；真实 Linux GCC
  tsan 编译/链接（freetype/glfw/libpng C 代码 + eui C++17）以 M5-02 CI 首跑
  为准——构建日志中 `-fsanitize=thread` 出现在 eui/bundled 目标编译命令即
  通过（本机无 Linux 工具链，w64devkit MinGW 不支持 TSAN，如实声明）。
- GCC 对 TSAN 不插桩 atomic fence 发 `-Wtsan`：第一方若被命中照抄 heyaki
  豁免（限定 GNU + thread sanitizer，仅 `-Wno-error=tsan` 不关警告），
  M5-02 tsan 首跑必验。
- tsan 运行期上游噪声（glfw/freetype/glib 竞态）：本机跑 UI 时按 usrsctp
  先例进 `cmake/tsan-suppressions.supp`，不改 third_party。
- 必须守住「测试 exe 不链 eui」（DEC-005）：否则破坏 console 约定与
  「CI 不跑渲染」声明基础，且 tsan ctest 会加载显示栈。
- EUI assets POST_BUILD copy 到 exe 旁——CI 只构建无影响；本机运行证据归档
  时核验就位。
- runner 依赖必须显式 `apt install`（除 libssl-dev/pkg-config 外均未预装，
  勿依赖预装状态——以实际安装运行为准）；apt 集须含 `wayland-protocols`
  （GLFW Wayland scanner 依赖）。
- Windows CI 零新增依赖（CURL QUIET 可选、tray 走 WINAPI），覆盖声明限
  Linux 不变。

## 验证方式

- 机制探针（2026-09-26 冻结调研会话执行）：`build/scratch/dec014-probe/`
  standalone configure + VS 生成工程逐 vcxprogrep，八目标全部携带
  `-fsanitize=thread -fno-omit-frame-pointer`（scratch，gitignored）。
- M5-02 履行：CI Linux 三档 apt 依赖集落地并首跑全绿（构建日志含 eui/bundled
  目标 `-fsanitize=thread`）；`-Wno-error=tsan` 豁免按 heyaki 同款条件入
  `aki_apply_warnings`；本条决策与 M5-02 验证记录互链。
- 覆盖声明五条为对外口径基准：后续任何「UI 经 TSAN 验证」表述须先满足
  第 3 条（本机 Linux 运行证据）。

## 关联文档和工作项

- [DEC-005](DEC-005-eui-neo-integration.md)（集成方式/构建开关冻结——本决策
  补其 CI 扩面挂起项）、[DEC-006](DEC-006-heyaki-api-contract.md)（全图
  插桩口径与 runner 显式装依赖先例）
- [Aki 设计方案](../design/aki_design.md)§9.1（渲染层验证策略/RULE-11 口径）
- [M5：EUI-NEO UI 与 MVP 验收](../plans/m5-eui-neo-ui-mvp.md)（`M5-02` 实施
  与验证记录；退出-2/退出-4）
- `cmake/tsan-suppressions.supp`（上游竞态抑制表）、`.github/workflows/ci.yml`
