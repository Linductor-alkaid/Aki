// M7（DEC-028 决策 10）：relay 进程内端到端回环集成测试。
//
// 覆盖（单用例二进制，[skip] 受控退出纪律沿 test_discovery_pairing_loopback）：
//   - 真实 RelayServer（heyaki::relay，仅测试目标链接——DEC-028 决策 10）：
//     OpenSSL 自签证书（上游 m3b_relay_test.cpp 同型）+ bootstrap token 经
//     RelayDatabase 种入 + 借用 Runtime（同一测试 Executor，EXEC-01 不建第二
//     executor——RelayServer::create(config, nullptr) 内部 Runtime::create_owned
//     会被拒绝使用，见 relay_server.cpp:1987-1995）。
//   - 真实 enrollment（DEC-028 决策 2）：aki::heyaki::enroll_relay_profile 走
//     真实 WSS + Ed25519 挑战 + token 准入，记录写入 profile store。
//   - NodeSession 自动登录（决策 1/3）：profile 存在有效 enrollment 时
//     Node::create 自动连 relay，relay_status() 收敛 "ready"。
//   - relay 发现（决策 5）：纯 relay 拓扑（production_lan_configuration +
//     lan.enabled=false——LAN 监听/组播关闭，node.cpp:1002 lan_enabled=false）
//     下 endpoints() 仅含 relay 条目；diff_lan_discovery 合成
//     DiscoveryMethod::Relay + "relay:" 前缀（LAN 关闭故断言收紧，无同机
//     LAN 双可见折衷）。
//   - relay 信令建链与配对（决策 6）：connect_peer（automatic 自动选路，无
//     LAN 条目 → relay 信令兜底）→ pairing_restricted → 单侧口令 pair_peer →
//     双侧 authenticated + 发起方 observer 一次性成功。
//   - 经该会话发送 aki.text（DEC-006 映射 4）：B 入站 + A acked。
//   - 关闭序：NodeSession::shutdown × 2（executor_shutdown_performed == false）
//     → server.shutdown() → 借用 runtime shutdown（executor_shutdown_performed
//     == false）→ Executor owner fully_stopped。
//
// TLS 主机名校验核实（2026-10-06，以实测为准）：
//   - enrollment/login transport 在 tls_verify_peer=true 时装载
//     boost::asio::ssl::host_name_verification(URL host)
//     （third_party/heyaki/src/client/relay_wss_client.cpp:644-655）；pinned
//     boost 的实现对 IP 字面量 host 走 X509_check_ip_asc
//     （third_party/boost-asio/include/boost/asio/ssl/impl/
//     host_name_verification.ipp:50-58），即只匹配 subjectAltName 的
//     iPAddress 条目。上游 m3b 证书（CN=heyaki-relay-test、无 SAN）在自身
//     测试里靠 verify_none 客户端绕过（m3b_relay_test.cpp:280）；Aki 包装面
//     无 tls_verify_peer=false 入口，故本测试证书额外写入
//     subjectAltName = IP:127.0.0.1（X509V3_EXT_conf_nid NID_subject_alt_name）。
//   - login 路径（Node 自动登录）的 TLS 信任基由 profile 里的 enrollment
//     记录决定：有 relay_pin → verify_peer=false + pin 校验；无 pin →
//     verify_peer=true + tls_ca_file（未持久化，回落系统信任根）。aki
//     包装的 enrollment 在 ca_file 提供时随记录持久化 pin（决策 2 修订，
//     2026-10-06 缺陷修复：此前记录不带任何 login 信任基，自签 relay 重启
//     自动登录必然 wss_tls_verification_failed）。
//
// 单段产品路径（缺陷修复后，2026-10-06 复验定型）：enrollment 成功即断言
// 记录自带 relay_pin（ca_file 首证书 DER 的 SHA-256，与测试侧独立实现
// X509_digest 复算对拍），随后全程无测试面写库：双 NodeSession 自动登录
// ready → A 目录见 B（relay_visible）→ diff 产 Relay 发现 → connect_peer →
// pairing_restricted → pair_peer → 双侧 authenticated → send_text 入站+ack
// → 受控拆除。另在用例首部（先于任何环境性 [skip] 路径）覆盖
// relay_certificate_pin 函数面错误路径（不存在文件/非 PEM 文件同步拒绝）。
#include "app/lifecycle/executor_owner.hpp"
#include "heyaki/adapter/lan_discovery.hpp"
#include "heyaki/adapter/local_identity.hpp"
#include "heyaki/adapter/wire_ids.hpp"
#include "heyaki/session/relay_enrollment.hpp"
#include "heyaki/session/runtime_node.hpp"

#include <heyaki/error.hpp>
#include <heyaki/runtime.hpp>

// DEC-028 决策 10：仅测试目标消费 heyaki::relay（src/relay 内部头，
// heyaki::relay PUBLIC include 传播）。
#include "relay_database.hpp"
#include "relay_server.hpp"

#include <catch2/catch_test_macros.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

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
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using aki::app::ExecutorOwner;
using aki::device::DeviceId;
using aki::heyaki::LanDiscoveryState;
using aki::heyaki::LocalProfile;
using aki::heyaki::NodeSession;

int g_counter = 0;

std::string temp_root(const std::string& tag) {
    auto path = std::filesystem::temp_directory_path()
        / ("aki-relay-e2e-" + tag + "-"
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

// 随机 bootstrap token（32 hex 字符：16 字节 ≥ relay_bootstrap_token_min_bytes，
// 字符集落在 0x21..0x7e 合法域，relay_database.hpp:489-503）。
std::optional<std::string> random_bootstrap_token() {
    std::array<unsigned char, 16> raw{};
    if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) {
        return std::nullopt;
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string token;
    token.reserve(raw.size() * 2);
    for (const auto byte : raw) {
        token.push_back(hex[byte >> 4]);
        token.push_back(hex[byte & 0x0f]);
    }
    return token;
}

// 服务器证书 SHA-256 pin，测试侧独立实现（X509_digest —— 上游 login 期
// pin 校验同 API，relay_wss_client.cpp:326-344）：与产品
// aki::heyaki::relay_certificate_pin（i2d_X509 + EVP_Digest）对拍，两侧
// 均为证书 DER 的 SHA-256，须逐字节一致。
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

// 上游 m3b_relay_test.cpp:87-145 同型自签证书（EC prime256v1、CN 固定、
// 随机序列号），差异仅一处：额外写入 subjectAltName = IP:127.0.0.1——
// 见文件头「TLS 主机名校验核实」。
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
    // 与 m3b 的差异点：iPAddress SAN。缺省 TLS 客户端校验
    // （host_name_verification → X509_check_ip_asc("127.0.0.1")）在无
    // iPAddress SAN 时必失败；上游 m3b 不受影响是因其客户端 verify_none。
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

// 上游 m3b_relay_test.cpp:147-161 同型（listen 127.0.0.1:0、测试证书、
// sqlite 文件、无信号处理、唯一 worker 名）；握手/关闭超时放宽到 5s
// （并行 ctest 下 2s 偏紧）。
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
    config.runtime.worker_name = "aki-relay-e2e";
    return config;
}

void print_relay_status(const char* tag, const aki::heyaki::RelayStatusView& status) {
    std::printf("    [diag] %s relay: enabled=%d state=%d(%s) url=%s tenant=%s\n",
        tag, status.enabled ? 1 : 0, status.state, status.state_name.c_str(),
        status.relay_url.c_str(), status.tenant.c_str());
    if (!status.last_error.empty()) {
        std::printf("    [diag] %s relay last_error: %s\n",
            tag, status.last_error.c_str());
    }
}

void print_server_snapshot(const heyaki::RelayServerSnapshot& snapshot) {
    std::printf(
        "    [diag] server: state=%d port=%u tcp=%llu ws=%llu hs_failed=%llu "
        "proto_rejected=%llu enroll_ch=%llu enroll_ok=%llu login_ch=%llu "
        "login_ok=%llu hb=%llu pub=%llu query=%llu fwd=%llu\n",
        static_cast<int>(snapshot.state),
        static_cast<unsigned>(snapshot.listen_port),
        static_cast<unsigned long long>(snapshot.tcp_accepted),
        static_cast<unsigned long long>(snapshot.websocket_accepted),
        static_cast<unsigned long long>(snapshot.handshake_failed),
        static_cast<unsigned long long>(snapshot.protocol_rejected),
        static_cast<unsigned long long>(snapshot.enrollment_challenges),
        static_cast<unsigned long long>(snapshot.enrollments_completed),
        static_cast<unsigned long long>(snapshot.login_challenges),
        static_cast<unsigned long long>(snapshot.logins_completed),
        static_cast<unsigned long long>(snapshot.heartbeats),
        static_cast<unsigned long long>(snapshot.endpoint_publications),
        static_cast<unsigned long long>(snapshot.endpoint_queries),
        static_cast<unsigned long long>(snapshot.signaling_forwarded));
    if (snapshot.last_error.has_value()) {
        std::printf("    [diag] server last_error: %s\n",
            std::string(snapshot.last_error->safe_detail()).c_str());
    }
}

}  // namespace

TEST_CASE("Relay enrollment, login, discovery, pairing and messaging in-process",
    "[integration][relay_e2e]") {
    // relay_certificate_pin 函数面错误路径（网络无关，先于任何环境性
    // [skip] 路径必达）：不存在文件与非 PEM 文件同步拒绝（error 非空，
    // RULE-09 拒绝可见；存在性 request 校验另见 test_relay_integration）。
    {
        std::string pin_error;
        REQUIRE_FALSE(aki::heyaki::relay_certificate_pin(
            std::filesystem::path("/nonexistent/aki-relay-pin-check.pem"),
            pin_error)
            .has_value());
        REQUIRE_FALSE(pin_error.empty());
    }
    {
        const auto pin_root = temp_root("pin-check");
        std::error_code pin_ec;
        std::filesystem::create_directories(pin_root, pin_ec);
        REQUIRE_FALSE(pin_ec);
        const auto not_pem = std::filesystem::path(pin_root) / "not-a-cert.txt";
        {
            std::ofstream out(not_pem, std::ios::binary);
            out << "this is not a PEM certificate\n";
        }
        std::string pin_error;
        REQUIRE_FALSE(aki::heyaki::relay_certificate_pin(not_pem, pin_error)
            .has_value());
        REQUIRE_FALSE(pin_error.empty());
        std::filesystem::remove_all(pin_root, pin_ec);
    }

    // 单一 Executor owner（EXEC-01：进程内唯一 owner；三个借用 Runtime 的
    // worker 名互异——aki-relay-e2e / -node-a / -node-b）。
    aki::app::ExecutorOwnerOptions owner_options;
    owner_options.executor_config.min_threads = 4;
    owner_options.executor_config.max_threads = 8;
    ExecutorOwner owner(owner_options);
    REQUIRE(owner.initialize());

    const std::string relay_root = temp_root("server");
    const std::string root_a = temp_root("node-a");
    const std::string root_b = temp_root("node-b");
    std::error_code ec;
    std::filesystem::create_directories(relay_root, ec);
    REQUIRE_FALSE(ec);

    // 环境性失败（TLS 证书生成不可用）→ [skip] 受控退出（loopback 纪律）。
    if (!write_test_certificate(relay_root)) {
        std::printf("[skip] OpenSSL certificate generation unavailable; relay "
                    "e2e loopback not verified in this environment\n");
        const auto skip_report = owner.shutdown();
        REQUIRE(skip_report.fully_stopped());
        return;
    }
    const auto ca_file = std::filesystem::path(relay_root) / "test-only-cert.pem";

    // 种 bootstrap token（server 启动前直写 sqlite——同一数据库文件，m4
    // relay 测试同型先例；本测试走真实 enrollment 消费，不直写设备记录）。
    auto token = random_bootstrap_token();
    REQUIRE(token.has_value());
    const std::string tenant = "aki";
    {
        auto database = heyaki::RelayDatabase::open(
            std::filesystem::path(relay_root) / "relay.sqlite");
        REQUIRE(database);
        const auto expires = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
                + 10min)
                .count());
        auto seeded = database.value_if()->create_bootstrap_token(
            tenant, *token, expires, 5U);
        REQUIRE(seeded);
        REQUIRE(seeded.value_if()->tenant == tenant);
        REQUIRE(seeded.value_if()->remaining_uses == 5U);
    }

    // 借用 Runtime + RelayServer（DEC-028 决策 10：不传 nullptr——那会在
    // 进程内 Runtime::create_owned 建第二个 executor，违反 EXEC-01）。
    heyaki::RuntimeConfig relay_runtime_config;
    relay_runtime_config.worker_name = "aki-relay-e2e";
    auto relay_runtime_result = heyaki::Runtime::create_borrowed(
        owner.executor(), relay_runtime_config);
    REQUIRE(relay_runtime_result);
    // 堆置稳定地址（RuntimeSession 同款理由：非拥有指针跨异步路径存活）。
    auto relay_runtime =
        std::make_unique<heyaki::Runtime>(std::move(*relay_runtime_result.value_if()));

    auto server_result = heyaki::RelayServer::create(
        test_relay_config(relay_root), relay_runtime.get());
    if (!server_result) {
        // 环境性失败（端口绑定等）→ [skip] 受控退出并打印原因。
        std::printf("[skip] relay server create failed: %s\n",
            std::string(server_result.error_if()->safe_detail()).c_str());
        (void)relay_runtime->shutdown();
        const auto skip_report = owner.shutdown();
        REQUIRE(skip_report.fully_stopped());
        return;
    }
    auto& server = *server_result.value_if();
    REQUIRE(wait_until([&] {
        const auto snapshot = server.snapshot();
        return snapshot.state == heyaki::RelayServerState::running
            && snapshot.listen_port != 0U;
    }, 10s));
    const auto ready_snapshot = server.snapshot();
    REQUIRE(ready_snapshot.state == heyaki::RelayServerState::running);
    REQUIRE(ready_snapshot.listen_port != 0U);
    const std::string relay_url =
        "wss://127.0.0.1:" + std::to_string(ready_snapshot.listen_port);

    // 双端 enrollment（真实 WSS + Ed25519 挑战 + token 准入，DEC-028 决策 2）。
    const std::string password_a = "relay-e2e-alpha-local";
    const std::string password_b = "relay-e2e-bravo-local";
    auto profile_a = LocalProfile::open(root_a, password_a);
    auto profile_b = LocalProfile::open(root_b, password_b);
    REQUIRE(profile_a.identity().id != profile_b.identity().id);

    for (auto* profile : {&profile_a, &profile_b}) {
        aki::heyaki::RelayEnrollRequest request;
        request.relay_url = relay_url;
        request.tenant = tenant;
        request.bootstrap_token = *token;
        request.ca_file = ca_file;
        std::string error;
        auto view = aki::heyaki::enroll_relay_profile(*profile, request, error);
        if (!view.has_value()) {
            std::printf("    [diag] enrollment failed: %s\n", error.c_str());
            print_server_snapshot(server.snapshot());
        }
        REQUIRE(view.has_value());
        REQUIRE(view->relay_url == relay_url);
        REQUIRE(view->tenant == tenant);
        REQUIRE(view->enrollment_generation >= 1U);
        REQUIRE(view->auto_connect);
        REQUIRE_FALSE(view->revoked);
    }
    // 决策 2 修订（缺陷修复验证点）：ca_file 提供时 enrollment 记录持久化
    // relay_pin = SHA-256(首证书 DER)。直查 heyaki 记录本体（RelayEnrollment-
    // View 不投影 pin），并与测试侧独立实现（certificate_pin / X509_digest）
    // 复算值逐字节对拍。
    const auto expected_pin = certificate_pin(ca_file);
    REQUIRE(expected_pin.has_value());
    for (auto* profile : {&profile_a, &profile_b}) {
        auto existing = profile->store().relay_enrollment(relay_url);
        REQUIRE(existing);
        REQUIRE(existing.value_if()->has_value());
        const auto& record = **existing.value_if();
        REQUIRE(record.relay_pin.has_value());
        REQUIRE(record.relay_pin->size() == 32U);
        REQUIRE(*record.relay_pin == *expected_pin);
    }
    // token 副本擦除后不再需要；profile 落库记录可见（决策 2 的持久化面）。
    std::fill(token->begin(), token->end(), '\0');
    for (auto* profile : {&profile_a, &profile_b}) {
        const auto views = aki::heyaki::relay_enrollment_views(*profile);
        REQUIRE(views.size() == 1U);
        REQUIRE(views.front().relay_url == relay_url);
        REQUIRE(views.front().auto_connect);
        REQUIRE_FALSE(views.front().revoked);
    }
    // 真实准入路径的服务端证据：两条 enrollment 完成（挑战 + 签名 + token 消费）。
    REQUIRE(wait_until([&] {
        return server.snapshot().enrollments_completed >= 2U;
    }, 5s));

    // 双 NodeSession（决策 1 automatic + 决策 5 纯 relay 拓扑：LAN 目录关闭）。
    auto lan_config = aki::heyaki::production_lan_configuration();
    lan_config.enabled = false;

    std::optional<NodeSession> side_a;
    std::optional<NodeSession> side_b;
    side_a.emplace(NodeSession::create(owner.executor(),
        {.profile = &profile_a,
            .lan_override = lan_config,
            .worker_name = "aki-relay-e2e-node-a"}));
    side_b.emplace(NodeSession::create(owner.executor(),
        {.profile = &profile_b,
            .lan_override = lan_config,
            .worker_name = "aki-relay-e2e-node-b"}));

    // LAN 关闭的可观测证据（纯 relay 拓扑前提）。
    REQUIRE_FALSE(side_a->has_lan_interfaces());
    REQUIRE_FALSE(side_b->has_lan_interfaces());

    // 决策 3：profile 存在有效 enrollment → Node 自动登录 relay。
    for (auto* side : {&*side_a, &*side_b}) {
        const auto status = side->relay_status();
        REQUIRE(status.enabled);
        REQUIRE(status.relay_url == relay_url);
        REQUIRE(status.tenant == tenant);
    }

    // 登录收敛——产品路径（决策 3）：记录自带 relay_pin → 上游 login 走
    // TOFU pin 校验（tls_verify_peer=false + 呈现证书 SHA-256 对比，
    // node.cpp:1122-1129 / relay_wss_client.cpp:326-344），全程无测试面
    // 写库。心跳/租约缺省 15s/45s，预算 60s。此段失败即回归（pin 信任基
    // 未随记录持久化，或 login pin 校验链路损坏）。
    const bool product_login_ready = wait_until([&] {
        return side_a->relay_status().state_name == "ready"
            && side_b->relay_status().state_name == "ready";
    }, 60s);
    if (!product_login_ready) {
        print_relay_status("A", side_a->relay_status());
        print_relay_status("B", side_b->relay_status());
        print_server_snapshot(server.snapshot());
    }
    REQUIRE(product_login_ready);

    // 服务端登录证据。
    REQUIRE(wait_until([&] { return server.snapshot().logins_completed >= 2U; }, 5s));

    // relay 发现（决策 5）：A 目录出现 B 的 relay 条目（poll_interval 100ms）。
    const auto id_b = profile_b.identity().id;
    const auto id_a = profile_a.identity().id;
    const bool b_visible = wait_until([&] {
        for (const auto& entry : side_a->endpoints()) {
            if (entry.device_id == id_b && entry.relay_visible) {
                return true;
            }
        }
        return false;
    }, 20s);
    if (!b_visible) {
        print_relay_status("A", side_a->relay_status());
        print_relay_status("B", side_b->relay_status());
        print_server_snapshot(server.snapshot());
    }
    REQUIRE(b_visible);

    std::optional<aki::heyaki::EndpointView> entry_b;
    for (const auto& entry : side_a->endpoints()) {
        if (entry.device_id == id_b) {
            entry_b = entry;
            break;
        }
    }
    REQUIRE(entry_b.has_value());
    REQUIRE(entry_b->relay_visible);
    REQUIRE_FALSE(entry_b->lan_visible);  // LAN 关闭：纯 relay 条目
    REQUIRE_FALSE(entry_b->trusted);
    REQUIRE(entry_b->public_key.bytes.size() == 32U);
    REQUIRE(entry_b->public_key.bytes == profile_b.identity().public_key.bytes);

    // diff 两轮：首轮合成 Relay 来源 discovered；次轮幂等无新增。
    LanDiscoveryState discovery_state;
    const auto first_tick =
        aki::heyaki::diff_lan_discovery(side_a->endpoints(), discovery_state);
    const auto* discovered_b = [&]() -> const aki::device::DiscoveredDevice* {
        for (const auto& device : first_tick.discovered) {
            if (device.identity.id == id_b) {
                return &device;
            }
        }
        return nullptr;
    }();
    REQUIRE(discovered_b != nullptr);
    REQUIRE(discovered_b->method == aki::device::DiscoveryMethod::Relay);
    REQUIRE(discovered_b->endpoint.description.rfind("relay:", 0) == 0);
    REQUIRE(discovered_b->identity.public_key.bytes
        == profile_b.identity().public_key.bytes);
    REQUIRE(discovered_b->identity.trust_state == aki::device::TrustState::Unknown);
    REQUIRE(discovered_b->identity.presence == aki::device::PresenceState::Online);
    const auto second_tick =
        aki::heyaki::diff_lan_discovery(side_a->endpoints(), discovery_state);
    for (const auto& device : second_tick.discovered) {
        REQUIRE_FALSE(device.identity.id == id_b);  // 幂等：不重复合成
    }

    // relay 信令建链与配对（决策 6）：connect_peer 自动选路（无 LAN 条目 →
    // relay 信令）→ pairing_restricted → 单侧口令 → 双侧 authenticated。
    std::atomic<bool> paired_a{false};
    std::atomic<int> pairing_failures{0};
    std::mutex pairing_diag_mutex;
    std::string pairing_failure_a;
    side_a->set_pairing_observer(
        [&](const DeviceId& peer, bool ok, const std::string& detail) {
            if (ok && peer == id_b) {
                paired_a.store(true);
            } else if (!ok) {
                pairing_failures.fetch_add(1);
                std::lock_guard<std::mutex> guard(pairing_diag_mutex);
                pairing_failure_a = detail;
            }
        });
    std::atomic<bool> paired_b{false};
    side_b->set_pairing_observer(
        [&](const DeviceId& peer, bool ok, const std::string&) {
            if (ok && peer == id_a) {
                paired_b.store(true);
            }
        });

    REQUIRE(side_a->connect_peer(id_b));
    const bool restricted = wait_until([&] {
        return side_a->session_pairing_restricted(id_b)
            || side_a->session_authenticated(id_b);
    }, 30s);
    if (!restricted) {
        for (const auto& entry : side_a->peer_session_diagnostics()) {
            std::printf("    [diag] A session peer=%s state=%d restricted=%d\n",
                entry.first.c_str(), entry.second.first, entry.second.second);
        }
        print_server_snapshot(server.snapshot());
    }
    REQUIRE(restricted);

    // A 提交 B 的本机口令（M5-11 单侧输入；B responder 即时升级）。
    REQUIRE(side_a->pair_peer(id_b, password_b));
    const bool authorized = wait_until([&] {
        return side_a->session_authenticated(id_b)
            && side_b->session_authenticated(id_a);
    }, 30s);
    if (!authorized) {
        for (const auto& entry : side_a->peer_session_diagnostics()) {
            std::printf("    [diag] A session peer=%s state=%d restricted=%d\n",
                entry.first.c_str(), entry.second.first, entry.second.second);
        }
        for (const auto& entry : side_b->peer_session_diagnostics()) {
            std::printf("    [diag] B session peer=%s state=%d restricted=%d\n",
                entry.first.c_str(), entry.second.first, entry.second.second);
        }
        {
            std::lock_guard<std::mutex> guard(pairing_diag_mutex);
            if (!pairing_failure_a.empty()) {
                std::printf("    [diag] A pairing failure: %s\n",
                    pairing_failure_a.c_str());
            }
        }
        print_server_snapshot(server.snapshot());
    }
    REQUIRE(authorized);
    REQUIRE(wait_until([&] { return paired_a.load(); }, 30s));
    REQUIRE(pairing_failures.load() == 0);

    // 经该会话发送 aki.text：B 入站 + A acked（信封 type/payload/ID 往返）。
    const auto message_id = aki::heyaki::new_message_id();
    std::mutex inbound_mutex;
    std::optional<aki::conversation::MessageId> inbound_id;
    std::string inbound_type;
    std::string inbound_payload;
    std::optional<aki::conversation::MessageId> acked_id;
    std::string acked_event;
    side_b->set_message_handlers(
        [&](const DeviceId& peer, const aki::conversation::MessageId& id,
            const std::string& type, const std::string& payload) {
            std::lock_guard<std::mutex> guard(inbound_mutex);
            if (peer == id_a) {
                inbound_id = id;
                inbound_type = type;
                inbound_payload = payload;
            }
        },
        [](const DeviceId&, const aki::conversation::MessageId&,
            const std::string&) {});
    side_a->set_message_handlers(
        [](const DeviceId&, const aki::conversation::MessageId&,
            const std::string&, const std::string&) {},
        [&](const DeviceId& peer, const aki::conversation::MessageId& id,
            const std::string& event) {
            std::lock_guard<std::mutex> guard(inbound_mutex);
            if (peer == id_b && event == "acked") {
                acked_id = id;
                acked_event = event;
            }
        });
    REQUIRE(side_a->send_text(id_b, message_id, "relay-e2e"));
    REQUIRE(wait_until([&] {
        std::lock_guard<std::mutex> guard(inbound_mutex);
        return inbound_id.has_value() && acked_id.has_value();
    }, 30s));
    {
        std::lock_guard<std::mutex> guard(inbound_mutex);
        REQUIRE(inbound_type == "aki.text");
        REQUIRE(inbound_payload == "relay-e2e");
        REQUIRE(inbound_id.has_value());
        REQUIRE(*inbound_id == message_id);  // MessageId 规范串往返一致
        REQUIRE(acked_id.has_value());
        REQUIRE(*acked_id == message_id);
        REQUIRE(acked_event == "acked");
    }

    // 拆除（关闭序：NodeSession ×2 → server → 借用 runtime → owner）。
    for (auto* side : {&*side_a, &*side_b}) {
        const auto node_report = side->shutdown();
        REQUIRE(node_report.node_stopped);
        REQUIRE(node_report.runtime_stopped);
        REQUIRE_FALSE(node_report.runtime_executor_shutdown_performed);
    }
    const auto server_report = server.shutdown();
    if (!server_report.stopped || server_report.timed_out) {
        print_server_snapshot(server_report.final_snapshot);
    }
    REQUIRE(server_report.stopped);
    REQUIRE_FALSE(server_report.timed_out);
    REQUIRE(server_report.final_snapshot.state == heyaki::RelayServerState::stopped);

    const auto relay_runtime_report = relay_runtime->shutdown();
    REQUIRE_FALSE(relay_runtime_report.executor_shutdown_performed);  // 借用语义
    REQUIRE(relay_runtime_report.final_phase == heyaki::RuntimePhase::stopped);

    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());

    std::filesystem::remove_all(relay_root, ec);
    std::filesystem::remove_all(root_a, ec);
    std::filesystem::remove_all(root_b, ec);
}
