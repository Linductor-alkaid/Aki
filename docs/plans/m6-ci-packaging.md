# M6：CI 打包与分发基线

> 状态：In Progress
> 负责人：Linductor
> 所属计划：[Aki 实施总计划](aki-implementation-plan.md)
> 前置：M0（CI 门禁）、M5-02（EUI-NEO 构建图接入）、DEC-014（CI 系统依赖集）
> 建议发布点：无（工程基础设施，支撑后续 v0.5.0+ 交付）
> 更新日期：2026-09-28

## 目标

CI 具备安装包产出能力：每次 PR 与 master push 交付 Windows 安装器
`aki-<ver>-win64-setup.exe` 与 Linux deb（最低适配 Ubuntu 20.04），
作为 workflow artifacts。打包配置与构建图同源（CPack），install 布局、
目标平台构建基线（glibc 2.31、GCC 12、静态 libstdc++/libgcc、MSVC
release CRT app-local）与分发旗标基线经决策冻结为
[DEC-017](../decisions/DEC-017-ci-packaging-and-distribution.md)。

## 范围与非目标

### 范围

- `cmake/Packaging.cmake`：install 布局（自包含目录，assets 与可执行
  文件同级）+ CPack NSIS/DEB 生成器配置 + Linux 桌面集成文件安装。
- `cmake/Packaging-prebuild.cmake`：CPack 临时安装树裁剪（pinned heyaki
  无条件 install() 带入的开发产物）。
- 根 `CMakeLists.txt`：`aki_locate_openssl_dll` 提取（POST_BUILD 部署与
  CPack install 共用）；分发旗标消费侧覆盖（EUI-NEO 上游 release 旗标
  与异常语义/分发安全基线冲突，DEC-017）。
- `packaging/linux/aki.desktop`；CI `package-linux` / `package-windows`
  job（release 构建 + 全量 ctest + cpack + artifacts）。
- 文档：DEC-017、本里程碑、总计划登记。

### 非目标

- release/tag 发布流程、签名与校验和发布面（另行立项）。
- snap/flatpak/AppImage、MSI。跨版本单包兼容（Ubuntu 20.04~24.04，
  2026-09-28 用户确认）在范围内，机制与边界见 DEC-017「单包跨版本兼
  容」条。
- 安装包运行期端到端安装验证（CI 无 GUI 环境；deb 布局经解包校验 +
  后续真机补跑条件见退出-4）。

## 设计与决策依据

- [DEC-017](../decisions/DEC-017-ci-packaging-and-distribution.md)：
  分发基线全项（生成器、布局、构建基线、依赖声明、裁剪、旗标覆盖）。
- [DEC-006](../decisions/DEC-006-heyaki-api-contract.md)（Windows OpenSSL
  运行期部署先例）、[DEC-005](../decisions/DEC-005-eui-neo-integration.md)
  / [DEC-014](../decisions/DEC-014-eui-tsan-coverage.md)（构建图与 CI
  系统依赖集）。
- EUI-NEO 编译器要求（GCC≥12 / Clang≥14 / MSVC 19.29+，
  third_party/EUI-NEO/README.md）——focal 工具链选型约束。

## 工作项

- [x] `M6-01` CPack 打包基线：install 布局（/opt/aki 与 Program Files
  自包含目录、desktop/hicolor 集成）、NSIS/DEB 生成器配置、pre-build
  裁剪、分发旗标消费侧覆盖。本地 Linux 三轮 IVA 验证通过（deb 控制字段、
  布局、Depends 合并、裁剪、strip、静态链接、release 43/43），证据见
  验证记录。
- [x] `M6-02` CI 打包 job：`package-linux`（ubuntu:20.04 容器 + PyPI
  cmake 3.x wheel + gcc-12 PPA + release + ctest + cpack DEB +
  artifacts）与
  `package-windows`（release + ctest + cpack NSIS + artifacts）。
- [ ] `M6-03` 收口：双端 CI 打包 job 全绿证据归集（含 Windows NSIS
  首轮实测）、本验证记录回填、总计划状态同步。
  （2026-09-28 完成：PR #55 终轮 run 36421170332 全绿 7/7，产物
  `aki-ubuntu20.04-amd64-deb` 13.5MB / `aki-windows-setup` 10.4MB 经
  artifacts 交付；deb 控制字段 CI 实测 `Depends: libc6 (>= 2.30),
  libcurl4, libglib2.0-0, libx11-6, …`——focal 名、无 libssl 依赖
  （OpenSSL 3 随包），glibc 2.30 下限即 20.04~24.04 单包兼容机制实证；
  四轮 CI 迭代修复链（OpenSSL 3 随包 / cmake 3.x 钉线 / sqlite
  Threads / NSIS 安装+glob）与本验证记录归集。）

## 风险与阻塞

- ~~**focal APT 源可用性**~~：2026-09-28 CI 首轮实测——
  `archive.ubuntu.com` focal 池可用，安装正常（run 36414428524
  package-linux 日志），风险解除；保留观察（focal 退役推进）。
- ~~**gcc-12 PPA 包可用性**~~：同轮实测——
  `ppa:ubuntu-toolchain-r/test` gcc-12.5 正常安装，风险解除。
- ~~**MSVC `/O2 /GS /sdl` 覆盖组合**~~：2026-09-28 CI 实测解除——
  run 36418888842 起 Windows release 构建 + 全量 ctest 通过，终轮
  run 36421170332 打包全绿。
- ~~**GCC 12 vs 13 告警面差异**~~：2026-09-28 CI 实测解除——focal
  容器 gcc-12 + `AKI_WARNINGS_AS_ERRORS=ON` 构建零错误（仅既登记的
  第三方 -Wmissing-field-initializers 诊断，已降级不升错）。
- **focal 无系统 OpenSSL 3**（首轮 CI 实测发现）：pinned heyaki 冻结
  OpenSSL 3.x ABI（≥3.0、<4.0），focal 系统库 1.1.1f 无法满足——已按
  DEC-017 处置：容器内源码构建 OpenSSL 3.5.4（SHA-256 固定校验）至
  /usr/local + `AKI_BUNDLE_OPENSSL_LINUX` 随包分发 + `$ORIGIN` rpath +
  shlibdeps 私有库排除。残留边界：随包 OpenSSL 3 与系统 libcurl（1.1
  ABI）同进程共载的符号解析路径未经 20.04 真机验证（退出-4 覆盖）。

## 测试与退出条件

- [x] 退出-1：Linux 本地 release 构建 + 全量 ctest 通过（GCC 13.3 本机，
  43/43 三轮复验；静态 libstdc++/libgcc 链接经 ldd 反证）。
- [x] 退出-2：deb 产物结构验收（dpkg-deb -I/-c）：控制字段
  （Package/Version/Architecture/Maintainer）、Depends（shlibdeps 自动集
  + 手动 dlopen 集合并）、布局（/opt/aki 自包含 + desktop + hicolor）、
  开发产物裁净（lib/include/share-heyaki 三目录反证）、许可文本保留、
  strip 生效。
- [x] 退出-3：CI 双端打包 job 全绿（PR #55 终轮 run 36421170332，
  7/7；产物 artifacts 可下载，见 M6-03）。
- [ ] 退出-4：deb 真机安装 + 启动验证（单包跨版本目标的实证面）：
  20.04 安装启动、22.04 安装启动（2026-09-28 用户环境已备）、24.04
  安装启动（本机）。补跑条件：CI 产物（focal 构建）下载后逐版执行；
  CI 容器仅证明构建基线，安装运行属 RULE-11 本机验证面；注意本机
  24.04 自建产物不可作跨版本证据（依赖基线即 24.04）。

## 验证记录

### 2026-09-28（M6-01 三轮 IVA 独立验证，Linux 侧）

环境：Ubuntu 24.04.5 / GCC 13.3 / CMake 3.28；链接模拟 CI：
`-static-libstdc++ -static-libgcc`；本机偏差：`GLFW_BUILD_WAYLAND=OFF`
（缺 libxkbcommon-dev 且无免密 sudo，沿 debug 基线形态；CI 容器依赖完整
不受影响，仅使本地 Depends 少 wayland/xkbcommon 项）。

- **轮 1（FAIL→发现既有缺陷）**：release 构建失败——`eui_apply_compile_options`
  在非 Debug 配置注入 `-Os -fno-exceptions -fno-rtti`（GNU/Clang 分支），
  executor/heyaki 异常语义被破坏（`task_cancellation.hpp:262` catch 不可
  用、`manager_runtime.hpp:211/213` handler 顺序错误）；最小复现实验证明
  源码正确、全为旗标产物。**根因**：CI 门禁五档全为 Debug，release 自
  M5-02 接入 eui_neo_configure_app 起从未被编译——打包是首个触发点。
  附带发现：`-Os` 位于 `-O3` 之后静默降优化；MSVC 分支 `/O1 /GS- /sdl-`
  同源问题（可编译但降低分发安全基线）。修复：DEC-017 消费侧后置
  `target_compile_options` 覆盖（GNU/Clang `-fexceptions -frtti -O2`、
  MSVC `/O2 /GS /sdl`，非 Debug 限定）。同轮确认打包配置未破坏 debug
  基线（43/43）。
- **轮 2（PASS WITH RISKS→两处收口）**：release 构建通过（flags.make 证
  据：上游旗标在前、覆盖旗标在命令行末尾，最终生效 `-O2` 与
  `-fexceptions -frtti`）；ctest 43/43；deb 生成。发现：(a) heyaki 无条件
  install() 把约 26MB 开发产物（lib/ 静态库、include/、share/heyaki）
  带进安装树；(b) GLFW 3.4 运行期 dlopen 的 X11/GL 库无 ELF NEEDED，
  不进 shlibdeps 的 Depends。修复：`CPACK_PRE_BUILD_SCRIPTS` 裁剪 +
  手动 `CPACK_DEBIAN_PACKAGE_DEPENDS` 追加 dlopen 集（与自动集合并）。
- **轮 3（FAIL→修复）**：裁剪脚本 `_aki_prune_lib` 列表缺 `lib` 元素
  （include/share-heyaki 已裁净、lib/ 24.9MB 残留；Depends 合并、strip、
  静态链接全部通过）。修复：统一列表 `lib;include;share/heyaki`。
- **轮 4（PASS）**：cpack pre-build script 执行无告警；三目录反证全部
  exit=1（裁净）；正向抽查（aki、图标、EUI 字体/shaders 33 项、desktop、
  hicolor、share/licenses/heyaki/LICENSE）齐全；Depends 逐字一致（手动
  X10 集 + libc6/libcurl4t64/libglib2.0-0t64/libssl3t64 自动集）；
  Installed-Size 45498→20021 KB（-56%，与根因定量吻合）；deb 体积
  16.77→10.49 MB；二进制 stripped；`ldd` 无 libstdc++/libgcc。

Windows NSIS 链路无法在本机验证，随 `M6-03` CI 首轮实测归集证据。

### 2026-09-28（CI 首轮 package-linux 实测与修复，run 36414428524）

PR #55 首轮 CI：门禁五档进行中，`package-linux` 2m22s 失败于
configure——focal 系统 OpenSSL 1.1.1f 不满足 heyaki 的
`find_package(OpenSSL 3.0 REQUIRED)`（focal 源、gcc-12 PPA、PyPI
cmake wheel 均工作正常）。处置（DEC-017 增补「OpenSSL 3 随包分发」条
款）：容器内源码构建 OpenSSL 3.5.4（tarball SHA-256
`967311f8…def355e99` 固定校验）shared 至 /usr/local；新增
`AKI_BUNDLE_OPENSSL_LINUX` 显式开关（默认 OFF，本地验证流程不变），
随包分发 `libssl.so.3`/`libcrypto.so.3` 至 /opt/aki + `$ORIGIN`
rpath + dpkg-shlibdeps 私有库目录排除（`-l opt/aki`）。修复 commit
重推后按 MR 闭环重新等待 CI。

### 2026-09-28（CI 迭代修复链与终轮全绿，PR #55）

门禁五档（debug/asan/ubsan/tsan + Windows MSVC debug）自首轮起持续
全绿；打包 job 经四轮迭代修复后终轮 run 36421170332 全绿（7/7）：

- 轮 1（run 36414428524）：focal 系统 OpenSSL 1.1.1f 不满足 heyaki
  `find_package(OpenSSL 3.0 REQUIRED)`——focal 无 OpenSSL 3 系统包，
  按 DEC-017 增补源码构建 + 随包分发（AKI_BUNDLE_OPENSSL_LINUX）。
- 轮 2（run 36415785709）：Kitware 源 cmake 4.4 在非 IDE 生成器上对
  消费侧 INTERFACE_SYSTEM_INCLUDE_DIRECTORIES 源目录前缀路径报
  generate 硬错误（4.x 行为变更）——PyPI wheel 钉 3.x 线替代。
- 轮 3（run 36416729582）：test_sqlite_sourceid 链接失败——glibc 2.34
  起 pthread 并入 libc 掩盖了 vendored sqlite3 的缺链，focal 2.31
  暴露；sqlite3 target PUBLIC 链 Threads::Threads。
- 轮 4（run 36418888842）：Windows「Cannot find NSIS compiler
  makensis」（预装假设证伪）→ Chocolatey 安装；deb sanity glob 笔误
  修正（产物为连字符命名）。本轮 MSVC `/O2 /GS /sdl` release 构建 +
  全量 ctest 首次实测通过。
- 终轮（run 36421170332，全绿 7/7）：package-linux 16m44s（focal 容
  器 OpenSSL 3.5.4 构建 + release + ctest + cpack DEB + dpkg 校验）、
  package-windows 19m48s（NSIS 安装 + release + ctest + cpack + 产
  物检查）；artifacts：`aki-ubuntu20.04-amd64-deb`（13.5MB）、
  `aki-windows-setup`（10.4MB）。deb Depends CI 实测：`libc6 (>= 2.30),
  libcurl4 (>= 7.16.2), libglib2.0-0 (>= 2.26.0)` + X11/Wayland/GL
  dlopen 集——focal 名、无 libssl（随包 OpenSSL 3），单包跨 20.04~
  24.04 机制实证（t64 Provides 于 24.04 本机核验）。

跨版本兼容决策修订（2026-09-28 用户确认「兼容 20.04 以及后续系统」）：
DEC-017「单包跨版本兼容」条落档；本机 24.04 自建产物不可装 22.04
（用户实测复现，依赖基线即 24.04）——跨版本兼容以 focal 容器 CI 产物
为准；22.04/24.04 真机安装启动转退出-4 验证面。
