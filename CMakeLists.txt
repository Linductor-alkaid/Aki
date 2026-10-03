cmake_minimum_required(VERSION 3.25)

project(aki VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AKI_BUILD_TESTS "Build Aki tests" ON)
option(AKI_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

include(cmake/Dependencies.cmake)

# M2（sqlite）目标级接入（DEC-004、M2-02）：vendored amalgamation 编译为静态库，
# 完整性由 cmake/Dependencies.cmake 在 configure 时按锁文件 SHA-256 校验；
# 第三方 target 不继承第一方告警级别（DEC-007 纪律），见 third_party/sqlite/CMakeLists.txt。
add_subdirectory(third_party/sqlite)

# M3-01（heyaki）目标级接入（DEC-006 构建接入条款）：单一构建图
# add_subdirectory(third_party/heyaki)，只消费 heyaki::client（heyaki::services
# 为空伞 target，不链）。executor target 由 heyaki 的 third_party/executor 提供
# （双侧同 pin e2362736，构建图内只编译一份 executor）——Aki 移除自己的
# executor add_subdirectory（DEC-003 executor 接入条款按 DEC-006 修订），
# SYSTEM include 与告警豁免设置转移到下方 heyaki 提供的 executor target。
# 构建开关冻结（DEC-006）：HEYAKI_BUILD_APPS=OFF、HEYAKI_AUTO_INSTALL=OFF；
# heyaki 测试不进入 Aki 构建面（BUILD_TESTING 在 heyaki 子目录内置 OFF，
# Aki 自身测试仍经文件末尾的 enable_testing/add_test 注册）。
# 依赖拉取只经 scripts/fetch_third_party.sh（ref+commit 双校验，DEC-006 首次
# 配置网络声明）：HEYAKI_FETCH_DEPENDENCIES=OFF 使 configure 只做 --check 校验
# （缺失/漂移即 FATAL_ERROR，不静默联网），HEYAKI_VERIFY_DEPENDENCIES=ON。
set(HEYAKI_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(HEYAKI_AUTO_INSTALL OFF CACHE BOOL "" FORCE)
set(HEYAKI_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)
set(HEYAKI_VERIFY_DEPENDENCIES ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF)
add_subdirectory(third_party/heyaki)
unset(BUILD_TESTING)

# M5-02（DEC-005 构建开关冻结）：EUI-NEO 单一构建图接入——
# add_subdirectory(third_party/EUI-NEO) + eui_neo_configure_app(aki) 注入
# 框架 main（/SUBSYSTEM:WINDOWS）。开关按 DEC-005 CACHE FORCE 显式写死，
# 不依赖上游默认值（EUI_DEPS_MODE=bundled 零联网拉取；apps/test-fixtures/
# install/modules 全 OFF；glfw+opengl 后端）。EUI_ENABLE_TRAY 保持上游默认
# ON（DEC-014：CI Linux 安装 glib/gio，不做 CI 专属降档）。pin 校验由
# cmake/Dependencies.cmake 按锁文件执行（b9032a8a，M0 起已登记）。
set(EUI_DEPS_MODE "bundled" CACHE STRING "" FORCE)
set(EUI_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(EUI_BUILD_USER_APPS OFF CACHE BOOL "" FORCE)
set(EUI_BUILD_TEST_FIXTURES OFF CACHE BOOL "" FORCE)
set(EUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(EUI_ENABLE_MODULES OFF CACHE BOOL "" FORCE)
set(EUI_WINDOW_BACKEND "glfw" CACHE STRING "" FORCE)
set(EUI_RENDER_BACKEND "opengl" CACHE STRING "" FORCE)
add_subdirectory(third_party/EUI-NEO)
include(cmake/EuiX11ImePatch.cmake)
aki_apply_eui_x11_ime_patch()

# 应用图标（2026-09-27 用户确认，aki_ui_design §2.6）：Windows exe 内嵌
# 图标采用 Heyaki 图标（assets/icons/aki-icon.ico，scripts/make_app_icons.py
# 生成）。eui_neo_configure_app 经 EUI_NEO_APP_ICON_RESOURCE 全局属性把上游
# assets/icon.ico 编入每个 app target；pinned 依赖不可改动，故在消费侧于
# add_subdirectory 之后、configure 之前覆写该属性为 Aki 自己的 rc（仅影响
# 本仓库 target；上游若改名则覆写静默失效、退回 EUI-NEO 图标，不致构建
# 失败）。运行期窗口图标为 assets/icons/aki-icon.png（main.cpp iconPath），
# 由 aki target 的 POST_BUILD 部署（见 eui_neo_configure_app(aki) 之后）。
if(WIN32)
    set(AKI_APP_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/aki-icon.ico")
    if(NOT EXISTS "${AKI_APP_ICON_ICO}")
        message(FATAL_ERROR
            "Aki app icon missing: ${AKI_APP_ICON_ICO} "
            "(regenerate via scripts/make_app_icons.py)")
    endif()
    set(AKI_APP_ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/aki_app_icon.rc")
    file(TO_NATIVE_PATH "${AKI_APP_ICON_ICO}" AKI_APP_ICON_ICO_NATIVE)
    string(REPLACE "\\" "\\\\" AKI_APP_ICON_ICO_ESCAPED "${AKI_APP_ICON_ICO_NATIVE}")
    file(WRITE "${AKI_APP_ICON_RC}"
        "IDI_APP_ICON ICON \"${AKI_APP_ICON_ICO_ESCAPED}\"\n")
    set_property(GLOBAL PROPERTY EUI_NEO_APP_ICON_RESOURCE "${AKI_APP_ICON_RC}")
endif()

# executor 公开头文件按 SYSTEM include 处理：其头文件告警（如 MSVC C4324 对齐
# 填充）不进入第一方 -Werror 门禁，与“第三方依赖不继承告警级别”的约定一致。
# M3-01 起设置转移到 heyaki 提供的 executor target（Aki 侧副本不再进入构建图，
# DEC-006/DEC-003 修订）。
# 门控说明：CMake 3.x 的 VS/Xcode IDE 生成器在 generate 阶段拒绝“源目录前缀路径”
# 进入 INTERFACE_SYSTEM_INCLUDE_DIRECTORIES（CI Windows job 实测报错），而 MSVC 上
# 该标注本就未落为 /external:I（见下方 /wd4324 注），故 IDE 生成器跳过此设置，
# 行为不变；Makefile/Ninja 生成器（CI Linux 三档）保持 SYSTEM 标注。
if(NOT CMAKE_GENERATOR MATCHES "Visual Studio|Xcode")
    set_target_properties(executor PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/third_party/heyaki/third_party/executor/include")
    # M5-02（DEC-005）：eui_neo 公开头文件同款 SYSTEM 标注——EUI-NEO 头文件
    # 告警不进入第一方 -Werror 门禁（与 executor/heyaki 的「第三方依赖不
    # 继承告警级别」纪律一致；IDE 生成器限制同上，跳过时行为不变）。
    set_target_properties(eui_neo PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/third_party/EUI-NEO/include")
endif()

# 警告级别只作用于第一方目标；第三方依赖（FetchContent 引入的测试框架等）不继承，
# 避免上游告警被 -Werror 放大成构建失败。
# MSVC 的 /utf-8：源码为 UTF-8 无 BOM，缺省 GBK 代码页会把多字节序列与换行符合并，
# 破坏后续行解析（M1 验证中实际复现）。
function(aki_apply_warnings target scope)
    if(MSVC)
        # /wd4324：executor 公开头文件对齐填充告警会命中第一方 /WX（其 SYSTEM 标注
        # 在 MSVC 上未被 CMake 落为 /external:I）；executor 自身目标同样豁免该告警，
        # 此处与其保持一致（第三方头文件告警不进第一方门禁）。
        target_compile_options(${target} ${scope} /W4 /permissive- /EHsc /utf-8 /wd4324)
    else()
        target_compile_options(${target} ${scope} -Wall -Wextra -Wpedantic)
        # DEC-014：GCC 对 ThreadSanitizer 不插桩 atomic fence 发 -Wtsan
        #（heyaki 先例 HeyakiProjectOptions——Asio 有意使用 fence）。tsan preset
        # 下第一方原子/栅栏代码会被命中：保留诊断但不升错（限定 GNU + 线程
        # 插桩构建，避免 CI tsan 档因工具链限制失败）。
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
           AND CMAKE_CXX_FLAGS MATCHES "-fsanitize=thread")
            target_compile_options(${target} ${scope}
                "$<$<COMPILE_LANGUAGE:CXX>:-Wno-error=tsan>")
        endif()
        # M5-02 CI（Linux GNU 档首跑）：EUI-NEO 公开头文件自带尾成员省略式聚合
        # 初始化（components/theme.h light()/dark() 省略末位 metrics 成员，
        # theme.h:165/178；C++ 聚合初始化其余成员值初始化，语义合法），在消费
        # TU 内命中 -Wextra 的 -Wmissing-field-initializers 并被 -Werror 放大
        # 为构建失败（MSVC 无对应告警，本机 /W4 /WX 全量构建 0 warning 已证；
        # eui 消费面为 aki_ui 与根 main.cpp，DEC-005 单一构建图）。third_party
        # 不可改动（仓库纪律），按本函数既定纪律「第三方头文件告警不进第一方
        # 门禁」仅解除升错、保留诊断可见（与上方 -Wno-error=tsan 同型）。
        target_compile_options(${target} ${scope}
            "$<$<COMPILE_LANGUAGE:CXX>:-Wno-error=missing-field-initializers>")
    endif()
    if(AKI_WARNINGS_AS_ERRORS)
        if(MSVC)
            target_compile_options(${target} ${scope} /WX)
        else()
            target_compile_options(${target} ${scope} -Werror)
        endif()
    endif()
endfunction()

# OpenSSL 运行期库定位候选根（M6-01 起由 POST_BUILD 部署与 CPack install
# 共用）：CMake/OpenSSL 变量与 OPENSSL_ROOT_DIR 环境变量（CI 注入），
# OPENSSL_INCLUDE_DIR 推导根殿后；后命中者优先。
function(aki_openssl_root_candidates out_var)
    set(_roots "")
    if(DEFINED OPENSSL_ROOT_DIR AND OPENSSL_ROOT_DIR)
        list(APPEND _roots "${OPENSSL_ROOT_DIR}")
    endif()
    if(DEFINED ENV{OPENSSL_ROOT_DIR} AND NOT "$ENV{OPENSSL_ROOT_DIR}" STREQUAL "")
        list(APPEND _roots "$ENV{OPENSSL_ROOT_DIR}")
    endif()
    if(DEFINED OPENSSL_INCLUDE_DIR AND OPENSSL_INCLUDE_DIR)
        cmake_path(SET _openssl_inc NORMALIZE "${OPENSSL_INCLUDE_DIR}")
        cmake_path(GET _openssl_inc PARENT_PATH _openssl_root)
        list(APPEND _roots "${_openssl_root}")
    endif()
    list(REMOVE_DUPLICATES _roots)
    set(${out_var} "${_roots}" PARENT_SCOPE)
endfunction()

# OpenSSL 3 运行期 DLL 定位（Windows；DLL 位于候选根或 bin/ 子目录，
# 未命中输出空串）。
function(aki_locate_openssl_dll dll out_var)
    aki_openssl_root_candidates(_roots)
    set(_dll_found "")
    foreach(_root IN LISTS _roots)
        foreach(_sub IN ITEMS "" bin)
            if(EXISTS "${_root}/${_sub}/${dll}")
                set(_dll_found "${_root}/${_sub}/${dll}")
            endif()
        endforeach()
    endforeach()
    set(${out_var} "${_dll_found}" PARENT_SCOPE)
endfunction()

# OpenSSL 3 运行期共享库定位（Linux，M6-02；.so SONAME 位于候选根或
# lib/lib64 子目录，未命中输出空串。install(FILES) 跟随 symlink 拷贝
# 真实文件，随包形态为无符号链接的同名库）。
function(aki_locate_openssl_shared_lib so out_var)
    aki_openssl_root_candidates(_roots)
    set(_so_found "")
    foreach(_root IN LISTS _roots)
        foreach(_sub IN ITEMS lib lib64)
            if(EXISTS "${_root}/${_sub}/${so}")
                set(_so_found "${_root}/${_sub}/${so}")
            endif()
        endforeach()
    endforeach()
    set(${out_var} "${_so_found}" PARENT_SCOPE)
endfunction()

# M3-01：Windows 运行期 OpenSSL DLL 部署（DEC-006 影响节：MSVC 链接系统 OpenSSL 3，
# 测试可执行需 libssl-3-x64.dll / libcrypto-3-x64.dll 就位）。仅在显式依赖
# heyaki::client 的目标上调用（当前为 test_heyaki_client_surface；Aki 产品目标
# M3-03 起才链真实 heyaki）。
function(aki_deploy_openssl_dlls target)
    if(NOT WIN32)
        return()
    endif()
    foreach(_dll IN ITEMS libssl-3-x64.dll libcrypto-3-x64.dll)
        aki_locate_openssl_dll("${_dll}" _dll_found)
        if(_dll_found STREQUAL "")
            message(WARNING
                "OpenSSL runtime DLL '${_dll}' not found under OPENSSL_ROOT_DIR; "
                "'${target}' may fail to start. Set OPENSSL_ROOT_DIR to the "
                "OpenSSL 3 install prefix (DEC-006, M3-01).")
            return()
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll_found}" "$<TARGET_FILE_DIR:${target}>"
            COMMENT "deploying ${_dll} for ${target}")
    endforeach()
endfunction()

# 领域骨架库（设计第 14 节目录结构）：M0 先锁定目录与依赖方向，领域实现自 M1 起填充。
function(aki_add_skeleton_library name)
    add_library(${name} INTERFACE)
    target_compile_features(${name} INTERFACE cxx_std_20)
    target_include_directories(${name} INTERFACE ${PROJECT_SOURCE_DIR})
    aki_apply_warnings(${name} INTERFACE)
endfunction()

add_subdirectory(app)
add_subdirectory(device)
add_subdirectory(conversation)
add_subdirectory(transfer)
add_subdirectory(persistence)
add_subdirectory(heyaki)
add_subdirectory(ui)

# M5-02：GUI 宿主（EUI-NEO 框架 main 注入；/SUBSYSTEM:WINDOWS）。宿主组合根
# 在 aki_host（EUI-NEO 无关，app/lifecycle/host_runtime），main.cpp 只保留
# dslAppConfig()+compose() 钩子与 onShutdown 薄委托（设计 §9.1）。eui::neo
# 经 eui_neo_configure_app 链入 exe（DEC-005 集成形态）；库面仅 aki_ui 消费。
add_executable(aki main.cpp)
aki_apply_warnings(aki PRIVATE)
# M3-03：宿主经 heyaki/adapter/local_identity.hpp 消费真实 heyaki 身份
# （DEC-006 单图接入的产品目标首批消费点）；OpenSSL 运行期 DLL 随宿主部署。
target_link_libraries(aki PRIVATE
    aki_host
    aki_ui
    aki_ui_models
    heyaki::client)
aki_deploy_openssl_dlls(aki)
eui_neo_configure_app(aki)
# 运行期图标资产随 exe 部署（POST_BUILD 注册序在 eui_neo_configure_app 的
# 上游 assets 拷贝之后执行；文件名 distinct，与上游 assets/ 无覆盖关系）。
add_custom_command(TARGET aki POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:aki>/assets/icons"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/aki-icon.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/aki-icon.ico"
        "$<TARGET_FILE_DIR:aki>/assets/icons"
    VERBATIM)
# M6-01（DEC-017）：分发构建旗标基线覆盖。eui_neo_configure_app 经
# eui_apply_compile_options 对非 Debug 配置注入上游 demo 策略旗标——
# GNU/Clang：-Os -fno-exceptions -fno-rtti（-fno-exceptions 破坏
# executor/heyaki 的异常语义，GCC release 构建无法编译：M6-01 IVA 实测；
# CI 门禁此前仅覆盖 Debug，release 首次由打包链路触发）；MSVC：/O1
# /GS- /sdl-（关闭栈保护与安全检查，不满足分发基线）。pinned 依赖不可
# 改动（仓库纪律），在消费侧以同一 target 的后置 target_compile_options
# 覆盖（按调用序拼接，后写者在 GCC/MSVC 命令行中生效）：GNU/Clang 恢复
# -fexceptions -frtti -O2，MSVC 恢复 /O2 /GS /sdl。生成器表达式限定非
# Debug，与上游注入条件对齐，debug 构建不受影响。
if(NOT MSVC)
    target_compile_options(aki PRIVATE
        "$<$<NOT:$<CONFIG:Debug>>:-fexceptions;-frtti;-O2>")
else()
    target_compile_options(aki PRIVATE
        "$<$<NOT:$<CONFIG:Debug>>:/O2;/GS;/sdl>")
endif()
# 框架注入源（glfw_app_main.cpp）不继承第一方 /W4 /WX——上游源码告警
#（如 runtime_render.h C4100）不进第一方门禁（DEC-005「第三方不继承告警
# 级别」；MSVC 无 /external:I 落点，以源级降档实现，最后一档生效）。
get_property(AKI_EUI_APP_MAIN_SOURCE GLOBAL PROPERTY EUI_NEO_APP_MAIN_SOURCE)
if(MSVC)
    set_source_files_properties("${AKI_EUI_APP_MAIN_SOURCE}"
        PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties("${AKI_EUI_APP_MAIN_SOURCE}"
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

# M6-01（DEC-017）：CPack 打包基线——Windows NSIS setup.exe 与 Linux DEB
#（最低适配 Ubuntu 20.04）。install 布局与生成器配置见 cmake/Packaging.cmake；
# 目标平台构建基线（glibc 2.31、GCC 12、静态 libstdc++/libgcc）由 CI
# package job 的 ubuntu:20.04 容器保证。须在 aki target 定义之后引入。
include(cmake/Packaging.cmake)

if(AKI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
