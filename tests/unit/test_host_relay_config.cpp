// M7（DEC-028）：HostRuntime relay/TURN 配置装配面测试（独立二进制）。
//
// HostRuntime 为进程级单例，一次装配↔关闭周期（test_host_runtime 同款纪律）
// ——本二进制用**预写 ice-servers.txt 的数据根**装配，覆盖 test_host_runtime
//（无 ice 文件 → 0/0）无法覆盖的分支：
//   - 装配报告：1 合法行 + 1 非法行 → ice_servers_configured==1、
//     ice_invalid_lines==1（非法行跳过计数不阻断装配）；
//   - turn_server() 预填视图：装配期解析的首条 turn_udp（credential 不回填
//     ——视图无该字段）；
//   - enroll_relay 异步路径：不可达 relay（wss://127.0.0.1:1，回环连接拒绝）
//     → submit 成功、失败经 SetRelayStatus 回传（enrolled=false +
//     last_error 非空）、profile 不落任何 enrollment 记录；
//   - set_turn_server 整文件覆写既有 ice-servers.txt；
//   - 受控关闭写路径零失败。
//
// M8-08（HEY-20261006-001 收口）增补：注册成功热连接 + 移除热断开的 Host 级
// 验证——经 HostRuntime::executor()（公开宿主生命周期路径，DOD-02）在同一
// 宿主 executor 上起进程内 token 模式 RelayServer（借用 Runtime + 测试证书
// + bootstrap token 种入，fixture 同型 test_relay_password_e2e_loopback），
// host.enroll_relay 成功后装配期已存在的 NodeSession 热连接（RelayStatus 经
// enrollment 任务 + 5s 连接态 sweep 收敛 ready），host.remove_relay 热断开
//（sweep 收敛 disabled + 服务器侧 active_sessions 归零）。NodeSession 层的
// 逐字段/关闭序证据由 test_relay_password_e2e_loopback 三个热生效用例承载
//（本用例验证 Host 编排面：任务接线、状态投影、单 executor 拓扑共存）。
//
// 进程唯一 Executor owner 是 HostRuntime 单例（AGENTS 规则 7/8；
// test_host_runtime 同款 main 模式）；测试线程只做有界轮询（yield），
// 不创建线程。
#include "app/lifecycle/host_runtime.hpp"
#include "heyaki/adapter/local_identity.hpp"
#include "heyaki/session/relay_enrollment.hpp"

#include <heyaki/runtime.hpp>

// 进程内 relay fixture（heyaki::relay 内部头，PUBLIC 传播 heyaki::client；
// 先例 tests/integration/test_relay_password_e2e_loopback.cpp）。
#include "relay_database.hpp"
#include "relay_server.hpp"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

using aki::app::HostRuntime;

bool wait_until(const std::function<bool()>& predicate,
    std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

std::filesystem::path make_temp_data_root() {
    static int counter = 0;
    const auto now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    auto root = std::filesystem::temp_directory_path()
        / ("aki-host-relay-config-" + std::to_string(now_ns) + "-"
            + std::to_string(++counter));
    std::filesystem::create_directories(root);
    return root;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// ---- 进程内 token 模式 relay fixture（M8-08 Host 级热生效验证用）----
// test_relay_password_e2e_loopback 同型自签证书（EC prime256v1、随机序列号、
// SAN = IP:127.0.0.1——ca_file 交换期主机名校验面）与 token 种入。
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
            reinterpret_cast<const unsigned char*>("heyaki-relay-host-e2e"), -1, -1, 0) == 1 &&
        X509_set_issuer_name(certificate, name) == 1;
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

// 宿主 executor 上的进程内 token 模式 relay（RAII 承载资源；skip_reason
// 非空表示环境性失败，调用方按既有纪律受控降级）。
struct HostRelayFixture {
    std::filesystem::path root;
    std::unique_ptr<heyaki::Runtime> relay_runtime;
    std::unique_ptr<heyaki::RelayServer> server;
    std::string tenant = "aki-host-e2e";
    std::string bootstrap_token;
    std::string relay_url;
    std::string skip_reason;

    ~HostRelayFixture() {
        if (server != nullptr) {
            (void)server->shutdown();
        }
        if (relay_runtime != nullptr) {
            (void)relay_runtime->shutdown();
        }
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    bool setup(kairo::Executor& host_executor) {
        root = make_temp_data_root();
        if (!write_test_certificate(root)) {
            skip_reason = "OpenSSL certificate generation unavailable";
            return false;
        }
        std::array<unsigned char, 16U> raw{};
        if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) {
            skip_reason = "bootstrap token generation failed";
            return false;
        }
        static constexpr char hex[] = "0123456789abcdef";
        bootstrap_token.reserve(raw.size() * 2U);
        for (const auto byte : raw) {
            bootstrap_token.push_back(hex[byte >> 4U]);
            bootstrap_token.push_back(hex[byte & 0x0fU]);
        }
        auto database = heyaki::RelayDatabase::open(root / "relay.sqlite");
        if (!database) {
            skip_reason = "relay database open failed";
            return false;
        }
        const auto expires = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch() + 10min)
                .count());
        auto seeded = database.value_if()->create_bootstrap_token(
            tenant, bootstrap_token, expires, 5U);
        if (!seeded) {
            skip_reason = "bootstrap token seeding failed";
            return false;
        }
        heyaki::RuntimeConfig runtime_config;
        // blocking worker 名与宿主 NodeSession（heyaki-asio）/注册句柄
        //（aki-relay-enroll）互异。
        runtime_config.worker_name = "aki-relay-host-e2e";
        auto runtime_result =
            heyaki::Runtime::create_borrowed(host_executor, runtime_config);
        if (!runtime_result) {
            skip_reason = "borrowed relay runtime create failed: "
                + std::string(runtime_result.error_if()->safe_detail());
            return false;
        }
        relay_runtime = std::make_unique<heyaki::Runtime>(
            std::move(*runtime_result.value_if()));
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
        config.runtime.worker_name = "aki-relay-host-e2e";
        config.enrollment_mode = heyaki::RelayEnrollmentMode::token;
        auto server_result =
            heyaki::RelayServer::create(std::move(config), relay_runtime.get());
        if (!server_result) {
            skip_reason = "relay server create failed: "
                + std::string(server_result.error_if()->safe_detail());
            return false;
        }
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
};

}  // namespace

TEST_CASE("HostRuntime assembles ice config and surfaces relay enrollment state",
    "[unit][host_runtime][m7_relay]") {
    const std::filesystem::path data_root = make_temp_data_root();
    // 预写 ICE 配置：1 合法 turn_udp 行 + 1 非法行（+ 注释）。
    {
        std::ofstream out(data_root / "ice-servers.txt",
            std::ios::binary | std::ios::trunc);
        out << "# aki ice servers\n";
        out << "turn_udp turn.example.org 3478 cfg-user cfg-cred\n";
        out << "bogus line\n";
    }

    HostRuntime& host = HostRuntime::instance();
    const auto& assembly = host.ensure_assembled(
        data_root.string(), {}, "test-local-password");
    REQUIRE(host.assembled());
    REQUIRE(assembly.ok);
    REQUIRE(assembly.failure_reason.empty());

    // ---- E2：装配报告新字段（1 合法 + 1 非法 → 1/1）----
    REQUIRE(assembly.ice_servers_configured == 1);
    REQUIRE(assembly.ice_invalid_lines == 1);

    // turn_server() 预填视图：首条 turn_udp；credential 不回填（视图无字段）。
    const auto turn_view = host.turn_server();
    REQUIRE(turn_view.host == "turn.example.org");
    REQUIRE(turn_view.port == 3478);
    REQUIRE(turn_view.username == "cfg-user");

    // 装配首推 relay 状态：无 enrollment 记录 → enrolled=false 基线。
    //（首推经 owner drain 落快照——quiesce = 组合根既有的首帧推进路径。）
    host.quiesce();
    aki::app::AppState snapshot;
    REQUIRE(host.load_state_snapshot(snapshot));
    REQUIRE(snapshot.relay.has_value());
    REQUIRE_FALSE(snapshot.relay->enrolled);

    // ---- 无记录 remove_relay 拒绝 ----
    std::string error;
    REQUIRE_FALSE(host.remove_relay(error));
    REQUIRE_FALSE(error.empty());

    // ---- enroll_relay 静态校验失败（同步 false + error）----
    error.clear();
    REQUIRE_FALSE(host.enroll_relay(
        "http://relay.example.com", "aki", "token", "", error));
    REQUIRE_FALSE(error.empty());

    // ---- enroll_relay 异步失败路径：不可达 relay（127.0.0.1:1）----
    // submit 面成功（admission）；失败经 SetRelayStatus 回传——有界轮询
    // 快照（pump_state = owner 上下文 drain），WSS transport 超时上界
    // ~12s（connect 5s + handshake 5s + close 2s），预算 30s。
    error.clear();
    REQUIRE(host.enroll_relay(
        "wss://127.0.0.1:1", "aki-test", "bootstrap-token", "", error));
    REQUIRE(error.empty());
    bool failure_visible = false;
    std::string observed_error;
    REQUIRE(wait_until([&] {
        host.pump_state();
        aki::app::AppState current;
        if (!host.load_state_snapshot(current)) {
            return false;
        }
        if (current.relay.has_value() && !current.relay->enrolled
            && !current.relay->last_error.empty()
            && current.relay->relay_url == "wss://127.0.0.1:1") {
            failure_visible = true;
            observed_error = current.relay->last_error;
            return true;
        }
        return false;
    }, 30s));
    REQUIRE(failure_visible);
    REQUIRE_FALSE(observed_error.empty());

    // 失败不落记录：profile 无任何 enrollment（重开只读视角验证持久面）。
    {
        auto profile = aki::heyaki::LocalProfile::open(data_root.string());
        REQUIRE(aki::heyaki::relay_enrollment_views(profile).empty());
    }

    // ---- set_turn_server 覆写既有 ice-servers.txt（异步整文件覆写）----
    error.clear();
    REQUIRE(host.set_turn_server(
        "turn2.example.org", 3479, "u2", "c2", error));
    REQUIRE(error.empty());
    std::string written;
    REQUIRE(wait_until([&] {
        written = read_file(data_root / "ice-servers.txt");
        return written == "turn_udp turn2.example.org 3479 u2 c2\n";
    }, 5s));
    REQUIRE(written == "turn_udp turn2.example.org 3479 u2 c2\n");
    // 装配期预填视图不被运行期覆写热更（重启生效语义）。
    const auto turn_view_after = host.turn_server();
    REQUIRE(turn_view_after.host == "turn.example.org");
    REQUIRE(turn_view_after.port == 3478);

    // ---- M8-08：注册成功热连接 + 移除热断开（Host 级，进程内 token 模式
    // relay 于宿主 executor）----
    // 前序异步失败用例已证明 profile 无记录；此处装配期已存在的
    // NodeSession（fresh 数据根）处于 relay disabled——注册成功后同会话
    // 热连接（无重启），移除后热断开。状态投影链：enrollment 任务
    // SetRelayStatus（enrolled）+ 5s 连接态 sweep（connection_state_name）。
    HostRelayFixture relay;
    if (!relay.setup(host.executor())) {
        std::printf("[skip] %s; Host-level hot-connect not verified in this "
                    "environment\n",
            relay.skip_reason.c_str());
    } else {
        const auto enroll_started = std::chrono::steady_clock::now();
        error.clear();
        REQUIRE(host.enroll_relay(relay.relay_url, relay.tenant,
            relay.bootstrap_token, (relay.root / "test-only-cert.pem").string(),
            error));
        REQUIRE(error.empty());
        // enrolled=true（任务面）+ ready（sweep 面，≤5s 周期 + 余量）。
        std::string observed_state;
        bool hot_connected = wait_until([&] {
            host.pump_state();
            aki::app::AppState current;
            if (!host.load_state_snapshot(current)
                || !current.relay.has_value()) {
                return false;
            }
            observed_state = current.relay->connection_state_name;
            return current.relay->enrolled
                && current.relay->connection_state_name == "ready"
                && current.relay->relay_url == relay.relay_url
                && current.relay->tenant == relay.tenant
                && current.relay->last_error.empty();
        }, 30s);
        if (!hot_connected) {
            std::printf("    [diag] host hot-connect did not converge: "
                        "state=%s\n",
                observed_state.c_str());
            const auto server_snapshot = relay.server->snapshot();
            std::printf(
                "    [diag] relay server: state=%d port=%u active=%llu "
                "enroll_ok=%llu login_ok=%llu\n",
                static_cast<int>(server_snapshot.state),
                static_cast<unsigned>(server_snapshot.listen_port),
                static_cast<unsigned long long>(
                    server_snapshot.active_sessions),
                static_cast<unsigned long long>(
                    server_snapshot.enrollments_completed),
                static_cast<unsigned long long>(
                    server_snapshot.logins_completed));
        }
        REQUIRE(hot_connected);
        // 服务器侧证据：真实登录会话在线（enroll_ok 来自注册交换）。
        REQUIRE(wait_until([&] {
            return relay.server->snapshot().active_sessions >= 1U;
        }, 10s));
        REQUIRE(wait_until([&] {
            return relay.server->snapshot().logins_completed >= 1U;
        }, 10s));
        const auto connect_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - enroll_started);
        std::printf("    [diag] host hot connect: enroll->ready %lldms "
                    "(no restart; state=%s)\n",
            static_cast<long long>(connect_elapsed.count()),
            observed_state.c_str());

        // 移除热断开：撤销持久化 + 同会话断开；enrolled=false 同步可见，
        // 连接态经 sweep（≤5s）收敛 disabled。
        error.clear();
        REQUIRE(host.remove_relay(error));
        REQUIRE(error.empty());  // 热断开失败会以 "relay hot-disconnect ..." 进 error
        std::string disconnect_state;
        bool hot_disconnected = wait_until([&] {
            host.pump_state();
            aki::app::AppState current;
            if (!host.load_state_snapshot(current)
                || !current.relay.has_value()) {
                return false;
            }
            disconnect_state = current.relay->connection_state_name;
            return !current.relay->enrolled
                && current.relay->connection_state_name == "disabled"
                && current.relay->last_error.empty();
        }, 15s);
        if (!hot_disconnected) {
            std::printf("    [diag] host hot-disconnect did not converge: "
                        "state=%s\n",
                disconnect_state.c_str());
        }
        REQUIRE(hot_disconnected);
        // 记录面：唯一记录已撤销（重启后不再自动连接）。
        {
            auto profile = aki::heyaki::LocalProfile::open(data_root.string());
            const auto views = aki::heyaki::relay_enrollment_views(profile);
            REQUIRE(views.size() == 1U);
            REQUIRE(views.front().relay_url == relay.relay_url);
            REQUIRE(views.front().revoked);
        }
        // 服务器侧证据：控制面拆除（active_sessions 归零）。
        REQUIRE(wait_until([&] {
            return relay.server->snapshot().active_sessions == 0U;
        }, 15s));
        std::printf("    [diag] host hot disconnect: state=%s, server "
                    "active_sessions=0\n",
            disconnect_state.c_str());

        // relay fixture 受控回收（先于宿主关闭；借用语义不收官宿主
        // executor——与宿主关闭序内 Runtime 报告同一断言面）。
        const auto server_report = relay.server->shutdown();
        REQUIRE(server_report.stopped);
        REQUIRE_FALSE(server_report.timed_out);
        const auto relay_runtime_report = relay.relay_runtime->shutdown();
        REQUIRE_FALSE(relay_runtime_report.executor_shutdown_performed);
        REQUIRE(relay_runtime_report.final_phase
            == heyaki::RuntimePhase::stopped);
    }

    // ---- 受控关闭（relay 任务 future 有界消费 + 写路径零失败）----
    const auto& closed = host.shutdown_with_report();
    REQUIRE(closed.attempted);
    REQUIRE(closed.hook_sequence_completed);
    // M8-04 复验面：注册任务（launch_relay_enroll 传宿主 executor 借用
    // Runtime）之后 Node 会话仍须可停（关闭路径闭合，AGENTS 完成定义）。
    REQUIRE(closed.node_stopped);
    REQUIRE(closed.runtime_stopped);
    REQUIRE(closed.write_settle_failures == 0);
    REQUIRE(closed.write_enqueue_rejected == 0);
    REQUIRE(closed.db_failed == 0);
    REQUIRE(closed.db_rejected == 0);
    REQUIRE_FALSE(closed.borrowed_runtime_shutdown_performed);  // DEC-006

    // 清理（成功路径；失败保留诊断）。
    std::error_code ec;
    std::filesystem::remove_all(data_root, ec);
}

int main(int argc, char* argv[]) {
    // 本进程唯一 Executor owner 是 HostRuntime 单例（AGENTS 规则 7/8；
    // test_host_runtime 同款模式：函数级 static 析构在 main 返回后执行，
    // 受控关闭已在用例内完成，析构为已停状态的空 teardown）。
    return Catch::Session().run(argc, argv);
}
