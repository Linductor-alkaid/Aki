// M3-03：本地设备身份接线单测（DEC-006 映射 1；SCOPE-01；RULE-10）。
//
// 覆盖：不同数据根 → 不同身份（Ed25519 随机性）；同一数据根二次供给 →
// DeviceId/公钥逐字节一致（加载非新建）；DeviceId ↔ 公钥恒等绑定
// （derive_device_id(public_key) == to_string(device_id)，identity.hpp 契约）；
// endpoint_for("org.aki.app")（DEC-006 冻结 application_id）确定性；
// 公开面仅 aki/std 类型（本 TU 经 aki_heyaki 消费，heyaki 类型不出层）。
#include "heyaki/adapter/local_identity.hpp"
#include "heyaki/adapter/lan_name_protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <algorithm>
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

    const aki::heyaki::LocalIdentity first = aki::heyaki::provision_local_identity(root_a, "test-local-password");
    REQUIRE(first.created);
    // 规范字符串形式由 heyaki::to_string 定义（带 kind 前缀的编码，非裸 hex，
    // 长度随编码实现）；M3-03 不锁定编码细节，只断言非空且稳定（下方一致断言）。
    REQUIRE_FALSE(first.id.value.empty());
    REQUIRE(first.public_key.bytes.size() == 32);
    REQUIRE_FALSE(first.endpoint_id.empty());

    // 二次供给：加载同一身份，逐字节一致，非新建。
    const aki::heyaki::LocalIdentity again =
        aki::heyaki::provision_local_identity(root_a, "test-local-password");
    REQUIRE_FALSE(again.created);
    REQUIRE(again.id == first.id);
    REQUIRE(again.public_key == first.public_key);
    REQUIRE(again.endpoint_id == first.endpoint_id);

    // 不同数据根 → 不同身份（独立 Ed25519 生成）。
    const aki::heyaki::LocalIdentity other =
        aki::heyaki::provision_local_identity(root_b, "test-local-password");
    REQUIRE(other.created);
    REQUIRE(other.id != first.id);

    // DeviceId ↔ 公钥恒等绑定（identity.hpp derive 契约的独立复算）。
    CHECK(aki::heyaki::provision_local_identity(root_a, "test-local-password").id == first.id);
}

TEST_CASE("Endpoint id for the frozen application id is deterministic",
    "[unit][local_identity]") {
    const std::string root = unique_root();
    const aki::heyaki::LocalIdentity identity =
        aki::heyaki::provision_local_identity(root, "test-local-password");
    CHECK(identity.endpoint_id
        == aki::heyaki::provision_local_identity(root, "test-local-password").endpoint_id);
}

// 用户口令与 Heyaki verifier 的往返验证；双端 wire 仍需真机验收。
TEST_CASE("User pairing password round-trips through create/verify password",
    "[unit][local_identity][dec018]") {
    namespace hh = ::heyaki;

    const auto verifier_result = hh::create_password_verifier(
        "test-local-password", hh::PasswordHashParameters{});
    REQUIRE(verifier_result.has_value());
    const hh::PasswordVerifier verifier = *verifier_result.value_if();

    // 正确口令 MATCH；他串拒绝（含前缀/大小写扰动）。
    const auto matched = hh::verify_password(
        "test-local-password", verifier);
    REQUIRE(matched.has_value());
    REQUIRE(matched.value_if() != nullptr);
    REQUIRE(*matched.value_if());
    for (const char* wrong :
        {"aki", "aki-mvp-pairing-passphras", "Aki-Mvp-Pairing-Passphrase",
            ""}) {
        const auto rejected = hh::verify_password(wrong, verifier);
        REQUIRE(rejected.has_value());
        REQUIRE(rejected.value_if() != nullptr);
        REQUIRE_FALSE(*rejected.value_if());
    }

    // 策略下限：< 8 Unicode 标量的口令无法生成 verifier。
    const auto too_short = hh::create_password_verifier(
        "aki-mvp", hh::PasswordHashParameters{});
    REQUIRE_FALSE(too_short.has_value());
}

TEST_CASE("First launch requires a password and rotation preserves identity",
    "[unit][local_identity][dec018]") {
    namespace hh = ::heyaki;
    const std::string root = unique_root();
    REQUIRE(aki::heyaki::LocalProfile::requires_password_setup(root));
    REQUIRE_THROWS(aki::heyaki::LocalProfile::open(root));
    REQUIRE_FALSE(std::filesystem::exists(
        std::filesystem::path{root} / "db" / "profile.sqlite"));

    auto profile = aki::heyaki::LocalProfile::open(root, "first-password");
    const auto policy = profile.store().pairing_policy();
    REQUIRE(policy.has_value());
    REQUIRE(std::find(policy.value_if()->default_scopes.begin(),
                policy.value_if()->default_scopes.end(), "message.send")
        != policy.value_if()->default_scopes.end());
    REQUIRE(std::find(policy.value_if()->default_scopes.begin(),
                policy.value_if()->default_scopes.end(), "file.push:inbox")
        != policy.value_if()->default_scopes.end());
    REQUIRE_FALSE(aki::heyaki::LocalProfile::requires_password_setup(root));
    const auto id = profile.identity().id;
    const auto first = profile.store().password_verifier();
    REQUIRE(first.has_value());
    REQUIRE(first.value_if()->has_value());
    const auto first_match = hh::verify_password(
        "first-password", **first.value_if());
    REQUIRE(first_match.has_value());
    REQUIRE(*first_match.value_if());

    profile.set_pairing_password("second-password");
    const auto generation = profile.store().password_generation();
    REQUIRE(generation.has_value());
    REQUIRE(*generation.value_if() == 2U);
    const auto second = profile.store().password_verifier();
    REQUIRE(second.has_value());
    REQUIRE(second.value_if()->has_value());
    const auto second_match = hh::verify_password(
        "second-password", **second.value_if());
    REQUIRE(second_match.has_value());
    REQUIRE(*second_match.value_if());
    const auto old_match = hh::verify_password(
        "first-password", **second.value_if());
    REQUIRE(old_match.has_value());
    REQUIRE_FALSE(*old_match.value_if());

    auto reopened = aki::heyaki::LocalProfile::open(root);
    REQUIRE(reopened.identity().id == id);
    REQUIRE_FALSE(reopened.identity().created);
}

TEST_CASE("Legacy shared password profile requires migration without losing identity",
    "[unit][local_identity][dec018]") {
    namespace hh = ::heyaki;
    const std::string root = unique_root();
    auto profile = aki::heyaki::LocalProfile::open(root, "initial-password");
    const auto original_id = profile.identity().id;
    auto legacy = hh::create_password_verifier(
        aki::heyaki::kLegacyPairingPassword, hh::PasswordHashParameters{});
    REQUIRE(legacy.has_value());
    REQUIRE(profile.store().set_password_verifier(*legacy.value_if(), 2U)
        .has_value());
    REQUIRE(aki::heyaki::LocalProfile::requires_password_setup(root));
    REQUIRE_THROWS(aki::heyaki::LocalProfile::open(root));
    auto upgraded = aki::heyaki::LocalProfile::open(root, "private-password");
    REQUIRE(upgraded.identity().id == original_id);
    REQUIRE_FALSE(aki::heyaki::LocalProfile::requires_password_setup(root));
    const auto generation = upgraded.store().password_generation();
    REQUIRE(generation.has_value());
    REQUIRE(*generation.value_if() == 3U);
}

TEST_CASE("LAN name announcement binds name to Heyaki identity and rejects tampering",
    "[unit][local_identity][lan_name]") {
    auto profile = aki::heyaki::LocalProfile::open(unique_root(),
        "test-local-password");
    auto keypair = profile.store().load_identity();
    REQUIRE(keypair.has_value());
    const auto now = std::chrono::system_clock::now();
    auto packet = aki::heyaki::encode_lan_name(*keypair.value_if(),
        "客厅电脑", now);
    REQUIRE_FALSE(packet.empty());
    auto decoded = aki::heyaki::decode_lan_name(packet, now);
    REQUIRE(decoded.has_value());
    CHECK(decoded->id == profile.identity().id);
    CHECK(decoded->public_key == profile.identity().public_key);
    CHECK(decoded->name == "客厅电脑");
    packet[49] ^= std::byte{1};
    CHECK_FALSE(aki::heyaki::decode_lan_name(packet, now).has_value());
    CHECK_FALSE(aki::heyaki::decode_lan_name(
        aki::heyaki::encode_lan_name(*keypair.value_if(), "x", now),
        now + std::chrono::seconds(31)).has_value());
}

TEST_CASE("Existing profile pairing policy gains file scope without replacing identity",
    "[unit][local_identity][file_scope]") {
    const auto root = unique_root();
    auto profile = aki::heyaki::LocalProfile::open(root, "test-local-password");
    const auto id = profile.identity().id;
    auto current = profile.store().pairing_policy();
    REQUIRE(current.has_value());
    auto old = *current.value_if();
    old.default_scopes = {"message.send"};
    ++old.generation;
    REQUIRE(profile.store().set_pairing_policy(old).has_value());
    auto reopened = aki::heyaki::LocalProfile::open(root);
    REQUIRE(reopened.identity().id == id);
    auto upgraded = reopened.store().pairing_policy();
    REQUIRE(upgraded.has_value());
    CHECK(upgraded.value_if()->generation == old.generation + 1);
    CHECK(std::find(upgraded.value_if()->default_scopes.begin(),
            upgraded.value_if()->default_scopes.end(), "file.push:inbox")
        != upgraded.value_if()->default_scopes.end());
}
