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

add_test(NAME skeleton.app_runs COMMAND aki "${CMAKE_CURRENT_BINARY_DIR}/aki-smoke-data")
set_tests_properties(skeleton.app_runs PROPERTIES
    LABELS "smoke"
    PASS_REGULAR_EXPRESSION "aki 0\\.1\\.0")

# M1-06：console 冒烟宿主（v0.1.0 验收载体，覆盖 M1 退出-1）。integration 标签
# 自本项启用（DEC-007）；宿主即进程内组合根（正式 ExecutorOwner，设计第 8.2/8.3
# 节），输出以 "smoke: PASS"/"smoke: FAIL" 收尾供正则断言。
# M2-07 起：数据根基址经 argv 注入（宿主每次运行使用基址下独立子目录并自清理，
# 不触碰真实 %APPDATA%/XDG 目录），宿主内含重启恢复逐域断言（M2 退出-1）。
add_test(NAME smoke.device_lifecycle COMMAND aki "${CMAKE_CURRENT_BINARY_DIR}/aki-smoke-data")
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
    aki_transfer
    aki_persistence)
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

# M2-07：重启恢复集成测试（设计第 11.1 节 ②③；DEC-004 验证方式；SCOPE-09）。
# 组合镜像宿主路径：恢复 → 播种 → 注册 worker → 写入 → 排空关闭 → 重开逐域
# 断言；损坏 DB 干净失败；清扫；宿主路径排空不丢作业。
add_executable(test_restart_recovery integration/test_restart_recovery.cpp)
target_compile_features(test_restart_recovery PRIVATE cxx_std_20)
aki_apply_warnings(test_restart_recovery PRIVATE)
target_link_libraries(test_restart_recovery PRIVATE
    Catch2::Catch2WithMain
    aki_persistence
    aki_app
    heyaki::client)
aki_deploy_openssl_dlls(test_restart_recovery)
add_test(NAME test_restart_recovery COMMAND test_restart_recovery)
set_tests_properties(test_restart_recovery PROPERTIES LABELS "integration")

# M2-07 验收 ②（宿主级）：损坏 DB 文件的组合根干净失败——aki 以 --exact 模式
# 直用损坏数据根，open 失败时输出 "[FATAL] startup recovery failed: <原因>"
# 并以非零退出码退出（不静默）。损坏文件在 configure 期固定写入测试二进制目录。
set(AKI_CORRUPT_DB_ROOT "${CMAKE_CURRENT_BINARY_DIR}/aki-corrupt-db-root")
file(MAKE_DIRECTORY "${AKI_CORRUPT_DB_ROOT}/db")
file(WRITE "${AKI_CORRUPT_DB_ROOT}/db/aki.db3"
    "this is not a sqlite database - M2-07 corrupt-db clean-failure fixture\n")
add_test(NAME recovery.corrupt_db_clean_failure
    COMMAND aki "${AKI_CORRUPT_DB_ROOT}" --exact)
set_tests_properties(recovery.corrupt_db_clean_failure PROPERTIES
    LABELS "integration"
    PASS_REGULAR_EXPRESSION "\\[FATAL\\] startup recovery failed")

# M3-01：heyaki::client 消费边界锁定（DEC-006 单图接入的唯一显式消费点；
# RULE-01/RULE-10）。链接 aki_persistence + heyaki::client：同一二进制内
# aki vendored sqlite 3.53.4（DEC-004 锁定）与 heyaki::sqlite 3.50.4 两份静态
# 副本并存，静态链接按序解析为单份——aki_persistence 必须列在 heyaki::client
# 之前：aki 侧 sqlite3_* 先被 Aki 经哈希审计的 vendored 副本满足，
# heyaki_sqlite 不会被拉入（有效版本运行期经 SQL 断言 == Aki 锁文件；实测
# heyaki 在前时有效版本为 3.50.4，见 M3-01 验证记录的双 SQLite 处置）。
# 符号单份性由 dumpbin/nm 离线证明。
add_executable(test_heyaki_client_surface unit/test_heyaki_client_surface.cpp)
target_compile_features(test_heyaki_client_surface PRIVATE cxx_std_20)
aki_apply_warnings(test_heyaki_client_surface PRIVATE)
target_link_libraries(test_heyaki_client_surface PRIVATE
    aki_persistence
    heyaki::client)
target_compile_definitions(test_heyaki_client_surface PRIVATE
    AKI_EXPECTED_SQLITE_VERSION="${AKI_SQLITE_LOCK_VERSION}")
aki_deploy_openssl_dlls(test_heyaki_client_surface)
add_test(NAME test_heyaki_client_surface COMMAND test_heyaki_client_surface)
set_tests_properties(test_heyaki_client_surface PROPERTIES LABELS "unit")

# M3-03：本地设备身份接线单测（DEC-006 映射 1；SCOPE-01）。aki_heyaki 为
# INTERFACE 头文件库，local_identity 的 heyaki 符号由 heyaki::client 提供；
# OpenSSL 运行期 DLL 随测试部署。
add_executable(test_local_identity unit/test_local_identity.cpp)
target_compile_features(test_local_identity PRIVATE cxx_std_20)
aki_apply_warnings(test_local_identity PRIVATE)
target_link_libraries(test_local_identity PRIVATE
    Catch2::Catch2WithMain
    aki_heyaki
    heyaki::client)
aki_deploy_openssl_dlls(test_local_identity)
add_test(NAME test_local_identity COMMAND test_local_identity)
set_tests_properties(test_local_identity PROPERTIES LABELS "unit")

# M3-04：DOD-02 六项沿 EXEC-04 timer 路径（executor 周期能力专项；网络无关）。
# 2026-09-24 评审拆分：本二进制不含 [skip]/受控退出——同一二进制内多个各自
# 可 std::_Exit(0) 的用例会互相掩盖既有失败、并使后行用例随机不可达（工程
# 规范第 7 节）；回环用例各占独立单用例二进制（见下两项）。
add_executable(test_discovery_pairing integration/test_discovery_pairing.cpp)
target_compile_features(test_discovery_pairing PRIVATE cxx_std_20)
aki_apply_warnings(test_discovery_pairing PRIVATE)
target_link_libraries(test_discovery_pairing PRIVATE
    Catch2::Catch2WithMain
    aki_app)
add_test(NAME test_discovery_pairing COMMAND test_discovery_pairing)
set_tests_properties(test_discovery_pairing PROPERTIES LABELS "integration")

# M3-04：发现/信任双节点回环（DEC-006 映射 2/3）。单用例二进制：[skip] 受控
# 退出（握手停滞会话阻塞 Node::shutdown，无法正常展开）只在前置断言全过时
# 可达；接口缺失环境输出 [skip]（补跑条件见里程碑验证记录）。
add_executable(test_discovery_pairing_loopback
    integration/test_discovery_pairing_loopback.cpp)
target_compile_features(test_discovery_pairing_loopback PRIVATE cxx_std_20)
aki_apply_warnings(test_discovery_pairing_loopback PRIVATE)
target_link_libraries(test_discovery_pairing_loopback PRIVATE
    Catch2::Catch2WithMain
    aki_persistence
    aki_app
    aki_heyaki
    heyaki::client)
aki_deploy_openssl_dlls(test_discovery_pairing_loopback)
add_test(NAME test_discovery_pairing_loopback COMMAND test_discovery_pairing_loopback)
set_tests_properties(test_discovery_pairing_loopback PROPERTIES LABELS "integration")

# M3-06：peer_sessions 管道 presence/path 回环（DEC-006 映射 5；SCOPE-10）。
# 2026-09-24 评审拆分自 test_discovery_pairing：单用例二进制，[skip] 受控
# 退出不掩盖既有失败、回环证据不再随机不可达；映射/diff 网络无关断言见
# test_peer_sessions_pipeline（unit）。
add_executable(test_peer_sessions_loopback
    integration/test_peer_sessions_loopback.cpp)
target_compile_features(test_peer_sessions_loopback PRIVATE cxx_std_20)
aki_apply_warnings(test_peer_sessions_loopback PRIVATE)
target_link_libraries(test_peer_sessions_loopback PRIVATE
    Catch2::Catch2WithMain
    aki_persistence
    aki_app
    aki_heyaki
    heyaki::client)
aki_deploy_openssl_dlls(test_peer_sessions_loopback)
add_test(NAME test_peer_sessions_loopback COMMAND test_peer_sessions_loopback)
set_tests_properties(test_peer_sessions_loopback PROPERTIES LABELS "integration")

# M3-05：文本消息真实化（DEC-006 映射 4；双节点回环；防火墙受限环境按 M3-04
# 先例 [skip] 降级，证据输出保留）。
add_executable(test_message_loopback integration/test_message_loopback.cpp)
target_compile_features(test_message_loopback PRIVATE cxx_std_20)
aki_apply_warnings(test_message_loopback PRIVATE)
target_link_libraries(test_message_loopback PRIVATE
    Catch2::Catch2WithMain
    aki_app
    aki_heyaki
    heyaki::client)
aki_deploy_openssl_dlls(test_message_loopback)
add_test(NAME test_message_loopback COMMAND test_message_loopback)
set_tests_properties(test_message_loopback PROPERTIES LABELS "integration")

# M3-07：断线恢复回环（DEC-006 映射 6；SCOPE-11；EXEC-05 重连长任务）。单
# 用例二进制（[skip] 受控退出与失败隔离纪律沿 M3-05/M3-06 评审拆分）。
add_executable(test_disconnect_recovery_loopback
    integration/test_disconnect_recovery_loopback.cpp)
target_compile_features(test_disconnect_recovery_loopback PRIVATE cxx_std_20)
aki_apply_warnings(test_disconnect_recovery_loopback PRIVATE)
target_link_libraries(test_disconnect_recovery_loopback PRIVATE
    Catch2::Catch2WithMain
    aki_app
    aki_heyaki
    heyaki::client)
aki_deploy_openssl_dlls(test_disconnect_recovery_loopback)
add_test(NAME test_disconnect_recovery_loopback
    COMMAND test_disconnect_recovery_loopback)
set_tests_properties(test_disconnect_recovery_loopback PROPERTIES LABELS "integration")

# M3-08：真实 Adapter SPI 全闭环回环（DEC-006 映射 1~4/6；宿主切换验证面）。
# 单用例二进制（[skip] 纪律同 M3-05/06/07）。
add_executable(test_real_adapter_loopback integration/test_real_adapter_loopback.cpp)
target_compile_features(test_real_adapter_loopback PRIVATE cxx_std_20)
aki_apply_warnings(test_real_adapter_loopback PRIVATE)
target_link_libraries(test_real_adapter_loopback PRIVATE
    Catch2::Catch2WithMain
    aki_persistence
    aki_app
    aki_heyaki
    heyaki::client)
aki_deploy_openssl_dlls(test_real_adapter_loopback)
add_test(NAME test_real_adapter_loopback COMMAND test_real_adapter_loopback)
set_tests_properties(test_real_adapter_loopback PROPERTIES LABELS "integration")

# M3-08（评审补证）：统一真实 Adapter SPI + 入站注入面单元测试（网络无关：
# 构造校验/出站校验/sink 分发/析构闭合——Adapter 类在测试中真实构造执行）。
add_executable(test_heyaki_node_adapter unit/test_heyaki_node_adapter.cpp)
target_compile_features(test_heyaki_node_adapter PRIVATE cxx_std_20)
aki_apply_warnings(test_heyaki_node_adapter PRIVATE)
target_link_libraries(test_heyaki_node_adapter PRIVATE
    Catch2::Catch2WithMain
    aki_app
    aki_heyaki
    heyaki::client)
aki_deploy_openssl_dlls(test_heyaki_node_adapter)
add_test(NAME test_heyaki_node_adapter COMMAND test_heyaki_node_adapter)
set_tests_properties(test_heyaki_node_adapter PROPERTIES LABELS "unit")

# M3-06：peer_sessions diff 管道单元测试（网络无关：ConnectionPath 映射 +
# authenticated↔closed 转移；管道层 DOD-02 沿 M3-04 periodic 六项）。
add_executable(test_peer_sessions_pipeline unit/test_peer_sessions_pipeline.cpp)
target_compile_features(test_peer_sessions_pipeline PRIVATE cxx_std_20)
aki_apply_warnings(test_peer_sessions_pipeline PRIVATE)
target_link_libraries(test_peer_sessions_pipeline PRIVATE
    Catch2::Catch2WithMain
    aki_app
    aki_heyaki
    heyaki::client)
add_test(NAME test_peer_sessions_pipeline COMMAND test_peer_sessions_pipeline)
set_tests_properties(test_peer_sessions_pipeline PROPERTIES LABELS "unit")

# M3-07：重连协调器单元测试（EXEC-05 长任务取消/超时闭合；DEC-008 宿主关闭
# 纪律；DOD-02 六项沿重连长任务路径；注入回调，网络无关）。
add_executable(test_reconnect_loop unit/test_reconnect_loop.cpp)
target_compile_features(test_reconnect_loop PRIVATE cxx_std_20)
aki_apply_warnings(test_reconnect_loop PRIVATE)
target_link_libraries(test_reconnect_loop PRIVATE
    Catch2::Catch2WithMain
    aki_app)
add_test(NAME test_reconnect_loop COMMAND test_reconnect_loop)
set_tests_properties(test_reconnect_loop PROPERTIES LABELS "unit")

# M3-05（评审拆分）：MessageId 双射 + aki.text 信封往返（DEC-006 冻结；网络
# 无关）。独立 unit 二进制：回环二进制的 [skip] 受控退出不得掩盖同二进制内
# 既有失败（工程规范第 7 节），网络无关断言在此始终完整执行。
add_executable(test_heyaki_message unit/test_heyaki_message.cpp)
target_compile_features(test_heyaki_message PRIVATE cxx_std_20)
aki_apply_warnings(test_heyaki_message PRIVATE)
target_link_libraries(test_heyaki_message PRIVATE
    Catch2::Catch2WithMain
    aki_app
    aki_heyaki
    heyaki::client)
aki_deploy_openssl_dlls(test_heyaki_message)
add_test(NAME test_heyaki_message COMMAND test_heyaki_message)
set_tests_properties(test_heyaki_message PROPERTIES LABELS "unit")
