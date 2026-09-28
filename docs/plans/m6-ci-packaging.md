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
- snap/flatpak/AppImage、MSI、跨发行版兼容声明（22.04+ 不在 deb 兼容
  声明内，DEC-017「不做」）。
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
- [x] `M6-02` CI 打包 job：`package-linux`（ubuntu:20.04 容器 + Kitware
  cmake + gcc-12 PPA + release + ctest + cpack DEB + artifacts）与
  `package-windows`（release + ctest + cpack NSIS + artifacts）。
- [ ] `M6-03` 收口：双端 CI 打包 job 全绿证据归集（含 Windows NSIS
  首轮实测）、本验证记录回填、总计划状态同步。

## 风险与阻塞

- **focal APT 源可用性**：Ubuntu 20.04 已过标准支持期（2025-04），
  `archive.ubuntu.com` 对 focal 的保留策略可能变化（移至
  old-releases.ubuntu.com）；首轮 CI 实测，若 404 则在 package-linux
  步骤内切换源并登记。
- **gcc-12 PPA 包可用性**：`ppa:ubuntu-toolchain-r/test` 对 focal 的
  gcc-12 供给随上游维护变化；同上首轮实测。
- **MSVC `/O2 /GS /sdl` 覆盖组合**：此前从未进入任何门禁（历史 release
  证据在上游 `/O1 /GS- /sdl-` 旗标下），Windows 打包 job 首轮存在暴露
  新告警/行为的可能；按 MR 闭环迭代处理。
- **GCC 12 vs 13 告警面差异**：release preset 携带
  `AKI_WARNINGS_AS_ERRORS=ON`，GCC 12 下可能出现 13 未见的诊断；同上
  迭代处理（保留诊断不升错的既有纪律可依例）。

## 测试与退出条件

- [x] 退出-1：Linux 本地 release 构建 + 全量 ctest 通过（GCC 13.3 本机，
  43/43 三轮复验；静态 libstdc++/libgcc 链接经 ldd 反证）。
- [x] 退出-2：deb 产物结构验收（dpkg-deb -I/-c）：控制字段
  （Package/Version/Architecture/Maintainer）、Depends（shlibdeps 自动集
  + 手动 dlopen 集合并）、布局（/opt/aki 自包含 + desktop + hicolor）、
  开发产物裁净（lib/include/share-heyaki 三目录反证）、许可文本保留、
  strip 生效。
- [ ] 退出-3：CI 双端打包 job 全绿（PR 门禁；产物 artifacts 可下载）。
- [ ] 退出-4：deb 在 Ubuntu 20.04 真机安装 + 启动验证（补跑条件：20.04
  环境/虚拟机；CI 容器仅证明构建基线，安装运行属 RULE-11 本机验证面）。

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
