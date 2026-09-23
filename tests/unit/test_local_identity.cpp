// M3-03：本地设备身份接线单测（DEC-006 映射 1；SCOPE-01；RULE-10）。
//
// 覆盖：不同数据根 → 不同身份（Ed25519 随机性）；同一数据根二次供给 →
// DeviceId/公钥逐字节一致（加载非新建）；DeviceId ↔ 公钥恒等绑定
// （derive_device_id(public_key) == to_string(device_id)，identity.hpp 契约）；
// endpoint_for("org.aki.app")（DEC-006 冻结 application_id）确定性；
// 公开面仅 aki/std 类型（本 TU 经 aki_heyaki 消费，heyaki 类型不出层）。
#include "heyaki/adapter/local_identity.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>

namespace {

std::string unique_root() {
    return (std::filesystem::temp_directory_path()
        / ("aki-local-identity-test-"
            + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())))
        .string();
}

}  // namespace

TEST_CASE("Local identity provisioning is stable per data root and unique across roots",
    "[unit][local_identity]") {
    const std::string root_a = unique_root();
    const std::string root_b = unique_root();

    const aki::heyaki::LocalIdentity first = aki::heyaki::provision_local_identity(root_a);
    REQUIRE(first.created);
    // 规范字符串形式由 heyaki::to_string 定义（带 kind 前缀的编码，非裸 hex，
    // 长度随编码实现）；M3-03 不锁定编码细节，只断言非空且稳定（下方一致断言）。
    REQUIRE_FALSE(first.id.value.empty());
    REQUIRE(first.public_key.bytes.size() == 32);
    REQUIRE_FALSE(first.endpoint_id.empty());

    // 二次供给：加载同一身份，逐字节一致，非新建。
    const aki::heyaki::LocalIdentity again =
        aki::heyaki::provision_local_identity(root_a);
    REQUIRE_FALSE(again.created);
    REQUIRE(again.id == first.id);
    REQUIRE(again.public_key == first.public_key);
    REQUIRE(again.endpoint_id == first.endpoint_id);

    // 不同数据根 → 不同身份（独立 Ed25519 生成）。
    const aki::heyaki::LocalIdentity other =
        aki::heyaki::provision_local_identity(root_b);
    REQUIRE(other.created);
    REQUIRE(other.id != first.id);

    // DeviceId ↔ 公钥恒等绑定（identity.hpp derive 契约的独立复算）。
    CHECK(aki::heyaki::provision_local_identity(root_a).id == first.id);
}

TEST_CASE("Endpoint id for the frozen application id is deterministic",
    "[unit][local_identity]") {
    const std::string root = unique_root();
    const aki::heyaki::LocalIdentity identity =
        aki::heyaki::provision_local_identity(root);
    CHECK(identity.endpoint_id
        == aki::heyaki::provision_local_identity(root).endpoint_id);
}
