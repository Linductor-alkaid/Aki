# CPack 打包基线（M6-01，DEC-017）：Windows NSIS setup.exe 与 Linux DEB
#（最低适配 Ubuntu 20.04）。目标平台的构建基线（glibc 2.31、GCC 12、静态
# libstdc++/libgcc）由 CI package job 的 ubuntu:20.04 容器保证（见
# .github/workflows/ci.yml 的 package job）；本文件只定义 install 布局与
# 生成器配置。
#
# 安装布局（DEC-017）：自包含目录——Windows <ProgramFiles>\Aki、Linux
# /opt/aki。assets/ 必须与可执行文件同级：main.cpp 的 iconPath 与框架的
# cwd 修复（repairCurrentWorkingDirectory → exe 目录）均按 exe 目录解析，
# aki-run.log 也落在同级。用户数据根经 XDG/%APPDATA% 解析（DEC-004），
# 与安装位置无关，故只读安装目录（/opt/aki）可正常运行。

install(TARGETS aki RUNTIME DESTINATION .)

# 运行期资源与 POST_BUILD 部署面同源：Aki 图标 + EUI-NEO 上游 assets
#（字体/shader 等，eui_neo_copy_assets 的 copy_directory 同一目录）。
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/"
    DESTINATION assets/icons)
get_property(AKI_EUI_ASSETS_DIR GLOBAL PROPERTY EUI_NEO_APP_ASSETS_DIR)
if(AKI_EUI_ASSETS_DIR AND EXISTS "${AKI_EUI_ASSETS_DIR}")
    install(DIRECTORY "${AKI_EUI_ASSETS_DIR}/" DESTINATION assets)
else()
    message(WARNING
        "EUI-NEO runtime assets directory unavailable; the package will be "
        "missing framework assets (fonts/shaders).")
endif()

if(MSVC)
    # OpenSSL 3 运行期 DLL 随包（DEC-006：MSVC 链接系统 OpenSSL 3）。候选
    # 解析与 aki_deploy_openssl_dlls 共用 aki_locate_openssl_dll；未命中仅
    # WARNING（构建可继续），缺 DLL 的包在启动期暴露——与测试部署面同纪律。
    foreach(_dll IN ITEMS libssl-3-x64.dll libcrypto-3-x64.dll)
        aki_locate_openssl_dll("${_dll}" _aki_pkg_dll)
        if(_aki_pkg_dll STREQUAL "")
            message(WARNING
                "OpenSSL runtime DLL '${_dll}' not found under "
                "OPENSSL_ROOT_DIR; the package will miss it. Set "
                "OPENSSL_ROOT_DIR to the OpenSSL 3 install prefix "
                "(DEC-006, M3-01).")
        else()
            install(FILES "${_aki_pkg_dll}" DESTINATION .)
        endif()
    endforeach()

    # MSVC release CRT app-local 部署（vcruntime140/msvcp140/vcruntime140_1，
    # 与 exe 同级，加载器 app-local 优先）。Debug CRT 不可再分发——打包一律
    # Release 构建（DEC-017，CI package job 约束）。
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ".")
    include(InstallRequiredSystemLibraries)
    if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION .)
    endif()
endif()

if(UNIX AND NOT APPLE)
    # Linux 桌面集成文件装到系统绝对路径（不受 /opt/aki prefix 影响）。
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/aki.desktop"
        DESTINATION /usr/share/applications)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/aki-icon.png"
        DESTINATION /usr/share/icons/hicolor/256x256/apps
        RENAME aki.png)
endif()

# M6-02（DEC-017）：OpenSSL 3 随包分发（显式开关，默认 OFF）。基线平台
# Ubuntu 20.04 的系统 OpenSSL 为 1.1，而 pinned heyaki 冻结 OpenSSL 3.x
# ABI（third_party/heyaki/CMakeLists.txt:116 要求 >=3.0、<4.0）——focal
# 系统库无法满足，CI package job 在容器内构建 OpenSSL 3 到 /usr/local
# 并开启本开关：libssl.so.3/libcrypto.so.3 随包装入 /opt/aki，aki 以
# $ORIGIN rpath 绑定同目录副本，不要求用户机装 OpenSSL 3。
option(AKI_BUNDLE_OPENSSL_LINUX
    "Bundle the OpenSSL 3 shared libraries next to the executable (Linux package builds on distros without system OpenSSL 3)."
    OFF)
if(AKI_BUNDLE_OPENSSL_LINUX AND UNIX AND NOT APPLE)
    foreach(_so IN ITEMS libssl.so.3 libcrypto.so.3)
        aki_locate_openssl_shared_lib("${_so}" _aki_pkg_so)
        if(_aki_pkg_so STREQUAL "")
            message(FATAL_ERROR
                "AKI_BUNDLE_OPENSSL_LINUX is ON but '${_so}' was not found "
                "under OPENSSL_ROOT_DIR; set OPENSSL_ROOT_DIR to the "
                "OpenSSL 3 install prefix (DEC-017).")
        endif()
        install(FILES "${_aki_pkg_so}" DESTINATION .)
    endforeach()
    # 可执行文件优先从安装目录解析随包 OpenSSL（先于系统路径）。
    set_target_properties(aki PROPERTIES INSTALL_RPATH "$ORIGIN")
    # dpkg-shlibdeps 把安装树内的私有库声明为私有目录：对其不生成系统包
    # Depends、不报 no-dependency-information（其余 NEEDED 照常生成）。
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_PARAMS "-l opt/aki")
endif()

# M6-01（DEC-017）：CPack pre-build 裁剪——pinned heyaki 的 install() 规则
# 无条件注册开发产物（third_party/heyaki/CMakeLists.txt:511 起；其
# HEYAKI_AUTO_INSTALL 只控制 POST_BUILD 安装 custom target，不约束 install
# 规则），否则 lib/ 静态库与 include/ 头文件（约 26MB）混入 deb 与 NSIS
# 安装树。pinned 依赖不可改动，在 CPack 临时安装树消费侧裁剪；第三方许可
# 文本（share/licenses/）保留随二进制分发。脚本内容见
# cmake/Packaging-prebuild.cmake。
set(CPACK_PRE_BUILD_SCRIPTS
    "${CMAKE_CURRENT_LIST_DIR}/Packaging-prebuild.cmake")

set(CPACK_VERBATIM_VARIABLES ON)
set(CPACK_PACKAGE_NAME "aki")
set(CPACK_PACKAGE_VENDOR "Aki")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Aki - cross-device instant messaging client (Heyaki reference application)")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Linductor-alkaid <202200171251@mail.sdu.edu.cn>")

if(WIN32)
    set(CPACK_GENERATOR "NSIS")
    # 产物名 = CPACK_PACKAGE_FILE_NAME + ".exe"：aki-<ver>-win64-setup.exe。
    set(CPACK_PACKAGE_FILE_NAME "aki-${PROJECT_VERSION}-win64-setup")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Aki")
    set(CPACK_NSIS_MUI_ICON
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/aki-icon.ico")
    set(CPACK_NSIS_MUI_UNIICON
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/aki-icon.ico")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)
    # aki.exe 在安装根（非 bin/），EXECUTABLES_DIRECTORY 置 "."。
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "aki.exe")
    # 开始菜单与桌面快捷方式（\\\\ 经 CMake 字符串转义产出 NSIS 的 \\）。
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Aki.lnk' '$INSTDIR\\\\aki.exe'
         CreateShortCut '$DESKTOP\\\\Aki.lnk' '$INSTDIR\\\\aki.exe'")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "Delete '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Aki.lnk'
         Delete '$DESKTOP\\\\Aki.lnk'")
elseif(UNIX)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/aki")
    set(CPACK_DEBIAN_PACKAGE_SECTION "net")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    # 运行期依赖由 dpkg-shlibdeps 扫描安装树自动生成；CI package job 的
    # 构建容器即 Ubuntu 20.04，Depends 基线随之锁定为 20.04 库版本
    #（DEC-017）。libstdc++/libgcc 已静态链接，不出现在 Depends。
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    # GLFW 3.4（pinned EUI-NEO bundled）运行期经 posix module dlopen X11/
    # Wayland/GL 库（无 ELF NEEDED，dpkg-shlibdeps 看不到）：按 CI 安装的
    # 后端依赖集显式声明运行库，保证最小化系统装包即可启动（包名在
    # Ubuntu 20.04 与 24.04 一致；与 shlibdeps 结果合并，DEC-017）。
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "libx11-6, libxext6, libxrandr2, libxinerama1, libxcursor1, libxi6, libxkbcommon0, libwayland-client0, libgl1, libegl1")
    # 发行包剥离调试符号（只去 .symtab，.dynsym 保留，不影响 shlibdeps）。
    set(CPACK_STRIP_FILES ON)
endif()

include(CPack)
