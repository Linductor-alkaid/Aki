cmake_minimum_required(VERSION 3.25)

project(aki VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AKI_BUILD_TESTS "Build Aki tests" ON)
option(AKI_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

include(cmake/Dependencies.cmake)

if(MSVC)
    add_compile_options(/W4 /permissive- /EHsc)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(AKI_WARNINGS_AS_ERRORS)
    if(MSVC)
        add_compile_options(/WX)
    else()
        add_compile_options(-Werror)
    endif()
endif()

# 领域骨架库（设计第 14 节目录结构）：M0 先锁定目录与依赖方向，领域实现自 M1 起填充。
function(aki_add_skeleton_library name)
    add_library(${name} INTERFACE)
    target_compile_features(${name} INTERFACE cxx_std_20)
    target_include_directories(${name} INTERFACE ${PROJECT_SOURCE_DIR})
endfunction()

add_subdirectory(app)
add_subdirectory(device)
add_subdirectory(conversation)
add_subdirectory(transfer)
add_subdirectory(persistence)
add_subdirectory(heyaki)
add_subdirectory(ui)

add_executable(aki main.cpp)
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
