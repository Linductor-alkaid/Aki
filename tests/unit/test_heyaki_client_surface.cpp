// M3-01：heyaki::client 消费边界锁定用例（DEC-006 构建接入；RULE-01/RULE-10）。
//
// 本目标是 Aki 构建图中唯一显式链接 heyaki::client 的编译单元（单图接入，
// DEC-006；Aki 产品目标自 M3-03 起才消费真实 heyaki，其余目标一律不链），
// 承担三类证明：
//   1. 契约基线可消费：pinned v1.0.1-38 公开头文件可包含、公开类型可构造；
//      wire 协议版本 {1,3} 与 DEC-006 冻结一致（静态与运行期双断言）。
//   2. 构建开关冻结（DEC-006：HEYAKI_BUILD_APPS=OFF）：build_info() 的
//      feature 位 client 在编、relay/tui 不在编。
//   3. 双 SQLite 静态共存（DEC-006 影响节）：同一二进制内 aki vendored
//      sqlite 3.53.4（aki_persistence PRIVATE）与 heyaki::sqlite 3.50.4
//      （heyaki_profile PRIVATE）——运行期经 aki 侧 sqlite3_libversion 校验
//      有效版本与锁文件一致；最终二进制仅一份 sqlite3 符号由 dumpbin/nm
//      离线证明（M3-01 验证记录）。
// RULE-10：Aki 公开头不含 heyaki 类型——结构性锁定（其余 Aki 目标不链
// heyaki::client、无 heyaki include 路径，公开面泄漏即编译失败）+ 仓库 grep
// 证据（M3-01 验证记录）；本文件属 heyaki/ 接线层消费点，允许包含 heyaki
// 公开头。
#include "heyaki/identity.hpp"
#include "heyaki/ids.hpp"
#include "heyaki/message.hpp"
#include "heyaki/node.hpp"
#include "heyaki/profile_store.hpp"
#include "heyaki/protocol.hpp"
#include "heyaki/runtime.hpp"
#include "heyaki/version.hpp"

#include "persistence/database/database.hpp"
#include "persistence/migration/migration.hpp"
#include "persistence/migration/schema_v1.hpp"

#include <cstdio>
#include <string>
#include <vector>

#ifndef AKI_EXPECTED_SQLITE_VERSION
#define AKI_EXPECTED_SQLITE_VERSION "unknown"
#endif

int main() {
    // ---- 契约基线：wire 协议 {1,3}（DEC-006 冻结常量）----
    static_assert(heyaki::current_protocol_version.major == 1U,
        "DEC-006 freezes wire protocol major 1");
    static_assert(heyaki::current_protocol_version.minor == 3U,
        "DEC-006 freezes wire protocol minor 3");
    const heyaki::BuildInfo info = heyaki::build_info();
    std::printf("heyaki %s @ %s, wire {%u,%u}\n", std::string(info.version).c_str(),
        std::string(info.commit).c_str(), info.protocol_major,
        info.protocol_minor);
    if (info.protocol_major != 1U || info.protocol_minor != 3U) {
        std::puts("[FAIL] runtime wire protocol is not the DEC-006 frozen {1,3}");
        return 1;
    }

    // ---- 构建开关：HEYAKI_BUILD_APPS=OFF（client 在编；relay/tui 不在编）----
    if (!info.features.has(heyaki::BuildFeature::client)
        || info.features.has(heyaki::BuildFeature::relay)
        || info.features.has(heyaki::BuildFeature::tui)) {
        std::puts("[FAIL] build features do not match HEYAKI_BUILD_APPS=OFF");
        return 1;
    }

    // ---- 公开类型可默认构造（编译级消费证明，不运行网络路径）----
    heyaki::NodeConfig node_config;
    (void)node_config;
    heyaki::RuntimeConfig runtime_config;
    (void)runtime_config;
    heyaki::ProfileOpenOptions profile_options;
    (void)profile_options;

    // ---- 同一二进制内的 aki vendored sqlite：迁移 + 有效版本断言 ----
    // 经 SQL 查询 sqlite_version()：无论链接器采纳哪一份 sqlite 副本，这里读到的
    // 就是该副本的版本（RULE-10：本 TU 不直接接触 sqlite3.h）。
    aki::persistence::Database db =
        aki::persistence::Database::open(":memory:");
    if (aki::persistence::Migrator(aki::persistence::schema_v1_steps())
            .bring_up_to_date(db)
        != 1) {
        std::puts("[FAIL] v1 migration should apply exactly one step");
        return 1;
    }
    aki::persistence::Statement version =
        db.prepare("SELECT sqlite_version();");
    if (!version.step()) {
        std::puts("[FAIL] sqlite_version() should return a row");
        return 1;
    }
    const std::string effective = version.column_text(0);
    std::printf("effective sqlite in this binary: %s (lock: %s)\n",
        effective.c_str(), AKI_EXPECTED_SQLITE_VERSION);
    if (effective != AKI_EXPECTED_SQLITE_VERSION) {
        std::puts("[FAIL] effective sqlite version does not match the lock file");
        return 1;
    }

    std::puts("[ok] heyaki client surface consumable; DEC-006 frozen constants hold");
    return 0;
}
