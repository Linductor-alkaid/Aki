// M2-02：vendored SQLite 版本一致性断言（DEC-004 建议项）。
//
// 证明编译进二进制的 SQLite 就是锁文件登记的版本：header 宏（SQLITE_VERSION）、
// 编译产物运行时报告（sqlite3_libversion()）与 CMake 从
// third_party/dependencies.lock.json 注入的期望版本三方一致；并断言
// sqlite3_sourceid() 与头文件 SQLITE_SOURCE_ID 一致（header/lib 配对，无混链）。
// 锁文件本身的完整性由 cmake/Dependencies.cmake 在 configure 时按 SHA-256 强制
// （漂移即 FATAL_ERROR，本测试不重复哈希比对）。
//
// 框架纪律（DEC-007）：单元测试一律 Catch2 v3。本用例不依赖 Executor，使用
// Catch2WithMain 变体（tests/CMakeLists.txt 的 aki_add_unit_test 接入）。
#include <sqlite3.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <string>

#ifndef AKI_EXPECTED_SQLITE_VERSION
#error "AKI_EXPECTED_SQLITE_VERSION must be defined from third_party/dependencies.lock.json"
#endif

TEST_CASE("vendored sqlite matches the lock file version (header/lib/config triple)",
    "[unit][persistence]") {
    const std::string expected = AKI_EXPECTED_SQLITE_VERSION;
    const std::string header_version = SQLITE_VERSION;
    const std::string libversion = sqlite3_libversion();
    const std::string sourceid = sqlite3_sourceid();
    const std::string header_source_id = SQLITE_SOURCE_ID;

    // 三方版本逐字输出：保持 M2-02 验证记录引用的证据面逐字可复现。
    std::printf("expected (lock):  %s\n", expected.c_str());
    std::printf("SQLITE_VERSION:   %s\n", header_version.c_str());
    std::printf("libversion:       %s\n", libversion.c_str());
    std::printf("sqlite3_sourceid: %s\n", sourceid.c_str());

    REQUIRE(header_version == expected);
    REQUIRE(libversion == expected);
    // header/lib 配对：运行时 sourceid 必须来自编译所用的同一份 amalgamation。
    REQUIRE(sourceid == header_source_id);
}
