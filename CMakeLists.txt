cmake_minimum_required(VERSION 3.25)

project(aki VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AKI_BUILD_TESTS "Build Aki tests" ON)
option(AKI_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

include(cmake/Dependencies.cmake)

# 警告级别只作用于第一方目标；第三方依赖（FetchContent 引入的测试框架等）不继承，
# 避免上游告警被 -Werror 放大成构建失败。
# MSVC 的 /utf-8：源码为 UTF-8 无 BOM，缺省 GBK 代码页会把多字节序列与换行符合并，
# 破坏后续行解析（M1 验证中实际复现）。
function(aki_apply_warnings target scope)
    if(MSVC)
        target_compile_options(${target} ${scope} /W4 /permissive- /EHsc /utf-8)
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
