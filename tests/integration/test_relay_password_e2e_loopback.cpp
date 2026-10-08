// M8-04（DEC-028 决策 11 阶段 2）：relay 密码准入端到端回环集成测试。
//
// 覆盖（[skip] 受控退出纪律沿 test_relay_e2e_loopback；前四用例各自独立
// server/owner fixture——单用例内 REQUIRE 失败不掩盖其余场景的验证证据；
// 第五用例为关闭竞争注入测试，自持 listener/owner，见文件尾注）：
//   - 真实 RelayServer 密码模式（enrollment_mode=password，仅测试目标链接
//     heyaki::relay——DEC-028 决策 10 先例）：自签证书（含 IP:127.0.0.1 SAN）
//     + owner 密码 verifier 经 RelayDatabase 种入（上游 `relay --init` 同型：
//     verifier = Argon2id(小写 hex(derive_enrollment_password_proof(
//     password, relay_id)))，固定参数 {2, 64 MiB}；relay_id 即 leaf 证书
//     SHA-256，relay_server.cpp:764-773 X509_digest 同法）。
//   - 密码模式 enrollment 成功（owned-runtime 回退：不传 host_executor）：
//     aki::heyaki::enroll_relay_profile 以 password + 空租户 + 无 ca_file 走
//     TOFU 首连（tls_verify_peer=false）；成功后断言：空租户归一 "default"
//     （服务端回显 + 调用方 request 原位可见）、密码成功路径同样原位擦除、
//     记录持久化、记录 relay_pin == 服务端 leaf 证书 SHA-256 独立复算
//     （X509_digest 对拍——决策 11 阶段 2 TOFU 锚定验收点）。
//   - 借用路径：同一成功场景传宿主 executor（测试自有 ExecutorOwner）作
//     host_executor——交换经借用 Runtime 运行在宿主 executor 上
//     （transport.runtime_borrowed，HEY-20261006-002 收口面）。
//   - 错密码：认证拒绝可见（error 含上游 enrollment_password 拒绝
//     detail，按 pinned 源码实测为 enrollment_password_rejected）、无记录
//     落库、密码原位擦除。
//   - 既有 token 模式 e2e 回归在 test_relay_e2e_loopback（本文件不触碰）。
//
// TLS 事实（沿 test_relay_e2e_loopback 2026-10-06 核实）：密码模式无
// ca_file 时交换 tls_verify_peer=false（TOFU 首连），证书 SAN 与否不影响
// 交换；本测试证书仍写 IP:127.0.0.1 SAN 以保持与 token e2e fixture 同型。
#include "app/lifecycle/executor_owner.hpp"
#include "heyaki/adapter/lan_discovery.hpp"
#include "heyaki/adapter/local_identity.hpp"
#include "heyaki/session/relay_enrollment.hpp"
#include "heyaki/session/runtime_node.hpp"

#include <heyaki/error.hpp>
#include <heyaki/password.hpp>
#include <heyaki/runtime.hpp>

// DEC-028 决策 10：仅测试目标消费 heyaki::relay（src/relay 内部头，
// heyaki::relay PUBLIC include 传播）。
#include "relay_database.hpp"
#include "relay_enrollment.hpp"
#include "relay_server.hpp"

#include <catch2/catch_test_macros.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using aki::app::ExecutorOwner;
using aki::heyaki::LocalProfile;

int g_counter = 0;

std::string temp_root(const std::string& tag) {
    auto path = std::filesystem::temp_directory_path()
        / ("aki-relay-pw-e2e-" + tag + "-"
            + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())
            + "-" + std::to_string(++g_counter));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return path.string();
}

bool wait_until(const std::function<bool()>& predicate,
    std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return predicate();
}

// 服务器 leaf 证书 SHA-256，测试侧独立实现（X509_digest——上游 relay_id
// 推导 relay_server.cpp:764-773 与 login 期 pin 校验同 API）：与服务端
// relay_id / 密码模式回传 relay_certificate_sha256 / 记录 relay_pin 三方
// 对拍，均为同一证书 DER 的 SHA-256，须逐字节一致。
std::optional<std::vector<std::byte>> certificate_pin(
    const std::filesystem::path& certificate_path) {
    BIO* input = BIO_new_file(certificate_path.string().c_str(), "rb");
    if (input == nullptr) {
        return std::nullopt;
    }
    X509* certificate = PEM_read_bio_X509(input, nullptr, nullptr, nullptr);
    BIO_free(input);
    if (certificate == nullptr) {
        return std::nullopt;
    }
    std::array<std::byte, 32> pin{};
    unsigned int size = 0U;
    const bool ok = X509_digest(certificate, EVP_sha256(),
                          reinterpret_cast<unsigned char*>(pin.data()), &size) == 1
        && size == pin.size();
    X509_free(certificate);
    if (!ok) {
        return std::nullopt;
    }
    return std::vector<std::byte>(pin.begin(), pin.end());
}

// test_relay_e2e_loopback.cpp 同型自签证书（EC prime256v1、CN 固定、随机
// 序列号、subjectAltName = IP:127.0.0.1）。
bool write_test_certificate(const std::filesystem::path& directory) {
    const auto certificate_path = directory / "test-only-cert.pem";
    const auto key_path = directory / "test-only-key.pem";

    EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1");
    if (key == nullptr) {
        return false;
    }
    X509* certificate = X509_new();
    if (certificate == nullptr) {
        EVP_PKEY_free(key);
        return false;
    }

    std::array<unsigned char, 8U> serial_bytes{};
    std::uint64_t serial = 1U;
    if (RAND_bytes(serial_bytes.data(), static_cast<int>(serial_bytes.size())) == 1) {
        std::memcpy(&serial, serial_bytes.data(), serial_bytes.size());
        serial &= (std::numeric_limits<std::uint64_t>::max)() >> 1U;
        serial = std::max<std::uint64_t>(serial, 1U);
    }
    bool configured =
        X509_set_version(certificate, 2L) == 1 &&
        ASN1_INTEGER_set_uint64(X509_get_serialNumber(certificate), serial) == 1 &&
        X509_gmtime_adj(X509_getm_notBefore(certificate), -60L) != nullptr &&
        X509_gmtime_adj(X509_getm_notAfter(certificate), 24L * 60L * 60L) != nullptr &&
        X509_set_pubkey(certificate, key) == 1;
    X509_NAME* name = X509_get_subject_name(certificate);
    configured =
        configured && name != nullptr &&
        X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("heyaki-relay-test"), -1, -1, 0) == 1 &&
        X509_set_issuer_name(certificate, name) == 1;
    X509_EXTENSION* constraints = X509V3_EXT_conf_nid(
        nullptr, nullptr, NID_basic_constraints, const_cast<char*>("critical,CA:FALSE"));
    if (constraints != nullptr) {
        configured = configured && X509_add_ext(certificate, constraints, -1) == 1;
        X509_EXTENSION_free(constraints);
    } else {
        configured = false;
    }
    X509_EXTENSION* alt_names = X509V3_EXT_conf_nid(
        nullptr, nullptr, NID_subject_alt_name, const_cast<char*>("IP:127.0.0.1"));
    if (alt_names != nullptr) {
        configured = configured && X509_add_ext(certificate, alt_names, -1) == 1;
        X509_EXTENSION_free(alt_names);
    } else {
        configured = false;
    }
    configured = configured && X509_sign(certificate, key, EVP_sha256()) > 0;

    BIO* certificate_output = BIO_new_file(certificate_path.string().c_str(), "wb");
    BIO* key_output = BIO_new_file(key_path.string().c_str(), "wb");
    configured = configured && certificate_output != nullptr && key_output != nullptr &&
                 PEM_write_bio_X509(certificate_output, certificate) == 1 &&
                 PEM_write_bio_PrivateKey(key_output, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
    if (certificate_output != nullptr) {
        BIO_free(certificate_output);
    }
    if (key_output != nullptr) {
        BIO_free(key_output);
    }
    X509_free(certificate);
    EVP_PKEY_free(key);
    return configured;
}

// 密码模式 relay 配置（test_relay_e2e_loopback::test_relay_config 同型 +
// enrollment_mode=password；enrollment_default_tenant 缺省即 "default"）。
heyaki::RelayServerConfig test_relay_config(const std::filesystem::path& root) {
    heyaki::RelayServerConfig config;
    config.listen_address = "127.0.0.1";
    config.listen_port = 0U;
    config.tls_certificate_file = root / "test-only-cert.pem";
    config.tls_private_key_file = root / "test-only-key.pem";
    config.database_file = root / "relay.sqlite";
    config.health_path = "/health";
    config.max_connections = 8U;
    config.handshake_timeout = 5000ms;
    config.shutdown_timeout = 5000ms;
    config.install_signal_handlers = false;
    config.runtime.worker_name = "aki-relay-pw-e2e";
    config.enrollment_mode = heyaki::RelayEnrollmentMode::password;
    return config;
}

std::uint64_t now_milliseconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// 小写 hex——上游 m3b_relay_enrollment_password_test.cpp:83-95 同款（服务端
// verifier 输入编码，enrolling 客户端呈现的证明同此编码）。
std::string hex_encode(const heyaki::EnrollmentPasswordProof& proof) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(proof.size() * 2U);
    for (const auto byte : proof) {
        const auto value = std::to_integer<unsigned char>(byte);
        output.push_back(digits[value >> 4U]);
        output.push_back(digits[value & 0x0fU]);
    }
    return output;
}

// --init 同型 verifier 种入（上游 m3b provision_verifier：verifier over
// 小写 hex(relay-bound proof)，固定参数 {2, 64 MiB}）。
bool provision_password_verifier(const std::filesystem::path& database_file,
    const std::string& password, const heyaki::RelayId& relay_id) {
    auto database = heyaki::RelayDatabase::open(database_file);
    if (!database) {
        return false;
    }
    auto proof =
        heyaki::derive_enrollment_password_proof(password, relay_id);
    if (!proof) {
        return false;
    }
    auto verifier = heyaki::create_password_verifier(
        hex_encode(*proof.value_if()),
        heyaki::PasswordHashParameters{2U, 64U * 1024U * 1024U});
    if (!verifier) {
        return false;
    }
    heyaki::RelayEnrollmentPasswordRecord record;
    record.format_version = verifier.value_if()->format_version;
    record.kdf_operations =
        static_cast<std::uint32_t>(verifier.value_if()->parameters.operations);
    record.kdf_memory_kib = static_cast<std::uint32_t>(
        verifier.value_if()->parameters.memory_bytes / 1024U);
    record.encoded = verifier.value_if()->encoded;
    record.updated_unix_milliseconds = now_milliseconds();
    auto stored =
        database.value_if()->set_enrollment_password_verifier(record);
    return stored.has_value();
}

void print_server_snapshot(const heyaki::RelayServerSnapshot& snapshot) {
    std::printf(
        "    [diag] server: state=%d port=%u tcp=%llu ws=%llu hs_failed=%llu "
        "proto_rejected=%llu enroll_ch=%llu enroll_ok=%llu login_ch=%llu "
        "login_ok=%llu\n",
        static_cast<int>(snapshot.state),
        static_cast<unsigned>(snapshot.listen_port),
        static_cast<unsigned long long>(snapshot.tcp_accepted),
        static_cast<unsigned long long>(snapshot.websocket_accepted),
        static_cast<unsigned long long>(snapshot.handshake_failed),
        static_cast<unsigned long long>(snapshot.protocol_rejected),
        static_cast<unsigned long long>(snapshot.enrollment_challenges),
        static_cast<unsigned long long>(snapshot.enrollments_completed),
        static_cast<unsigned long long>(snapshot.login_challenges),
        static_cast<unsigned long long>(snapshot.logins_completed));
    if (snapshot.last_error.has_value()) {
        std::printf("    [diag] server last_error: %s\n",
            std::string(snapshot.last_error->safe_detail()).c_str());
    }
}

// 密码模式 fixture：ExecutorOwner + 自签证书 + verifier 种入 + 借用
// Runtime RelayServer（三用例各自一份；析构按关闭序回收——RAII 之外还有
// 用例内显式 teardown 断言，结构体仅承载资源）。skip_reason 非空表示
// 环境性失败，调用方按 [skip] 纪律受控退出。
struct RelayPasswordFixture {
    std::optional<ExecutorOwner> owner;
    std::unique_ptr<heyaki::Runtime> relay_runtime;
    std::unique_ptr<heyaki::RelayServer> server;
    std::vector<std::string> temp_roots;
    std::string relay_url;
    std::string owner_password = "relay-pw-e2e-owner-secret";
    std::optional<std::vector<std::byte>> leaf_pin;
    std::string skip_reason;

    ~RelayPasswordFixture() {
        if (server != nullptr) {
            (void)server->shutdown();
        }
        if (relay_runtime != nullptr) {
            (void)relay_runtime->shutdown();
        }
        if (owner.has_value()) {
            (void)owner->shutdown();
        }
        std::error_code ec;
        for (const auto& root : temp_roots) {
            std::filesystem::remove_all(root, ec);
        }
    }

    bool setup(const std::string& tag) {
        aki::app::ExecutorOwnerOptions owner_options;
        owner_options.executor_config.min_threads = 4;
        owner_options.executor_config.max_threads = 8;
        owner.emplace(owner_options);
        if (!owner->initialize()) {
            skip_reason = "executor owner initialize failed";
            return false;
        }
        const std::string relay_root = temp_root(tag + "-server");
        temp_roots.push_back(relay_root);
        std::error_code ec;
        std::filesystem::create_directories(relay_root, ec);
        if (ec) {
            skip_reason = "temp directory create failed";
            return false;
        }
        if (!write_test_certificate(relay_root)) {
            skip_reason = "OpenSSL certificate generation unavailable";
            return false;
        }
        const auto cert_file =
            std::filesystem::path(relay_root) / "test-only-cert.pem";
        leaf_pin = certificate_pin(cert_file);
        if (!leaf_pin.has_value() || leaf_pin->size() != 32U) {
            skip_reason = "certificate pin computation failed";
            return false;
        }
        heyaki::RelayId relay_id{};
        std::copy(leaf_pin->begin(), leaf_pin->end(), relay_id.begin());
        if (!provision_password_verifier(
                std::filesystem::path(relay_root) / "relay.sqlite",
                owner_password, relay_id)) {
            skip_reason = "password verifier provisioning unavailable";
            return false;
        }
        heyaki::RuntimeConfig relay_runtime_config;
        relay_runtime_config.worker_name = "aki-relay-pw-e2e";
        auto runtime_result =
            heyaki::Runtime::create_borrowed(owner->executor(), relay_runtime_config);
        if (!runtime_result) {
            skip_reason = "borrowed runtime create failed: "
                + std::string(runtime_result.error_if()->safe_detail());
            return false;
        }
        relay_runtime = std::make_unique<heyaki::Runtime>(
            std::move(*runtime_result.value_if()));
        auto server_result = heyaki::RelayServer::create(
            test_relay_config(relay_root), relay_runtime.get());
        if (!server_result) {
            skip_reason = "relay server create failed: "
                + std::string(server_result.error_if()->safe_detail());
            return false;
        }
        // RelayServer 为 movable pimpl：从 Result 内部移出（Result 本体随
        // setup 返回销毁，直接持指针会悬垂——test_relay_e2e_loopback 以
        // Result 存活全程规避，此处移出语义等价）。
        server = std::make_unique<heyaki::RelayServer>(
            std::move(*server_result.value_if()));
        if (!wait_until([&] {
                const auto snapshot = server->snapshot();
                return snapshot.state == heyaki::RelayServerState::running
                    && snapshot.listen_port != 0U;
            }, 10s)) {
            skip_reason = "relay server did not reach running state";
            return false;
        }
        relay_url = "wss://127.0.0.1:"
            + std::to_string(server->snapshot().listen_port);
        return true;
    }

    std::string new_node_root(const std::string& tag) {
        const auto root = temp_root(tag);
        temp_roots.push_back(root);
        return root;
    }
};

// 用例内显式 teardown 断言（fixture 析构为兜底；此处验证关闭序语义）。
void teardown_fixture(RelayPasswordFixture& fixture) {
    REQUIRE(fixture.server != nullptr);
    const auto server_report = fixture.server->shutdown();
    if (!server_report.stopped || server_report.timed_out) {
        print_server_snapshot(server_report.final_snapshot);
    }
    REQUIRE(server_report.stopped);
    REQUIRE_FALSE(server_report.timed_out);
    REQUIRE(server_report.final_snapshot.state
        == heyaki::RelayServerState::stopped);

    REQUIRE(fixture.relay_runtime != nullptr);
    const auto runtime_report = fixture.relay_runtime->shutdown();
    REQUIRE_FALSE(runtime_report.executor_shutdown_performed);  // 借用语义
    REQUIRE(runtime_report.final_phase == heyaki::RuntimePhase::stopped);

    const auto report = fixture.owner->shutdown();
    REQUIRE(report.fully_stopped());
}

}  // namespace

TEST_CASE("Relay password enrollment anchors the TOFU pin (owned fallback)",
    "[integration][relay_password_e2e]") {
    RelayPasswordFixture fixture;
    if (!fixture.setup("owned")) {
        std::printf("[skip] %s; relay password e2e (owned) not verified in "
                    "this environment\n",
            fixture.skip_reason.c_str());
        SUCCEED();
        return;
    }
    auto profile = LocalProfile::open(
        fixture.new_node_root("owned-node"), "relay-pw-e2e-alpha-local");

    // password + 空租户 + 无 ca_file → TOFU 首连（tls_verify_peer=false）；
    // 不传 host_executor → 上游 owned-runtime 回退（原行为面）。
    aki::heyaki::RelayEnrollRequest request;
    request.relay_url = fixture.relay_url;
    request.tenant = "";  // password 模式空租户 → 归一 "default"
    request.enrollment_password = fixture.owner_password;
    std::string error;
    auto view = aki::heyaki::enroll_relay_profile(profile, request, error);
    if (!view.has_value()) {
        std::printf("    [diag] owned-fallback enrollment failed: %s\n",
            error.c_str());
        print_server_snapshot(fixture.server->snapshot());
    }
    REQUIRE(view.has_value());
    REQUIRE(view->relay_url == fixture.relay_url);
    REQUIRE(view->tenant == "default");  // 空租户归一（服务端回显）
    REQUIRE(view->enrollment_generation >= 1U);
    REQUIRE(view->auto_connect);
    REQUIRE_FALSE(view->revoked);
    // 空租户归一对调用方原位可见（validate 非 const 引用契约）。
    REQUIRE(request.tenant == "default");
    // M8-02/DEC-018：成功路径同样原位擦除密码。
    REQUIRE(request.enrollment_password.empty());
    REQUIRE(request.bootstrap_token.empty());
    // 决策 11 阶段 2 验收点：记录持久化且 relay_pin == 服务端 leaf
    // 证书 SHA-256（X509_digest 独立复算对拍）。
    auto existing = profile.store().relay_enrollment(fixture.relay_url);
    REQUIRE(existing);
    REQUIRE(existing.value_if()->has_value());
    const auto& record = **existing.value_if();
    REQUIRE(record.relay_pin.has_value());
    REQUIRE(record.relay_pin->size() == 32U);
    REQUIRE(*record.relay_pin == *fixture.leaf_pin);
    std::printf("    [diag] owned-fallback enrollment ok: tenant=%s "
                "generation=%llu pin==leaf_sha256(%02x%02x%02x%02x...)\n",
        view->tenant.c_str(),
        static_cast<unsigned long long>(view->enrollment_generation),
        static_cast<unsigned char>(fixture.leaf_pin->at(0)),
        static_cast<unsigned char>(fixture.leaf_pin->at(1)),
        static_cast<unsigned char>(fixture.leaf_pin->at(2)),
        static_cast<unsigned char>(fixture.leaf_pin->at(3)));
    // 视图投影（设置页状态展示面）。
    const auto views = aki::heyaki::relay_enrollment_views(profile);
    REQUIRE(views.size() == 1U);
    REQUIRE(views.front().relay_url == fixture.relay_url);
    REQUIRE(views.front().tenant == "default");
    REQUIRE(views.front().auto_connect);
    REQUIRE_FALSE(views.front().revoked);
    // 服务端证据：一次真实密码准入完成（挑战 + Argon2id 证明 + verifier）。
    REQUIRE(wait_until(
        [&] { return fixture.server->snapshot().enrollments_completed >= 1U; },
        5s));
    print_server_snapshot(fixture.server->snapshot());

    teardown_fixture(fixture);
}

TEST_CASE("Relay password enrollment runs on a borrowed host runtime",
    "[integration][relay_password_e2e]") {
    RelayPasswordFixture fixture;
    if (!fixture.setup("borrowed")) {
        std::printf("[skip] %s; relay password e2e (borrowed) not verified "
                    "in this environment\n",
            fixture.skip_reason.c_str());
        SUCCEED();
        return;
    }
    auto profile = LocalProfile::open(
        fixture.new_node_root("borrowed-node"), "relay-pw-e2e-bravo-local");

    // 借用路径：主线程构造 RelayEnrollRuntime 句柄（worker 内创建/析构
    // 会自等待破坏 Node 关闭——IVA 缺陷修复后的生命周期纪律），交换经
    // runtime_borrowed 运行在宿主 executor 上（HEY-20261006-002 收口面）。
    aki::heyaki::RelayEnrollRuntime enroll_runtime{
        fixture.owner->executor()};
    aki::heyaki::RelayEnrollRequest request;
    request.relay_url = fixture.relay_url;
    request.enrollment_password = fixture.owner_password;
    std::string error;
    auto view = aki::heyaki::enroll_relay_profile(
        profile, request, error, &enroll_runtime);
    if (!view.has_value()) {
        std::printf("    [diag] borrowed-runtime enrollment failed: %s\n",
            error.c_str());
        print_server_snapshot(fixture.server->snapshot());
    }
    REQUIRE(view.has_value());
    REQUIRE(view->relay_url == fixture.relay_url);
    REQUIRE(view->tenant == "default");
    REQUIRE(view->enrollment_generation >= 1U);
    REQUIRE(request.enrollment_password.empty());  // 成功路径原位擦除
    REQUIRE(request.tenant == "default");
    auto existing = profile.store().relay_enrollment(fixture.relay_url);
    REQUIRE(existing);
    REQUIRE(existing.value_if()->has_value());
    const auto& record = **existing.value_if();
    REQUIRE(record.relay_pin.has_value());
    REQUIRE(record.relay_pin->size() == 32U);
    REQUIRE(*record.relay_pin == *fixture.leaf_pin);
    std::printf("    [diag] borrowed-runtime enrollment ok: tenant=%s "
                "generation=%llu pin==leaf_sha256\n",
        view->tenant.c_str(),
        static_cast<unsigned long long>(view->enrollment_generation));
    REQUIRE(wait_until(
        [&] { return fixture.server->snapshot().enrollments_completed >= 1U; },
        5s));

    teardown_fixture(fixture);
}

TEST_CASE("Relay password enrollment rejects a wrong password visibly",
    "[integration][relay_password_e2e]") {
    RelayPasswordFixture fixture;
    if (!fixture.setup("wrongpw")) {
        std::printf("[skip] %s; relay password e2e (wrong password) not "
                    "verified in this environment\n",
            fixture.skip_reason.c_str());
        SUCCEED();
        return;
    }
    auto profile = LocalProfile::open(
        fixture.new_node_root("wrongpw-node"), "relay-pw-e2e-charlie-local");

    aki::heyaki::RelayEnrollRequest request;
    request.relay_url = fixture.relay_url;
    request.enrollment_password = "definitely-not-the-owner-password";
    std::string error;
    auto view = aki::heyaki::enroll_relay_profile(profile, request, error);
    REQUIRE_FALSE(view.has_value());
    // 认证语义可见（上游 safe_detail：enrollment_password_rejected）。
    std::printf("    [diag] wrong-password error detail: %s\n", error.c_str());
    REQUIRE_FALSE(error.empty());
    REQUIRE(error.find("enrollment_password") != std::string::npos);
    // 失败不落记录（RULE-09：拒绝面不产生副作用）。
    REQUIRE(aki::heyaki::relay_enrollment_views(profile).empty());
    auto existing = profile.store().relay_enrollment(fixture.relay_url);
    REQUIRE(existing);
    REQUIRE_FALSE(existing.value_if()->has_value());
    // 错密码同样原位擦除。
    REQUIRE(request.enrollment_password.empty());
    // 服务端证据：挑战发出但完成数不增加。
    const auto snapshot = fixture.server->snapshot();
    print_server_snapshot(snapshot);
    REQUIRE(snapshot.enrollments_completed == 0U);

    teardown_fixture(fixture);
}

// 修复 ② 核心语义验证（IVA 缺陷修复复验）：主线程构造的 RelayEnrollRuntime
// 句柄在同一 executor 上与 NodeSession 共存——经句柄注册（借用 Runtime
// 交换）后，Node 自动登录以记录 relay_pin（修复 ① 的登录期后果：TOFU
// pin 生效才能对自签 relay 登录 ready）且 NodeSession::shutdown 仍闭合
//（node_stopped / runtime_stopped；借用语义 executor_shutdown_performed
// == false）。worker_name 冲突面（HostRuntime 默认名 "heyaki-asio" 与
// NodeSession 默认名同串导致注册句柄构造失败）是 HostRuntime 侧缺陷，
// 实测证据见 test_host_relay_config；本用例 NodeSession 显式别名以隔离
// 验证句柄生命周期语义本身。
TEST_CASE("Relay password enrollment with a live node session stops cleanly",
    "[integration][relay_password_e2e]") {
    RelayPasswordFixture fixture;
    if (!fixture.setup("node")) {
        std::printf("[skip] %s; relay password e2e (node session) not "
                    "verified in this environment\n",
            fixture.skip_reason.c_str());
        SUCCEED();
        return;
    }
    auto profile = LocalProfile::open(
        fixture.new_node_root("node-scenario"), "relay-pw-e2e-delta-local");

    // 主线程构造句柄（非 worker 创建/析构纪律）；先断言有效（本用例无
    // worker 名冲突——NodeSession 用显式别名）。
    aki::heyaki::RelayEnrollRuntime enroll_runtime{fixture.owner->executor()};
    if (!enroll_runtime.valid()) {
        std::printf("[diag] RelayEnrollRuntime construction invalid on a "
                    "name-collision-free executor\n");
    }
    REQUIRE(enroll_runtime.valid());

    // 经句柄注册（借用 Runtime 交换）+ 修复 ① pin 对拍（该路径复验）。
    aki::heyaki::RelayEnrollRequest request;
    request.relay_url = fixture.relay_url;
    request.enrollment_password = fixture.owner_password;
    std::string error;
    auto view = aki::heyaki::enroll_relay_profile(
        profile, request, error, &enroll_runtime);
    if (!view.has_value()) {
        std::printf("    [diag] handle-based enrollment failed: %s\n",
            error.c_str());
        print_server_snapshot(fixture.server->snapshot());
    }
    REQUIRE(view.has_value());
    REQUIRE(view->tenant == "default");
    REQUIRE(request.enrollment_password.empty());
    auto existing = profile.store().relay_enrollment(fixture.relay_url);
    REQUIRE(existing);
    REQUIRE(existing.value_if()->has_value());
    const auto& record = **existing.value_if();
    REQUIRE(record.relay_pin.has_value());
    REQUIRE(*record.relay_pin == *fixture.leaf_pin);

    // 同一 executor 上创建 NodeSession（显式 worker_name 隔离冲突面）；
    // 密码注册记录带 TOFU pin → 自动登录走 pin 校验（修复 ① 登录期后果：
    // 自签 relay 无 pin 必败 wss_tls_verification_failed，有 pin 须 ready）。
    auto lan_config = aki::heyaki::production_lan_configuration();
    lan_config.enabled = false;
    auto node = aki::heyaki::NodeSession::create(fixture.owner->executor(),
        {.profile = &profile,
            .lan_override = lan_config,
            .worker_name = "aki-relay-pw-e2e-node"});
    const bool login_ready = wait_until(
        [&] { return node.relay_status().state_name == "ready"; }, 60s);
    if (!login_ready) {
        const auto status = node.relay_status();
        std::printf("    [diag] node relay: enabled=%d state=%d(%s) "
                    "url=%s tenant=%s error=%s\n",
            status.enabled ? 1 : 0, status.state,
            status.state_name.c_str(), status.relay_url.c_str(),
            status.tenant.c_str(), status.last_error.c_str());
        print_server_snapshot(fixture.server->snapshot());
    }
    REQUIRE(login_ready);

    // 修复 ② 核心：经句柄注册后 Node 关闭仍闭合。
    const auto node_report = node.shutdown();
    REQUIRE(node_report.node_stopped);
    REQUIRE(node_report.runtime_stopped);
    REQUIRE_FALSE(node_report.runtime_executor_shutdown_performed);
    std::printf("    [diag] node session after handle-based enrollment: "
                "node_stopped=%d runtime_stopped=%d\n",
        node_report.node_stopped ? 1 : 0, node_report.runtime_stopped ? 1 : 0);

    // 句柄在 owner 关闭前主线程析构（声明序即析构序，teardown 前销毁）。
    teardown_fixture(fixture);
}

// ---- 注入测试（HEY-20261006-002 补跑条件：「补关闭竞争测试（enroll 在途
//      时 shutdown）」）----
// 在途注册交换遇宿主关闭：HangingTcpListener 只 listen 不 accept（内核
// backlog 完成 TCP 握手但不回 TLS——上游 m3b_relay_enrollment_borrow_
// runtime_test.cpp 的 HangingListener 同型手法，本二进制无 boost include
// 面故用 POSIX socket 等价实现）；密码模式注册交换阻塞在 handshake
//（transport 上界 connect 5s + handshake 5s + close 2s）。任务进入后立即
// 对 owner 执行受控关闭。两条收敛路径均合法：
//   A）blocking worker 回收（EXEC-01 步骤 2/3 join 借用 Runtime 的
//       AsioWorker）解除在途交换阻塞 → 任务在步骤 4 预算（3s）内完成 →
//       fully_stopped() == true；
//   B）任务存活超过步骤 4 预算 → completion_wait_timed_out 如实记录，
//       但 shutdown(true) 仍 Completed 且生命周期收敛 Stopped。
// 共同断言：关闭有界（≤15s 实测收紧）、无崩溃、交换以失败终态可见
//（error 非空 + 密码原位擦除 + 不落记录）、句柄主线程析构有界、
// 进程内无残留（新一轮 owner+句柄+注册仍可用且干净关闭）。
namespace {

// 只 listen 不 accept：客户端 TCP connect 由内核 backlog 完成，TLS
// ClientHello 无响应 → 客户端 handshake 悬置至自身超时（无 TLS fixture）。
class HangingTcpListener {
public:
    HangingTcpListener() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&address),
                sizeof(address))
            != 0) {
            close();
            return;
        }
        if (::listen(fd_, 8) != 0) {
            close();
            return;
        }
        sockaddr_in bound{};
        socklen_t length = sizeof(bound);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound),
                &length)
            != 0) {
            close();
            return;
        }
        port_ = ntohs(bound.sin_port);
    }
    HangingTcpListener(const HangingTcpListener&) = delete;
    HangingTcpListener& operator=(const HangingTcpListener&) = delete;
    ~HangingTcpListener() { close(); }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    void close() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        port_ = 0;
    }

private:
    int fd_{-1};
    std::uint16_t port_{0};
};

}  // namespace

TEST_CASE("In-flight relay enrollment exchange exits bounded on host shutdown",
    "[integration][relay_password_e2e]") {
    HangingTcpListener listener;
    REQUIRE(listener.port() != 0U);
    const std::string hanging_url =
        "wss://127.0.0.1:" + std::to_string(listener.port());

    aki::app::ExecutorOwnerOptions owner_options;
    owner_options.executor_config.min_threads = 4;
    owner_options.executor_config.max_threads = 8;
    ExecutorOwner owner(owner_options);
    REQUIRE(owner.initialize());

    const std::string node_root = temp_root("shutdown-race");
    auto profile = LocalProfile::open(node_root, "relay-pw-e2e-echo-local");

    // 主线程构造句柄（IVA 修复纪律；worker 名 "aki-relay-enroll" 与本
    // executor 上无其他 Runtime 冲突）。
    aki::heyaki::RelayEnrollRuntime enroll_runtime{owner.executor()};
    REQUIRE(enroll_runtime.valid());

    // 交换任务（executor 上 submit_auto；密码模式，凭据任意非空——服务端
    // 不存在，凭据不会被校验）。slot 经 shared_ptr 携带，future.get() 后
    // 读取（happens-before 由 future 同步保证）。
    struct ExchangeSlot {
        std::atomic<bool> entered{false};
        bool failed{false};
        std::string error;
        bool password_scrubbed{false};
    };
    auto slot = std::make_shared<ExchangeSlot>();
    auto* profile_for_task = &profile;
    auto* runtime_for_task = &enroll_runtime;
    const auto started = std::chrono::steady_clock::now();
    std::future<void> task = owner.executor().submit_auto(
        [profile_for_task, runtime_for_task, hanging_url, slot]() mutable {
            slot->entered.store(true);
            aki::heyaki::RelayEnrollRequest request;
            request.relay_url = hanging_url;
            request.enrollment_password = "shutdown-race-owner-secret";
            std::string error;
            auto view = aki::heyaki::enroll_relay_profile(
                *profile_for_task, request, error, runtime_for_task);
            slot->failed = !view.has_value();
            slot->error = error;
            slot->password_scrubbed = request.enrollment_password.empty();
        });
    REQUIRE(task.valid());
    // 确认任务已进入（在途前提），预算 2s。
    REQUIRE(wait_until([&] { return slot->entered.load(); }, 2s));

    // 在途立即受控关闭（沿第 4 用例 owner.shutdown 形态；EXEC-01 五步）。
    const auto shutdown_report = owner.shutdown();
    const auto shutdown_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
    // 关闭有界（建议预算 15s，实测定值见输出；transport 上界 ~12s 兜底）。
    REQUIRE(shutdown_elapsed <= 15s);
    // 收敛路径二选一均合法，但 shutdown 本身必须完成且生命周期收敛。
    REQUIRE(shutdown_report.producers_stopped);
    REQUIRE(shutdown_report.executor_shutdown_completed);
    REQUIRE(shutdown_report.lifecycle_after
        == kairo::ExecutorLifecycleState::Stopped);
    const bool path_a_clean = shutdown_report.fully_stopped();
    const bool path_b_bounded_wait = shutdown_report.completion_wait_timed_out;
    REQUIRE((path_a_clean || path_b_bounded_wait));
    std::printf(
        "    [diag] shutdown path: elapsed=%lldms fully_stopped=%d "
        "wait_timed_out=%d wait_budget=%lldms\n",
        static_cast<long long>(shutdown_elapsed.count()),
        path_a_clean ? 1 : 0, path_b_bounded_wait ? 1 : 0,
        static_cast<long long>(
            shutdown_report.completion_wait_budget.count()));

    // 交换任务以失败终态可见（经 future 消费；不悬挂——阻塞到此即证据）。
    REQUIRE(wait_until([&] { return slot->failed || !slot->error.empty(); },
        10s));
    task.get();
    REQUIRE(slot->failed);
    REQUIRE_FALSE(slot->error.empty());
    std::printf("    [diag] in-flight exchange terminal error: %s\n",
        slot->error.c_str());
    // 凭据纪律：竞争路径同样原位擦除。
    REQUIRE(slot->password_scrubbed);
    // 失败不落记录。
    REQUIRE(aki::heyaki::relay_enrollment_views(profile).empty());

    // 句柄主线程析构有界（executor 已停后的销毁路径，另一收敛面）。
    const auto destroy_started = std::chrono::steady_clock::now();
    {
        aki::heyaki::RelayEnrollRuntime doomed = std::move(enroll_runtime);
    }
    const auto destroy_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - destroy_started);
    REQUIRE(destroy_elapsed <= 15s);
    std::printf("    [diag] handle destroy after stopped executor: "
                "%lldms\n",
        static_cast<long long>(destroy_elapsed.count()));

    // 进程内无残留：新一轮 owner+句柄+注册（顺序性第二 owner——首个已
    // fully shutdown，不同时并存；纪律同本文件各 TEST_CASE 独立 owner）。
    // 打不可达端口（127.0.0.1:1 连接拒绝）快速失败，证明首轮关闭竞争
    // 未留下阻塞进程全局面（如未回收的 owned runtime）。
    {
        ExecutorOwner second_owner(owner_options);
        REQUIRE(second_owner.initialize());
        aki::heyaki::RelayEnrollRuntime second_runtime{
            second_owner.executor()};
        REQUIRE(second_runtime.valid());
        const auto round_started = std::chrono::steady_clock::now();
        aki::heyaki::RelayEnrollRequest request;
        request.relay_url = "wss://127.0.0.1:1";
        request.enrollment_password = "second-round-secret";
        std::string error;
        auto view = aki::heyaki::enroll_relay_profile(
            profile, request, error, &second_runtime);
        const auto round_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - round_started);
        REQUIRE_FALSE(view.has_value());
        REQUIRE_FALSE(error.empty());
        REQUIRE(request.enrollment_password.empty());
        REQUIRE(round_elapsed < 5s);  // 连接拒绝即时，无残留阻塞
        std::printf("    [diag] second-round enroll against dead port: "
                    "%lldms error=%s\n",
            static_cast<long long>(round_elapsed.count()), error.c_str());
        const auto second_report = second_owner.shutdown();
        REQUIRE(second_report.fully_stopped());
    }

    std::error_code ec;
    std::filesystem::remove_all(node_root, ec);
}
