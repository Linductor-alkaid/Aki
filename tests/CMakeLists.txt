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

# M1-06：console 冒烟宿主（v0.1.0 验收载体，覆盖 M1 退出-1）。integration 标签
# 自本项启用（DEC-007）；宿主即进程内组合根（正式 ExecutorOwner，设计第 8.2/8.3
# 节），输出以 "smoke: PASS"/"smoke: FAIL" 收尾供正则断言。
add_test(NAME smoke.device_lifecycle COMMAND aki)
set_tests_properties(smoke.device_lifecycle PROPERTIES
    LABELS "integration"
    PASS_REGULAR_EXPRESSION "smoke: PASS")

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

# M1-03：Heyaki Adapter SPI 与 FakeHeyakiAdapter 测试。同为自带 main 的
# Executor owner（AGENTS.md 规则 7）；aki_heyaki 经仓库根 include 目录使用
# 头文件形式接入，链接经 aki_app 传播 executor::executor。
add_executable(test_heyaki_adapter unit/test_heyaki_adapter.cpp)
target_compile_features(test_heyaki_adapter PRIVATE cxx_std_20)
aki_apply_warnings(test_heyaki_adapter PRIVATE)
target_link_libraries(test_heyaki_adapter PRIVATE
    Catch2::Catch2
    aki_app
    aki_device
    aki_conversation
    aki_transfer
    aki_heyaki)
add_test(NAME test_heyaki_adapter COMMAND test_heyaki_adapter)
set_tests_properties(test_heyaki_adapter PROPERTIES LABELS "unit")

# M1-04：Executor 生命周期 owner（EXEC-01 五步 + shutdown 证据）测试。
# 被测对象即 owner，本目标不设进程级临时 Executor（设计第 8.2 节落点说明）。
add_executable(test_executor_lifecycle unit/test_executor_lifecycle.cpp)
target_compile_features(test_executor_lifecycle PRIVATE cxx_std_20)
aki_apply_warnings(test_executor_lifecycle PRIVATE)
target_link_libraries(test_executor_lifecycle PRIVATE
    Catch2::Catch2
    aki_app
    aki_device
    aki_conversation
    aki_transfer)
add_test(NAME test_executor_lifecycle COMMAND test_executor_lifecycle)
set_tests_properties(test_executor_lifecycle PROPERTIES LABELS "unit")

# M1-05：Device/Conversation/Message/Transfer Manager 骨架（DEC-008）测试。
# 被测对象持有各自用例的 ExecutorOwner（独立实例，AGENTS 规则 7/8；被测对象即
# 组合根的一部分，设计第 8.3 节装配顺序），不设进程级临时 Executor。
add_executable(test_app_managers unit/test_app_managers.cpp)
target_compile_features(test_app_managers PRIVATE cxx_std_20)
aki_apply_warnings(test_app_managers PRIVATE)
target_link_libraries(test_app_managers PRIVATE
    Catch2::Catch2
    aki_app
    aki_device
    aki_conversation
    aki_transfer
    aki_heyaki)
add_test(NAME test_app_managers COMMAND test_app_managers)
set_tests_properties(test_app_managers PROPERTIES LABELS "unit")

# M2-02：vendored SQLite sourceid 断言（DEC-004 建议项；Catch2 单测，DEC-007）。
# 期望版本由 cmake/Dependencies.cmake 从 third_party/dependencies.lock.json 注入。
aki_add_unit_test(test_sqlite_sourceid unit/test_sqlite_sourceid.cpp)
target_compile_definitions(test_sqlite_sourceid PRIVATE
    AKI_EXPECTED_SQLITE_VERSION="${AKI_SQLITE_LOCK_VERSION}")
# 直连 sqlite3 target：M2-03 起 aki_persistence 以 PRIVATE 链接 sqlite3（RULE-10，
# 不向消费者传播 include/库），本用例测的就是 vendored sqlite3 本体。
target_link_libraries(test_sqlite_sourceid PRIVATE sqlite3)

# M2-03：persistence 薄 RAII 封装与迁移框架测试（DEC-004；设计第 11.1 节 ②）。
add_executable(test_persistence_database unit/test_persistence_database.cpp)
target_compile_features(test_persistence_database PRIVATE cxx_std_20)
aki_apply_warnings(test_persistence_database PRIVATE)
target_link_libraries(test_persistence_database PRIVATE
    Catch2::Catch2WithMain
    aki_persistence)
add_test(NAME test_persistence_database COMMAND test_persistence_database)
set_tests_properties(test_persistence_database PROPERTIES LABELS "unit")

# RULE-10 公开边界锁定用例：消费编译单元只链接 aki_persistence（sqlite3 PRIVATE，
# 无 sqlite include 路径传播）即可编译公开头并完成一次真实使用；任一公开头
# 引入 <sqlite3.h> 本目标即编译失败。
add_executable(test_persistence_public_surface unit/test_persistence_public_surface.cpp)
target_compile_features(test_persistence_public_surface PRIVATE cxx_std_20)
aki_apply_warnings(test_persistence_public_surface PRIVATE)
target_link_libraries(test_persistence_public_surface PRIVATE aki_persistence)
add_test(NAME test_persistence_public_surface COMMAND test_persistence_public_surface)
set_tests_properties(test_persistence_public_surface PROPERTIES LABELS "unit")

# M2-04：v1 schema（ER 四表）与仓储层测试（DEC-004；设计第 11 节/11.1 节 ①④）。
add_executable(test_persistence_repository unit/test_persistence_repository.cpp)
target_compile_features(test_persistence_repository PRIVATE cxx_std_20)
aki_apply_warnings(test_persistence_repository PRIVATE)
target_link_libraries(test_persistence_repository PRIVATE
    Catch2::Catch2WithMain
    aki_persistence)
add_test(NAME test_persistence_repository COMMAND test_persistence_repository)
set_tests_properties(test_persistence_repository PROPERTIES LABELS "unit")

# M2-05：DatabaseWorker 单测（blocking worker 首次启用，DOD-02 六项 = 退出-2）。
# 链接 aki_app（ExecutorOwner）与 aki_persistence（PRIVATE executor/sqlite 由
# 本测试目标经 aki_app/executor 传播补足）；包含注册侧接线头（executor 类型
# 在接线层，RULE-10 守卫公开面不含之）。
add_executable(test_database_worker unit/test_database_worker.cpp)
target_compile_features(test_database_worker PRIVATE cxx_std_20)
aki_apply_warnings(test_database_worker PRIVATE)
target_link_libraries(test_database_worker PRIVATE
    Catch2::Catch2WithMain
    aki_persistence
    aki_app)
add_test(NAME test_database_worker COMMAND test_database_worker)
set_tests_properties(test_database_worker PROPERTIES LABELS "unit")

# M2-06：文件本体存储布局与生命周期测试（DEC-004；设计第 11.1 节 ④/②）。
add_executable(test_file_store unit/test_file_store.cpp)
target_compile_features(test_file_store PRIVATE cxx_std_20)
aki_apply_warnings(test_file_store PRIVATE)
target_link_libraries(test_file_store PRIVATE
    Catch2::Catch2WithMain
    aki_persistence
    aki_app)
add_test(NAME test_file_store COMMAND test_file_store)
set_tests_properties(test_file_store PROPERTIES LABELS "unit")
