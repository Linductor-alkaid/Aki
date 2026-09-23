# persistence 层（设计第 11/14 节）：M2-03 起为实体接口面——薄 RAII 封装
# （persistence/database）与版本化迁移框架（persistence/migration），DEC-004。
# sqlite3 以 PRIVATE 链接：sqlite3* 封死在本层编译单元内（RULE-10），
# 消费者（Manager/宿主/测试）拿不到 sqlite include 路径，公开头只含 std 类型。
add_library(aki_persistence STATIC
    database/database.cpp
    database/database_worker.cpp
    migration/migration.cpp
    migration/schema_v1.cpp
    repository/repositories.cpp
    repository/statement_cache.cpp
    storage/data_root.cpp
    storage/file_jobs.cpp
    storage/file_store.cpp
    storage/sha256.cpp)
target_compile_features(aki_persistence PUBLIC cxx_std_20)
aki_apply_warnings(aki_persistence PRIVATE)
target_include_directories(aki_persistence PUBLIC ${PROJECT_SOURCE_DIR})
# sqlite3 与 executor 均为 PRIVATE：两者类型都封死在本层（RULE-10；消费者
# 经公开头只见领域/std 类型；注册侧接线头 database_worker_adapter.hpp 的
# executor 依赖由测试/宿主目标各自链接 executor 提供）。
target_link_libraries(aki_persistence PRIVATE sqlite3 executor::executor)
