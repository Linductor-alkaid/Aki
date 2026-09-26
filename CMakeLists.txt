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
# （双侧同 pin 74a94198，构建图内只编译一份 executor）——Aki 移除自己的
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
    endif()
    if(AKI_WARNINGS_AS_ERRORS)
        if(MSVC)
            target_compile_options(${target} ${scope} /WX)
        else()
            target_compile_options(${target} ${scope} -Werror)
        endif()
    endif()
endfunction()

# M3-01：Windows 运行期 OpenSSL DLL 部署（DEC-006 影响节：MSVC 链接系统 OpenSSL 3，
# 测试可执行需 libssl-3-x64.dll / libcrypto-3-x64.dll 就位）。仅在显式依赖
# heyaki::client 的目标上调用（当前为 test_heyaki_client_surface；Aki 产品目标
# M3-03 起才链真实 heyaki）。根目录候选：CMake/OpenSSL 变量与 OPENSSL_ROOT_DIR
# 环境变量（CI 注入），DLL 位于根或 bin/ 子目录。
function(aki_deploy_openssl_dlls target)
    if(NOT WIN32)
        return()
    endif()
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
    foreach(_dll IN ITEMS libssl-3-x64.dll libcrypto-3-x64.dll)
        set(_dll_found "")
        foreach(_root IN LISTS _roots)
            foreach(_sub IN ITEMS "" bin)
                if(EXISTS "${_root}/${_sub}/${_dll}")
                    set(_dll_found "${_root}/${_sub}/${_dll}")
                endif()
            endforeach()
        endforeach()
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
    heyaki::client)
aki_deploy_openssl_dlls(aki)
eui_neo_configure_app(aki)
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

if(AKI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
