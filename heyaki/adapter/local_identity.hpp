// 本地设备身份接线（DEC-006 映射 1「身份/配置」；M3-03；SCOPE-01）。
//
// ProfileStore create-or-open + 首次 initialize_local + endpoint_for(DEC-006
// 冻结 application_id)。首次启动创建 heyaki 长期身份（Ed25519，profile 落盘），
// 后续启动 open 加载同一身份（DeviceId/公钥逐字节一致）。DeviceId 恒等绑定
// heyaki 身份体系：本接线同时断言 derive_device_id(public_key) == profile
// device_id（identity.hpp 的公钥稳定绑定契约）。
//
// RULE-10：heyaki 类型封死在本层——公开面仅 aki 领域类型（DeviceId/PublicKey）
// 与 std 类型；调用方（组合根）不见 heyaki 类型。调用时序：启动恢复段主线程
// 同步执行（第 11.1 节 ② 模式，无新并发路径，RULE-07）。aki_heyaki 为
// INTERFACE 头文件库，本接线为单头文件 inline 实现（消费方链接 heyaki::client
// 提供 include 路径与符号，见根/tests CMake）。
//
// 宿主配置决定（M3-03，如实记录）：secret_backend.prefer_os_backend = false——
// 控制台宿主与自动化测试需要确定性（不依赖 OS 钥匙串），走 heyaki 加密文件
// 回退（allow_encrypted_file_fallback 默认 true）；OS 后端集成属后续设置面
// 议题（M5）。password_verifier 为占位 verifier（heyaki 自身测试同型）——
// 本地口令流程随 M5 设置面引入，DEC-006 未冻结口令处理。pairing 默认授予
// scope 取 DEC-006 冻结常量 message.send。
#pragma once

#include "device/device/device_types.hpp"

#include <heyaki/identity.hpp>
#include <heyaki/profile_store.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace aki::heyaki {

// DEC-006 冻结常量：application_id。
inline constexpr char kAkiApplicationId[] = "org.aki.app";

// 本地身份（std/aki 类型公开面）。
struct LocalIdentity {
    aki::device::DeviceId id;          // ::heyaki::to_string(device_id) 规范 hex
    aki::device::PublicKey public_key; // Ed25519 公钥 32 字节
    std::string endpoint_id;           // endpoint_for(kAkiApplicationId) 规范 hex
    bool created = false;              // true = 本次启动新建；false = 既有加载
};

// 在 <data_root>/db/profile.sqlite 创建或打开本地 profile 并补齐就绪项
// （身份/endpoint/口令 verifier/配对策略/LAN 配置）。失败抛 std::runtime_error
// （携带 heyaki error_code_name + safe_detail，不静默）——组合根按第 11.1 节 ②
// 干净退出。
[[nodiscard]] inline LocalIdentity provision_local_identity(
    const std::string& data_root) {
    namespace hh = ::heyaki;

    const auto profile_path =
        std::filesystem::path{data_root} / "db" / "profile.sqlite";
    const bool created = !std::filesystem::exists(profile_path);

    hh::ProfileOpenOptions options;
    options.secret_backend.prefer_os_backend = false;  // 确定性（见头注）
    auto profile_result = created ? hh::ProfileStore::create(profile_path, options)
                                  : hh::ProfileStore::open(profile_path, options);
    if (!profile_result) {
        const auto* error = profile_result.error_if();
        throw std::runtime_error(std::string("local identity: profile ")
            + (created ? "create" : "open") + " failed: "
            + std::string(hh::error_code_name(error->code())) + ": "
            + std::string(error->safe_detail()));
    }
    auto profile = std::move(*profile_result.value_if());

    // 首次启动补齐本地初始化；后续启动经 readiness 收敛（幂等）。
    auto readiness = profile.local_readiness(kAkiApplicationId);
    if (!readiness) {
        const auto* error = readiness.error_if();
        throw std::runtime_error(std::string("local identity: readiness failed: ")
            + std::string(hh::error_code_name(error->code())) + ": "
            + std::string(error->safe_detail()));
    }
    if (!readiness.value_if()->ready()) {
        hh::PasswordVerifier verifier{.format_version = 1U,
            .parameters = hh::PasswordHashParameters{},
            .encoded = "$argon2id$v=19$m=65536,t=2,p=1$aki$aki"};
        hh::PairingPolicy pairing;
        pairing.default_scopes = {"message.send"};  // DEC-006 冻结配对 scope
        hh::LocalProfileInitialization initialization{
            .application_id = kAkiApplicationId,
            .password_verifier = std::move(verifier),
            .password_generation = 1U,
            .pairing_policy = std::move(pairing),
            .lan = hh::LanConfiguration{}};
        auto initialized = profile.initialize_local(initialization);
        if (!initialized) {
            const auto* error = initialized.error_if();
            throw std::runtime_error(
                std::string("local identity: initialize_local failed: ")
                + std::string(hh::error_code_name(error->code())) + ": "
                + std::string(error->safe_detail()));
        }
    }

    LocalIdentity out;
    out.created = created;

    // DeviceId ↔ 公钥恒等绑定（DEC-006 映射 1；identity.hpp derive 契约）。
    const hh::IdentityPublicKey& key = profile.identity_public_key();
    auto derived = hh::derive_device_id(std::span<const std::byte>(key));
    if (!derived || !(*derived.value_if() == profile.device_id())) {
        throw std::runtime_error(
            "local identity: derive_device_id(public_key) does not match the "
            "profile device id (identity binding broken)");
    }

    out.id = aki::device::DeviceId{hh::to_string(profile.device_id())};
    out.public_key.bytes.reserve(key.size());
    for (const std::byte byte : key) {
        out.public_key.bytes.push_back(std::to_integer<std::uint8_t>(byte));
    }

    auto endpoint = profile.endpoint_for(kAkiApplicationId);
    if (!endpoint) {
        const auto* error = endpoint.error_if();
        throw std::runtime_error(std::string("local identity: endpoint_for('") + kAkiApplicationId
            + "') failed: " + std::string(hh::error_code_name(error->code()))
            + ": " + std::string(error->safe_detail()));
    }
    out.endpoint_id = hh::to_string(*endpoint.value_if());
    return out;
}

}  // namespace aki::heyaki
