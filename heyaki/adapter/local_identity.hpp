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
// 议题（M5）。password_verifier 自 M5-04 起为 kAkiPairingPassword 的真实
// argon2id verifier（DEC-016——M3-03 占位串为假编码、验不了任何口令，已
// 退役；存量 profile 处置见 DEC-016「影响与风险」：删除 db/profile.sqlite
// 重建，不静默迁移）。pairing 默认授予 scope 取 DEC-006 冻结常量
// message.send。
#pragma once

#include "device/device/device_types.hpp"

#include <heyaki/identity.hpp>
#include <heyaki/password.hpp>
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

// DEC-016 冻结常量：MVP 配对授权口令（≥8 个 Unicode 标量，满足
// PasswordSecurityPolicy.minimum_unicode_scalars）。目标端以其验证 pair_peer
// 提交（verifier 同批真实化）；在 heyaki/adapter→DeviceManager 内部传递，
// 不进 SPI/UiActions 签名。固定口令=公开弱口令（安全语义披露见 DEC-016），
// 移除条件：M5-07 设置面引入用户口令。
inline constexpr char kAkiPairingPassword[] = "aki-mvp-pairing-passphrase";

// 本地身份（std/aki 类型公开面）。
struct LocalIdentity {
    aki::device::DeviceId id;          // ::heyaki::to_string(device_id)——hy1_
                                       // 前缀 base32 规范串（SHA-256 摘要，
                                       // 非 hex；DEC-006 映射 1 实测修正）
    aki::device::PublicKey public_key; // Ed25519 公钥 32 字节
    std::string endpoint_id;           // endpoint_for(kAkiApplicationId) 规范 hex
    bool created = false;              // true = 本次启动新建；false = 既有加载
};

// 持有打开的本地 profile（M3-04：Node/Runtime 装配需要常驻 store）。
// store() 供 heyaki/ 层内接线（heyaki/session）使用；组合根只见 identity()。
class LocalProfile {
public:
    // create-or-open + readiness 收敛（语义同 provision_local_identity）。
    [[nodiscard]] static LocalProfile open(const std::string& data_root) {
        namespace hh = ::heyaki;

        const auto profile_path =
            std::filesystem::path{data_root} / "db" / "profile.sqlite";
        const bool created = !std::filesystem::exists(profile_path);

        hh::ProfileOpenOptions options;
        options.secret_backend.prefer_os_backend = false;  // 确定性（见头注）
        auto profile_result = created
            ? hh::ProfileStore::create(profile_path, options)
            : hh::ProfileStore::open(profile_path, options);
        if (!profile_result) {
            const auto* error = profile_result.error_if();
            throw std::runtime_error(std::string("local identity: profile ")
                + (created ? "create" : "open") + " failed: "
                + std::string(hh::error_code_name(error->code())) + ": "
                + std::string(error->safe_detail()));
        }

        LocalProfile profile(std::move(*profile_result.value_if()), created);
        profile.converge_local_initialization();
        profile.refresh_identity();
        return profile;
    }

    LocalProfile(LocalProfile&&) noexcept = default;
    LocalProfile& operator=(LocalProfile&&) = delete;
    LocalProfile(const LocalProfile&) = delete;
    LocalProfile& operator=(const LocalProfile&) = delete;

    [[nodiscard]] const LocalIdentity& identity() const noexcept {
        return identity_;
    }

    // heyaki/ 层内接线使用（heyaki/session 的 Node 装配）；组合根不得调用
    // （RULE-10：heyaki 类型不出层）。
    [[nodiscard]] ::heyaki::ProfileStore& store() noexcept { return store_; }

private:
    LocalProfile(::heyaki::ProfileStore store, bool created)
        : store_(std::move(store)) {
        identity_.created = created;
    }

    void converge_local_initialization() {
        namespace hh = ::heyaki;
        auto readiness = store_.local_readiness(kAkiApplicationId);
        if (!readiness) {
            const auto* error = readiness.error_if();
            throw std::runtime_error(
                std::string("local identity: readiness failed: ")
                + std::string(hh::error_code_name(error->code())) + ": "
                + std::string(error->safe_detail()));
        }
        if (readiness.value_if()->ready()) {
            return;
        }
        // DEC-016：真实 argon2id verifier（kAkiPairingPassword 的摘要）——
        // 仅 created 分支承担创建耗时（m=64MiB/t=2，启动恢复段主线程同步，
        // §11.1 ②）。存量 profile（占位 verifier）不在此触达：处置见头注。
        auto verifier = hh::create_password_verifier(
            kAkiPairingPassword, hh::PasswordHashParameters{});
        if (!verifier) {
            const auto* error = verifier.error_if();
            throw std::runtime_error(
                std::string("local identity: create_password_verifier "
                            "failed: ")
                + std::string(hh::error_code_name(error->code())) + ": "
                + std::string(error->safe_detail()));
        }
        hh::PairingPolicy pairing;
        pairing.default_scopes = {"message.send"};  // DEC-006 冻结配对 scope
        hh::LocalProfileInitialization initialization{
            .application_id = kAkiApplicationId,
            .password_verifier = std::move(*verifier.value_if()),
            .password_generation = 1U,
            .pairing_policy = std::move(pairing),
            .lan = hh::LanConfiguration{}};
        auto initialized = store_.initialize_local(initialization);
        if (!initialized) {
            const auto* error = initialized.error_if();
            throw std::runtime_error(
                std::string("local identity: initialize_local failed: ")
                + std::string(hh::error_code_name(error->code())) + ": "
                + std::string(error->safe_detail()));
        }
    }

    void refresh_identity() {
        namespace hh = ::heyaki;
        const hh::IdentityPublicKey& key = store_.identity_public_key();
        auto derived = hh::derive_device_id(std::span<const std::byte>(key));
        if (!derived || !(*derived.value_if() == store_.device_id())) {
            throw std::runtime_error(
                "local identity: derive_device_id(public_key) does not match "
                "the profile device id (identity binding broken)");
        }
        identity_.id =
            aki::device::DeviceId{hh::to_string(store_.device_id())};
        identity_.public_key.bytes.clear();
        identity_.public_key.bytes.reserve(key.size());
        for (const std::byte byte : key) {
            identity_.public_key.bytes.push_back(
                std::to_integer<std::uint8_t>(byte));
        }
        auto endpoint = store_.endpoint_for(kAkiApplicationId);
        if (!endpoint) {
            const auto* error = endpoint.error_if();
            throw std::runtime_error(
                std::string("local identity: endpoint_for('")
                + kAkiApplicationId + "') failed: "
                + std::string(hh::error_code_name(error->code())) + ": "
                + std::string(error->safe_detail()));
        }
        identity_.endpoint_id = hh::to_string(*endpoint.value_if());
    }

    ::heyaki::ProfileStore store_;
    LocalIdentity identity_;
};

// 便捷形态（M3-03）：供给身份后即关闭 profile（无 Node 装配的场景）。
[[nodiscard]] inline LocalIdentity provision_local_identity(
    const std::string& data_root) {
    return LocalProfile::open(data_root).identity();
}

}  // namespace aki::heyaki
