# Vendored SQLite（DEC-004，M2-02 接入）：官方 amalgamation 仅取 sqlite3.c/sqlite3.h。
# 第三方 target 不继承第一方告警级别（DEC-007 纪律，不调用 aki_apply_warnings）；
# 编译宏按 DEC-004：SQLITE_DQS=0、SQLITE_OMIT_LOAD_EXTENSION=1。
# 完整性与版本由 cmake/Dependencies.cmake 在 configure 时按
# third_party/dependencies.lock.json 校验（漂移即 FATAL_ERROR）。
enable_language(C)

add_library(sqlite3 STATIC sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_DQS=0
    SQLITE_OMIT_LOAD_EXTENSION=1)
