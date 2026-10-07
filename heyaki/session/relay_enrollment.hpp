// Relay enrollment 包装（M7，DEC-028 决策 2/4；设计 §4 Relay 发现来源）。
//
// 上游 `enroll_relay_profile` 的 aki/std 公开面：一次性 bootstrap token
// 准入 + Ed25519 挑战签名的同步 WSS 交换（transport 超时上界 connect 5s +
// handshake 5s + close 2s，一次性有界），成功把 `RelayEnrollmentRecord`
// 持久化进 heyaki profile store；此后每次 `Node::create` 自动登录 relay
//（运行中的 Node 不感知——HEY-20261006-001，变更重启生效）。
//
// 证书信任语义（DEC-028 决策 2 修订，2026-10-06 实测定型）：relay_url 走
// 公网 CA 时 ca_file 留空（系统信任根 + 主机名校验）。自签部署时 ca_file
// 须为 **relay 服务端使用的证书文件本体**（上游 quickstart 的 relay.crt
// 即是）：enrollment 交换用它做链 + 主机名校验；其 SHA-256 随记录持久化
// 为 `relay_pin`——登录起 TOFU pin 校验替代链校验（上游 profile 记录无
// ca_file 字段，pin 是唯一可持久化信任基）。提供「非服务端证书的私 CA」
// 时 pin 在登录期 wss_tls_pin_mismatch 可见失败，不静默。
//
// 调用纪律：本头文件的函数是阻塞网络操作，调用方（HostRuntime）必须在
// executor 任务内执行（EXEC-02/§9.1 三不——不得在 UI 点击回调或 compose
// 上下文直接调用）；bootstrap token 属凭据，随请求以非 const 引用传入，
// 本层在全部退出路径（校验拒绝/证书失败/交换失败/成功）原位擦除，调用方
// 其余自有副本（lambda 捕获等）各自擦除（DEC-018 同款纪律；原位擦除使
// 纪律对调用方可断言——M8-02 断言缝合点）。已知上游限制：WSS 交换内部经
// `RelayWssClient` 自建 owned runtime（HEY-20261006-002），Aki 无法注入
// 借用 executor——一次性有界（≤12s），由 HostRuntime 包在 executor 任务
// 内并保持结果可见。
//
// RULE-10：heyaki 类型封死在本层；公开面仅 aki/std 类型。
#pragma once

#include "heyaki/adapter/local_identity.hpp"

#include <heyaki/profile_store.hpp>
#include <heyaki/relay_enrollment_client.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace aki::heyaki {

// 注册请求（设置页向导字段的有界校验面）。
struct RelayEnrollRequest {
    std::string relay_url;   // 必须形如 wss://host[:port][/path]
    std::string tenant;      // 非空；与 relay 侧 token 所属租户一致
    // bootstrap token（凭据）：随请求以非 const 引用传入 enroll_relay_profile，
    // 由其在全部退出路径原位擦除（成功/失败一致）。
    std::string bootstrap_token;
    // 自签部署时必填（CA 证书文件路径）；公网证书留空走系统信任根。
    std::optional<std::filesystem::path> ca_file;
};

// enrollment 记录的 aki 投影（设置页状态展示）。
struct RelayEnrollmentView {
    std::string relay_url;
    std::string tenant;
    std::uint64_t enrollment_generation = 0;
    bool auto_connect = true;
    bool revoked = false;
};

// 请求静态校验（无 IO）：URL 前缀/非空、tenant 非空、token 非空、
// ca_file 提供时文件存在。失败返回可展示原因（RULE-09 拒绝可见）。
[[nodiscard]] inline std::optional<std::string> validate_relay_enroll_request(
    const RelayEnrollRequest& request) {
    if (request.relay_url.rfind("wss://", 0) != 0
        || request.relay_url.size() <= std::strlen("wss://")) {
        return std::string("relay address must be a wss:// URL");
    }
    if (request.tenant.empty()) {
        return std::string("relay tenant must not be empty");
    }
    if (request.bootstrap_token.empty()) {
        return std::string("bootstrap token must not be empty");
    }
    if (request.ca_file.has_value()) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(*request.ca_file, ec)) {
            return std::string("CA certificate file not found: "
                + request.ca_file->string());
        }
    }
    return std::nullopt;
}

// 证书文件首张证书的 SHA-256 pin（32 字节，上游 relay_pin 语义 = 服务端
// 呈现证书的 DER 哈希）。不做 CA/叶形态区分——上游 quickstart 的
// `openssl req -x509` 产物本就是 CA:TRUE 自签证书且直接用作服务端证书；
// 若用户提供的是「非服务端证书的私 CA」，pin 在登录期以
// wss_tls_pin_mismatch 可见失败（不静默）。读取/解析失败同步拒绝。
[[nodiscard]] inline std::optional<std::vector<std::byte>>
relay_certificate_pin(const std::filesystem::path& file, std::string& error) {
    BIO* bio = BIO_new_file(file.string().c_str(), "rb");
    if (bio == nullptr) {
        error = "certificate file cannot be opened: " + file.string();
        return std::nullopt;
    }
    X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (certificate == nullptr) {
        error = "certificate file is not valid PEM: " + file.string();
        return std::nullopt;
    }
    unsigned char* der = nullptr;
    const int der_size = i2d_X509(certificate, &der);
    X509_free(certificate);
    if (der_size <= 0 || der == nullptr) {
        OPENSSL_free(der);
        error = "certificate cannot be encoded";
        return std::nullopt;
    }
    std::vector<std::byte> pin(32);
    unsigned int digest_size = 0U;
    const int hashed = EVP_Digest(der, static_cast<std::size_t>(der_size),
        reinterpret_cast<unsigned char*>(pin.data()), &digest_size,
        EVP_sha256(), nullptr);
    OPENSSL_free(der);
    if (hashed != 1 || digest_size != pin.size()) {
        error = "certificate digest failed";
        return std::nullopt;
    }
    return pin;
}

// 执行 enrollment（阻塞，executor 任务内调用）。成功返回记录视图；失败
// 返回可展示错误（网络/token 拒绝/TLS/pin——上游 safe_detail 语义），
// profile 不落任何记录。request 以非 const 引用传入：bootstrap_token 在
// 全部退出路径（含上方校验/证书失败早退）原位擦除——凭据纪律对调用方
// 可断言（M8-02）。ca_file 提供时其 pin 随记录持久化（决策 2 修订——
// 登录期 TOFU 信任基）。
[[nodiscard]] inline std::optional<RelayEnrollmentView>
enroll_relay_profile(LocalProfile& profile, RelayEnrollRequest& request,
    std::string& error) {
    const auto scrub_token = [&request] {
        std::fill(request.bootstrap_token.begin(),
            request.bootstrap_token.end(), '\0');
        request.bootstrap_token.clear();
    };
    const auto invalid = validate_relay_enroll_request(request);
    if (invalid.has_value()) {
        error = *invalid;
        scrub_token();
        return std::nullopt;
    }
    std::optional<std::vector<std::byte>> pin;
    if (request.ca_file.has_value()) {
        pin = relay_certificate_pin(*request.ca_file, error);
        if (!pin.has_value()) {
            scrub_token();
            return std::nullopt;
        }
    }
    ::heyaki::RelayEnrollmentWssTransportConfig transport;
    transport.relay_url = request.relay_url;
    if (request.ca_file.has_value()) {
        // 交换期：链 + 主机名校验（transport 不带 pin——pin 只进记录，
        // 登录期生效）。
        transport.tls_ca_file = *request.ca_file;
    }
    ::heyaki::RelayEnrollmentClientConfig config;
    config.profile = &profile.store();
    config.application_id = kAkiApplicationId;
    config.relay_url = request.relay_url;
    config.tenant = request.tenant;
    config.relay_pin = pin;  // 随 RelayEnrollmentRecord 持久化（TOFU 基）
    config.wss_transport = transport;
    config.exchange = ::heyaki::make_relay_enrollment_wss_exchange(transport);
    const std::uint64_t now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    auto enrolled = ::heyaki::enroll_relay_profile(
        config, request.bootstrap_token, now_ms);
    scrub_token();
    if (!enrolled) {
        error = std::string(enrolled.error_if()->safe_detail());
        return std::nullopt;
    }
    RelayEnrollmentView view;
    view.relay_url = enrolled.value_if()->relay_url;
    view.tenant = enrolled.value_if()->tenant;
    view.enrollment_generation =
        enrolled.value_if()->enrollment_generation;
    view.auto_connect = true;
    view.revoked = false;
    return view;
}

// 移除注册（决策 4）：标记 revoked——Node 下次启动不再自动连接该 relay；
// 运行中的连接同样重启生效。无该 URL 记录时 false（无操作可见）。
[[nodiscard]] inline bool revoke_relay_enrollment(LocalProfile& profile,
    const std::string& relay_url, std::string& error) {
    auto existing = profile.store().relay_enrollment(relay_url);
    if (!existing) {
        error = std::string(existing.error_if()->safe_detail());
        return false;
    }
    if (!existing.value_if()->has_value()) {
        error = "no enrollment record for " + relay_url;
        return false;
    }
    const auto& record = **existing.value_if();
    auto marked = profile.store().mark_relay_revoked(
        relay_url, record.enrollment_generation);
    if (!marked) {
        error = std::string(marked.error_if()->safe_detail());
        return false;
    }
    return true;
}

// profile 中全部 enrollment 记录的投影（首条 auto_connect && !revoked 为
// Node 实际使用的记录——上游 initialize_relay 语义；读取失败返回空集，
// 状态展示按「未注册」处理，RULE-09 拒绝面经调用方日志可见）。
[[nodiscard]] inline std::vector<RelayEnrollmentView>
relay_enrollment_views(LocalProfile& profile) {
    std::vector<RelayEnrollmentView> views;
    auto records = profile.store().relay_enrollments();
    if (!records) {
        return views;
    }
    for (const auto& record : *records.value_if()) {
        RelayEnrollmentView view;
        view.relay_url = record.relay_url;
        view.tenant = record.tenant;
        view.enrollment_generation = record.enrollment_generation;
        view.auto_connect = record.auto_connect;
        view.revoked = record.revoked;
        views.push_back(std::move(view));
    }
    return views;
}

}  // namespace aki::heyaki
