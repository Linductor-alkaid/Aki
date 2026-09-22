cmake_minimum_required(VERSION 3.25)

project(aki VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AKI_BUILD_TESTS "Build Aki tests" ON)
option(AKI_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

include(cmake/Dependencies.cmake)

# M1（executor）目标级接入（DEC-003、Dependencies.cmake 头注）：pinned executor
# 进入构建，executor::comm 承载 Application State 跨上下文交付（EXEC-03，M1-02），
# 库本体供测试进程 Executor owner 与 M1-04/05 的任务承载使用。
# executor 自带的 tests/examples 与 GPU/CUDA/OpenCL 探测在 Aki 构建中显式关闭，
# 保证 Windows(MinGW/MSVC) 与 Linux(CI) 三套工具链的确定性配置。
set(EXECUTOR_BUILD_TESTS OFF)
set(EXECUTOR_BUILD_EXAMPLES OFF)
set(EXECUTOR_ENABLE_GPU OFF)
set(EXECUTOR_ENABLE_CUDA OFF)
set(EXECUTOR_ENABLE_OPENCL OFF)
add_subdirectory(third_party/executor executor.system EXCLUDE_FROM_ALL)

# M2（sqlite）目标级接入（DEC-004、M2-02）：vendored amalgamation 编译为静态库，
# 完整性由 cmake/Dependencies.cmake 在 configure 时按锁文件 SHA-256 校验；
# 第三方 target 不继承第一方告警级别（DEC-007 纪律），见 third_party/sqlite/CMakeLists.txt。
add_subdirectory(third_party/sqlite)

# executor 公开头文件按 SYSTEM include 处理：其头文件告警（如 MSVC C4324 对齐填充）
# 不进入第一方 -Werror 门禁，与“第三方依赖不继承告警级别”的约定一致。
# 门控说明：CMake 3.x 的 VS/Xcode IDE 生成器在 generate 阶段拒绝“源目录前缀路径”
# 进入 INTERFACE_SYSTEM_INCLUDE_DIRECTORIES（CI Windows job 实测报错），而 MSVC 上
# 该标注本就未落为 /external:I（见下方 /wd4324 注），故 IDE 生成器跳过此设置，
# 行为不变；Makefile/Ninja 生成器（CI Linux 三档）保持 SYSTEM 标注。
if(NOT CMAKE_GENERATOR MATCHES "Visual Studio|Xcode")
    set_target_properties(executor PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/third_party/executor/include")
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
    endif()
    if(AKI_WARNINGS_AS_ERRORS)
        if(MSVC)
            target_compile_options(${target} ${scope} /WX)
        else()
            target_compile_options(${target} ${scope} -Werror)
        endif()
    endif()
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

add_executable(aki main.cpp)
aki_apply_warnings(aki PRIVATE)
target_link_libraries(aki PRIVATE
    aki_app
    aki_device
    aki_conversation
    aki_transfer
    aki_persistence
    aki_heyaki
    aki_ui)

if(AKI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
