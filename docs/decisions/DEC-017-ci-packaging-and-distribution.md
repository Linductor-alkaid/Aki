# DEC-017：CI 打包与分发基线（setup.exe / deb）

> 状态：Accepted
> 日期：2026-09-28
> 负责人：Linductor
> 冻结里程碑：M6-01（随 M6-02 CI 打包 job 实施）
> 替代/被替代：无

## 背景与问题

M0~M5 的 CI 门禁（`.github/workflows/ci.yml` 五档矩阵）只产出测试证据，
不产出可安装的分发产物。项目需要 CI 具备打包能力：Windows 输出安装器
`setup.exe`，Linux 输出 deb，且 deb 最低适配 Ubuntu 20.04。这带来一组
必须先冻结的选择：

- 打包技术（生成器）与安装布局；
- 目标平台构建基线：GitHub `ubuntu-20.04` runner 已退役（2025-04），
  `ubuntu-latest`（24.04，glibc 2.39）上构建的二进制无法在 20.04
  （glibc 2.31）运行；同时 pinned EUI-NEO 硬性要求 GCC≥12 / Clang≥14
  / MSVC 19.29+（`third_party/EUI-NEO/README.md`），而 focal 官方仓库
  最高 gcc-10，focal 自带 cmake 3.16 < 工程规范的 CMake≥3.25；
- MSVC 构建配置与 CRT 再分发约束（Debug CRT 不可再分发）；
- pinned 依赖的 install 行为与分发构建旗标与 Aki 分发基线的冲突
  （见「分发旗标基线覆盖」与「安装树裁剪」两条，均为 M6-01 IVA 实测
  发现）。

## 决策

- **生成器：CPack 双生成器**（`cmake/Packaging.cmake`）。Windows NSIS
  → `aki-<ver>-win64-setup.exe`；Linux DEB → `aki_<ver>_<arch>.deb`。
  版本取 `project(aki VERSION ...)`，随里程碑发布点演进。不引入独立
  安装器脚本（NSIS 手写模板/Inno Setup/WiX），打包配置与构建图同源。
- **安装布局：自包含目录**。Windows `%ProgramFiles%\Aki`，Linux
  `/opt/aki`；`assets/` 与可执行文件同级（main.cpp 的 `iconPath` 与
  框架 cwd 修复均按 exe 目录解析，aki-run.log 落同级）。用户数据根经
  XDG/%APPDATA% 解析（DEC-004），与安装位置无关，只读安装目录可运行。
  Linux 桌面集成文件（desktop 入口、hicolor 256x256 图标）装系统绝对
  路径 `/usr/share/...`。不修改应用代码的路径解析。
- **Windows：一律 Release 构建**（MSVC Debug CRT 不可再分发）；OpenSSL
  3 运行期 DLL 随包（复用 `aki_locate_openssl_dll` 候选解析）；MSVC
  release CRT（vcruntime140/msvcp140/vcruntime140_1）经
  `InstallRequiredSystemLibraries` app-local 部署到安装根，不捆绑
  vc_redist 安装器、不要求用户先装运行时。
- **Linux 构建基线（最低适配 Ubuntu 20.04）**：CI `package-linux` job
  在 `container: ubuntu:20.04` 内构建——glibc 2.31 即 20.04 原生基线。
  工具链：cmake 3.x 线经 PyPI 官方 wheel（`>=3.25,<4`；4.x 在非 IDE
  生成器上对 INTERFACE_SYSTEM_INCLUDE_DIRECTORIES 源目录前缀路径转为
  硬错误，run 36415785709 实测，pinned 消费侧 SYSTEM 标注在 4.x 不可
  用），GCC 12 经
  `ubuntu-toolchain-r/test` PPA（EUI-NEO 硬性要求 GCC≥12）。**产物
  静态链接 libstdc++/libgcc**（`-static-libstdc++ -static-libgcc`），
  不要求用户侧 GLIBCXX ≥ 3.4.30；glibc 保持动态（2.31 为下限，不做
  静态链接——NSS/dlopen 语义风险，也不符合常规）。
- **OpenSSL 3 随包分发（focal 事实约束）**：Ubuntu 20.04 系统 OpenSSL
  为 1.1.1f，而 pinned heyaki 冻结 OpenSSL 3.x ABI（要求 ≥3.0、<4.0，
  third_party/heyaki/CMakeLists.txt:116）——focal 系统库无法满足。
  构建期在容器内源码构建 OpenSSL 3.5.4（3.x LTS 线，tarball 经
  SHA-256 固定校验）shared 安装到 /usr/local 供构建图链接；运行期经
  `AKI_BUNDLE_OPENSSL_LINUX`（显式开关，默认 OFF，仅 package job
  开启）把 `libssl.so.3`/`libcrypto.so.3` 随包装入 /opt/aki，aki 以
  `$ORIGIN` rpath 绑定同目录副本——用户机无需 OpenSSL 3。dpkg-shlibdeps
  经 `SHLIBDEPS_PRIVATE_PARAMS "-l opt/aki"` 将随包私有库排除出系统包
  Depends；系统 libssl1.1 仍经 libcurl4 依赖传递满足（libcurl 为 focal
  系统库，OpenSSL 1.1 ABI）。
- **deb 依赖声明**：`CPACK_DEBIAN_PACKAGE_SHLIBDEPS=ON` 自动生成
  （构建容器即 20.04，Depends 基线随之锁定）；显式追加 GLFW 运行期
  dlopen 集（`libx11-6 libxext6 libxrandr2 libxinerama1 libxcursor1
  libxi6 libxkbcommon0 libwayland-client0 libgl1 libegl1`——pinned
  GLFW 3.4 posix module 无 ELF NEEDED，dpkg-shlibdeps 不可见），与
  shlibdeps 结果合并。`CPACK_STRIP_FILES` 剥离调试符号（.dynsym
  保留，不影响 shlibdeps）。
- **安装树裁剪（消费侧）**：pinned heyaki 的 `install()` 规则无条件
  注册开发产物（third_party/heyaki/CMakeLists.txt:511 起；其
  `HEYAKI_AUTO_INSTALL` 只控制 POST_BUILD 安装 custom target），会把
  lib/ 静态库、include/ 头文件、share/heyaki（proto/coturn/supply-chain
  合规审计面）带进安装树（约 26MB）。pinned 依赖不可改动，以
  `CPACK_PRE_BUILD_SCRIPTS`（cmake/Packaging-prebuild.cmake）在 CPack
  临时安装树删除 `lib/`、`include/`、`share/heyaki`（DEB 与 NSIS 两
  布局根都探测，幂等）；`share/licenses/` 保留（第三方许可文本随
  二进制分发的合规要求）。
- **分发旗标基线覆盖（消费侧）**：`eui_neo_configure_app(aki)` 经
  `eui_apply_compile_options` 对非 Debug 配置注入上游 demo 策略旗标
  （GNU/Clang：`-Os -fno-exceptions -fno-rtti`；MSVC：`/O1 /GS- /sdl-`）。
  `-fno-exceptions` 破坏 executor/heyaki 的异常语义——GCC release
  构建无法编译（M6-01 IVA 实测；CI 门禁此前仅覆盖 Debug，release 首次
  由打包链路触发）；`/GS- /sdl-` 关闭栈保护与安全检查，不满足分发
  基线。在 Aki 消费侧以同一 target 的后置 `target_compile_options`
  覆盖（按调用序拼接，后写者在 GCC/MSVC 命令行中生效）：GNU/Clang
  恢复 `-fexceptions -frtti -O2`，MSVC 恢复 `/O2 /GS /sdl`；生成器
  表达式限定非 Debug，与上游注入条件对齐。优化级别取 `-O2`（发行
  默认），不接受上游 `-Os`/`/O1` 体积优先策略。
- **交付形态**：打包 job（package-linux / package-windows）随 PR 与
  master push 运行，先 release 构建并跑全量 ctest，再 `cpack`，产物经
  `actions/upload-artifact` 交付。release/tag 发布流程不在本决策范围
  （届时另行立项）。
- **不做**：不引入 snap/flatpak/AppImage；不做 Windows MSI（WiX）；
  不交叉编译（MinGW 受限已有 M3-01 记录）；不为 20.04 之外的发行版
  声明兼容（22.04+ 的 libssl1.1 缺失属预期，下游按需重建）。

## 备选方案

- **vc_redist 捆绑进 NSIS**（安装时静默执行）：需要自定义 NSIS 片段与
  联网/嵌入二进制，收益仅是省几百 KB CRT DLL；app-local 部署为微软
  支持的合法形态，选择后者。
- **单一 glibc 基线策略**（如 /onefetch、zig cc 交叉固定 glibc 2.17）：
  引入新工具链与供应链面，且本项目依赖树（GLFW dlopen、libdatachannel）
  未验证交叉形态；容器构建用发行版原生工具链最可复现。
- **静态链接 glibc**：NSS 与 dlopen 语义风险，glibc 静态链接为已知
  反模式，否决。
- **修改 heyaki/EUI-NEO 关闭 install/旗标注入**：pinned 依赖不可改动
  （仓库纪律），且属上游通用策略而非缺陷；在消费侧收口。

## 影响

- CI 增加两个 job：PR 与 master push 时长增加（容器内全量 release
  构建 + 测试 + 打包）；产物随 artifacts 保留（默认 90 天）。
- 供应链新增面：PyPI `cmake` wheel（CMake 官方二进制 repackage，
  版本约束 `>=3.25,<4`）、
  `ppa:ubuntu-toolchain-r/test`（GCC 12）与 OpenSSL 3.5.4 源码 tarball
  （GitHub release，SHA-256
  `967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99`
  固定校验）。渠道均为业界标准来源，已在本决策登记；若后续引入
  软件源级 pin/校验需求，按 `docs/supply-chain/` 纪律另行登记。
- 已知边界（如实声明，RULE-11）：deb 在标准桌面环境的 Ubuntu 20.04+
  可安装运行；22.04+ 发行版因 libssl1.1 → libssl3 的 soname 演进不在
  本包兼容声明内；最小化/无桌面系统由显式 dlopen 集依赖保证可启动。
  focal deb 内随包 OpenSSL 3 与系统 libcurl（1.1 ABI 编译）共存于同
  一进程：ELF 全局符号解析下 libcurl 的 `SSL_*` 符号解析到先加载的
  随包 libssl.so.3（OpenSSL 3 兼容层覆盖 focal libcurl 7.68 所用
  API 集，风险低），该路径未经 20.04 真机验证——由 M6 退出-4 真机
  安装启动补跑条件覆盖。
- MSVC `/O2 /GS /sdl` 覆盖组合此前从未进入任何门禁（历史 release 证据
  均在上游旗标下），Windows 打包 job 首轮存在暴露新告警/行为的可能，
  按 MR 闭环迭代处理（M6 风险表跟踪）。
