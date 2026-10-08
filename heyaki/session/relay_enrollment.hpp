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
// 上下文直接调用）；bootstrap token / 注册密码均属凭据，随请求以非 const
// 引用传入，本层在全部退出路径（校验拒绝/证书失败/交换失败/成功）原位
// 擦除，调用方其余自有副本（lambda 捕获等）各自擦除（DEC-018 同款纪律；
// 原位擦除使纪律对调用方可断言——M8-02 断言缝合点）。
//
// 借用 runtime（M8-04，HEY-20261006-002 收口）：交换经借用 Runtime 运行在
// 宿主 executor 上（transport.runtime_borrowed），进入其生命周期视图——
// 宿主 shutdown 可取消在途交换。**纪律（IVA 缺陷修复）**：heyaki Runtime
// 的创建/析构必须在非 worker 线程执行（与 executor owner 同款）——worker
// 内一次性创建/析构会在 teardown 自等待并破坏后续 Node 关闭。故以
// `RelayEnrollRuntime` 句柄承载：宿主主线程构造一次、任务内只读使用、
// 关闭序消费注册任务 future 后先于 executor 回收销毁。句柄为空时保留
// 上游 owned-runtime 回退（原行为，测试面沿用）。
//
// 密码准入（M8-04，DEC-028 决策 11 阶段 2）：enrollment_password 非空时走
// 上游 password 模式（Argon2id 挑战绑定证明）。租户语义：上游要求客户端
// 租户与 relay `enrollment_default_tenant` 严格相等，故本层对空租户填默认
// 值 "default"（relay 侧该键缺省即 "default"；机主自定义默认租户的部署经
// token 高级模式覆盖）。TOFU 首连：密码模式且未提供 ca_file 时交换以
// tls_verify_peer=false 首连，relay 回传的 leaf 证书 SHA-256 由上游客户端
// 自动持久化为记录 relay_pin（登录期 TOFU 校验基）——首连窗口的 MITM
// 可锚定自身证书，安全假设为「地址 + 密码的送达渠道可信」（决策 11 阶段
// 2 条款，设置页文案如实披露）；提供 ca_file 的严格部署仍走链校验。
//
// 已知上游限制：owned 回退的 WSS 交换内部经 `RelayWssClient` 自建
// owned runtime（HEY-20261006-002），一次性有界（≤12s）。
//
// RULE-10：heyaki 类型封死在本层；公开面仅 aki/std 类型。
#pragma once

#include "heyaki/adapter/local_identity.hpp"

#include <heyaki/profile_store.hpp>
#include <heyaki/relay_enrollment_client.hpp>

#include <kairo/executor.hpp>

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

// 密码准入常量（与上游 src/relay/relay_enrollment.hpp 的
// enrollment_password_max_bytes=256 及 relay 配置 enrollment_default_tenant
// 缺省值 "default" 对齐——决策 11 阶段 2 冻结依据）。
inline constexpr std::size_t kRelayEnrollmentPasswordMaxBytes = 256U;
inline constexpr std::string_view kRelayPasswordDefaultTenant = "default";

// 注册请求（设置页向导字段的有界校验面）。凭据二选一：bootstrap_token
//（token 模式，租户必填）或 enrollment_password（password 模式，空租户由
// 本层落默认值）；两者同时提供或同时为空均拒绝。
struct RelayEnrollRequest {
    std::string relay_url;   // 必须形如 wss://host[:port][/path]
    std::string tenant;      // token 模式必填；password 模式可空（落默认）
    // bootstrap token（凭据）：随请求以非 const 引用传入 enroll_relay_profile，
    // 由其在全部退出路径原位擦除（成功/失败一致）。
    std::string bootstrap_token;
    // 注册密码（凭据，M8-04）：同上原位擦除；TOFU 首连语义见头注。
    std::string enrollment_password;
    // 自签部署时必填（CA 证书文件路径）；公网证书留空走系统信任根；
    // 密码模式留空走 TOFU 首连（relay 回传指纹自动锚定）。
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

// 请求静态校验（无 IO）：URL 前缀、凭据二选一（token 模式租户必填；
// password 模式密码非空且 ≤256B、空租户落默认值）、ca_file 提供时文件
// 存在。失败返回可展示原因（RULE-09 拒绝可见）。
[[nodiscard]] inline std::optional<std::string> validate_relay_enroll_request(
    RelayEnrollRequest& request) {
    if (request.relay_url.rfind("wss://", 0) != 0
        || request.relay_url.size() <= std::strlen("wss://")) {
        return std::string("relay address must be a wss:// URL");
    }
    const bool has_token = !request.bootstrap_token.empty();
    const bool has_password = !request.enrollment_password.empty();
    if (has_token == has_password) {
        return std::string(
            "exactly one of bootstrap token or enrollment password is"
            " required");
    }
    if (has_password) {
        if (request.enrollment_password.size()
            > kRelayEnrollmentPasswordMaxBytes) {
            return std::string("enrollment password must be at most "
                + std::to_string(kRelayEnrollmentPasswordMaxBytes) + " bytes");
        }
        // password 模式：客户端租户须与 relay enrollment_default_tenant
        // 严格相等（上游 enrollment_tenant_unknown 语义）——空租户落默认。
        if (request.tenant.empty()) {
            request.tenant = std::string(kRelayPasswordDefaultTenant);
        }
    } else if (request.tenant.empty()) {
        return std::string("relay tenant must not be empty");
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

// 注册交换借用 Runtime 句柄（主线程构造/析构，任务内只读；见头注纪律）。
class RelayEnrollRuntime {
public:
    explicit RelayEnrollRuntime(kairo::Executor& executor) {
        ::heyaki::RuntimeConfig config;
        // 同一 executor 上多个借用 Runtime 的 blocking worker 名须互异
        //（NodeSession 缺省即 "heyaki-asio"，重名注册 asio_worker_start_
        // failed——IVA 缺陷③修复）。
        config.worker_name = "aki-relay-enroll";
        auto runtime = ::heyaki::Runtime::create_borrowed(executor, config);
        if (runtime) {
            runtime_.emplace(std::move(*runtime.value_if()));
        }
    }
    RelayEnrollRuntime(RelayEnrollRuntime&&) = default;
    RelayEnrollRuntime& operator=(RelayEnrollRuntime&&) = default;
    ~RelayEnrollRuntime() = default;  // 仅宿主主线程析构（非 worker）。
    [[nodiscard]] bool valid() const noexcept { return runtime_.has_value(); }
    [[nodiscard]] ::heyaki::Runtime* get() noexcept {
        return runtime_.has_value() ? &*runtime_ : nullptr;
    }

private:
    std::optional<::heyaki::Runtime> runtime_;
};

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
// 返回可展示错误（网络/凭据拒绝/TLS/pin——上游 safe_detail 语义），
// profile 不落任何记录。request 以非 const 引用传入：bootstrap_token 与
// enrollment_password 在全部退出路径（含上方校验/证书失败早退）原位擦除
// ——凭据纪律对调用方可断言（M8-02）。凭据二选一：token 走既有链校验；
// password 走上游 password 模式（无 ca_file 时 TOFU 首连）。runtime_handle
// 非空且有效时交换借用宿主 Runtime 运行（HEY-20261006-002 收口；句柄
// 须由主线程构造，见 RelayEnrollRuntime 纪律）；为空保留 owned 回退。
// ca_file 提供时其 pin 随记录持久化；密码模式下 relay 回传的 leaf
// SHA-256（enrolled result）由本层读回记录并以 UPSERT 回写为 relay_pin
//（上游记录写入只取 config.relay_pin，返回值指纹不落库——IVA 缺陷修复，
// 无此 pin 自签 relay 的后续登录必败 TLS）。
[[nodiscard]] inline std::optional<RelayEnrollmentView>
enroll_relay_profile(LocalProfile& profile, RelayEnrollRequest& request,
    std::string& error, RelayEnrollRuntime* runtime_handle = nullptr) {
    const auto scrub_secrets = [&request] {
        std::fill(request.bootstrap_token.begin(),
            request.bootstrap_token.end(), '\0');
        request.bootstrap_token.clear();
        std::fill(request.enrollment_password.begin(),
            request.enrollment_password.end(), '\0');
        request.enrollment_password.clear();
    };
    const auto invalid = validate_relay_enroll_request(request);
    if (invalid.has_value()) {
        error = *invalid;
        scrub_secrets();
        return std::nullopt;
    }
    const bool password_mode = !request.enrollment_password.empty();
    std::optional<std::vector<std::byte>> pin;
    if (request.ca_file.has_value()) {
        pin = relay_certificate_pin(*request.ca_file, error);
        if (!pin.has_value()) {
            scrub_secrets();
            return std::nullopt;
        }
    }
    ::heyaki::RelayEnrollmentWssTransportConfig transport;
    transport.relay_url = request.relay_url;
    if (request.ca_file.has_value()) {
        // 交换期：链 + 主机名校验（transport 不带 pin——pin 只进记录，
        // 登录期生效）。
        transport.tls_ca_file = *request.ca_file;
    } else if (password_mode) {
        // TOFU 首连（决策 11 阶段 2）：无 ca_file 的密码模式交换不校验
        // 链——信任基为密码送达渠道；relay 回传指纹随后锚定记录 pin。
        transport.tls_verify_peer = false;
    }
    // 借用 Runtime（主线程构造的句柄；交换期存续——头文件契约）。
    ::heyaki::Runtime* borrowed_runtime =
        runtime_handle != nullptr ? runtime_handle->get() : nullptr;
    if (runtime_handle != nullptr && borrowed_runtime == nullptr) {
        error = "relay enrollment runtime unavailable";
        scrub_secrets();
        return std::nullopt;
    }
    if (borrowed_runtime != nullptr) {
        transport.runtime_borrowed = borrowed_runtime;
    }
    ::heyaki::RelayEnrollmentClientConfig config;
    config.profile = &profile.store();
    config.application_id = kAkiApplicationId;
    config.relay_url = request.relay_url;
    config.tenant = request.tenant;
    config.relay_pin = pin;  // 随 RelayEnrollmentRecord 持久化（TOFU 基）
    config.wss_transport = transport;
    config.credential_exchange =
        ::heyaki::make_relay_enrollment_wss_credential_exchange(transport);
    ::heyaki::RelayEnrollmentCredential credential;
    credential.kind = password_mode
        ? ::heyaki::RelayEnrollmentCredential::Kind::password
        : ::heyaki::RelayEnrollmentCredential::Kind::token;
    credential.secret = password_mode ? request.enrollment_password
                                      : request.bootstrap_token;
    const std::uint64_t now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    auto enrolled = ::heyaki::enroll_relay_profile_with_credential(
        config, credential, now_ms);
    credential.secret.assign(credential.secret.size(), '\0');
    credential.secret.clear();
    scrub_secrets();
    if (!enrolled) {
        error = std::string(enrolled.error_if()->safe_detail());
        return std::nullopt;
    }
    // TOFU pin 回写（IVA 缺陷修复）：密码模式 relay 回传 leaf 指纹只存在于
    // 返回值——读回记录 UPSERT 同代回写（put 以 relay_url 为键原位更新，
    // generation 不变）；失败如实可见（无 pin 的自签记录登录必败，宁可使
    // 本次注册报错，由用户重试覆盖同 url 记录）。
    if (enrolled.value_if()->relay_certificate_sha256.has_value()
        && !pin.has_value()) {
        auto existing = profile.store().relay_enrollment(
            enrolled.value_if()->relay_url);
        if (!existing || !existing.value_if()->has_value()) {
            error = "relay enrollment record missing after success";
            return std::nullopt;
        }
        auto record = **existing.value_if();
        record.relay_pin = *enrolled.value_if()->relay_certificate_sha256;
        auto persisted = profile.store().put_relay_enrollment(record);
        if (!persisted) {
            error = std::string("relay pin persist failed: ")
                + std::string(persisted.error_if()->safe_detail());
            return std::nullopt;
        }
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
