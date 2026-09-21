# M1-07（DEC-007）：测试框架为 Catch2 v3，FetchContent 锁定 commit。
# 标签约定：unit / integration / smoke；CI 按标签选择执行集（CI 基线随 M0 收尾建立）。
include(FetchContent)

set(AKI_CATCH2_COMMIT "644821ce28cb25d7992a4d0375b1d83214392592" CACHE STRING
    "Pinned Catch2 commit (v3.9.1), see docs/decisions/DEC-007-test-framework.md")

FetchContent_Declare(catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG ${AKI_CATCH2_COMMIT}
    GIT_SHALLOW TRUE)
set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(catch2)

function(aki_add_unit_test name)
    add_executable(${name} ${ARGN})
    target_compile_features(${name} PRIVATE cxx_std_20)
    aki_apply_warnings(${name} PRIVATE)
    target_link_libraries(${name} PRIVATE
        Catch2::Catch2WithMain
        aki_device
        aki_conversation
        aki_transfer)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "unit")
endfunction()

add_executable(aki_skeleton_test test_skeleton.cpp)
target_compile_features(aki_skeleton_test PRIVATE cxx_std_20)
aki_apply_warnings(aki_skeleton_test PRIVATE)
target_link_libraries(aki_skeleton_test PRIVATE
    aki_app
    aki_device
    aki_conversation
    aki_transfer
    aki_persistence
    aki_heyaki
    aki_ui)

add_test(NAME skeleton.includes COMMAND aki_skeleton_test)
set_tests_properties(skeleton.includes PROPERTIES LABELS "unit")

add_test(NAME skeleton.app_runs COMMAND aki)
set_tests_properties(skeleton.app_runs PROPERTIES
    LABELS "smoke"
    PASS_REGULAR_EXPRESSION "aki 0\\.1\\.0")

aki_add_unit_test(test_trust_state unit/test_trust_state.cpp)
aki_add_unit_test(test_delivery_state unit/test_delivery_state.cpp)
aki_add_unit_test(test_conversation_state unit/test_conversation_state.cpp)
aki_add_unit_test(test_transfer_state unit/test_transfer_state.cpp)

# M1-02：Application State 单写者边界测试。本测试进程的唯一 Executor owner 是
# 测试自身的 main()（AGENTS.md 规则 7；正式 owner 由 M1-04 提供），因此链接
# Catch2 无 main 变体并自带 main；executor::executor 经 aki_app 传播。
add_executable(test_app_state unit/test_app_state.cpp)
target_compile_features(test_app_state PRIVATE cxx_std_20)
aki_apply_warnings(test_app_state PRIVATE)
target_link_libraries(test_app_state PRIVATE
    Catch2::Catch2
    aki_app
    aki_device
    aki_conversation
    aki_transfer)
add_test(NAME test_app_state COMMAND test_app_state)
set_tests_properties(test_app_state PROPERTIES LABELS "unit")
