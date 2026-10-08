// M7（DEC-028）：relay 跨网段通信的纯函数与 profile 面单元测试（网络无关：
// 不对真实 relay 地址发起注册——异步失败路径只打 127.0.0.1 不可达端口）。
//
// 覆盖：
//   - diff_lan_discovery 的来源映射（M7/DEC-028 决策 5）：仅 relay 可见条目 →
//     DiscoveryMethod::Relay + "relay:" 前缀；LAN+relay 双可见 → LanDiscovery
//     优先 + "lan:" 前缀；trusted/非 32 字节公钥/消失回落语义不变；
//   - validate_relay_enroll_request 静态校验各拒绝分支与合法分支
//     （relay_enrollment.hpp，含 ca_file 存在性）；
//   - M8-04（DEC-028 决策 11 阶段 2）：凭据二选一契约（双空/双非空拒绝、
//     password ≤256B、password 模式空租户归一 "default"、token 模式租户
//     仍必填）；
//   - enroll_relay_profile 对不可达地址失败可见且 profile 不落记录
//     （阻塞调用，超时上界由上游 transport 配置约束，见头注）；
//   - enroll_relay_profile 全部失败退出路径原位擦除调用方持有的
//     bootstrap_token 与 enrollment_password（M8-02/DEC-018：校验拒绝、
//     非 PEM pin 失败、不可达交换失败，兼回归 nullopt + error 非空 +
//     不落记录；M8-04 密码镜像 + 双凭据拒绝路径两凭据均擦除）；
//   - revoke_relay_enrollment：无记录 false；put 造记录后撤销成功、视图
//     revoked 可见（relay_enrollment_views 投影）；
//   - ICE 配置（ice_config.hpp）：parse 各合法/非法行、invalid_lines 计数、
//     注释/空行/CRLF 容错、serialize→parse round-trip、validate_ice_server
//     全拒绝分支、load_ice_servers_file 文件缺失 = 空配置。
//
// 本二进制无 Executor owner 需求（全部被测面为纯函数 / 主线程阻塞调用）；
// Catch2 WithMain 提供入口（test_peer_sessions_pipeline 同款链接形态）。
#include "heyaki/adapter/lan_discovery.hpp"
#include "heyaki/adapter/local_identity.hpp"
#include "heyaki/session/ice_config.hpp"
#include "heyaki/session/relay_enrollment.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using aki::heyaki::EndpointView;
using aki::heyaki::IceServerConfig;
using aki::heyaki::LanDiscoveryState;
using aki::heyaki::RelayEnrollRequest;

int g_counter = 0;

std::filesystem::path make_temp_root(const std::string& tag) {
    auto root = std::filesystem::temp_directory_path()
        / ("aki-relay-integration-" + tag + "-"
            + std::to_string(std::chrono::steady_clock::now()
                                 .time_since_epoch()
                                 .count())
            + "-" + std::to_string(++g_counter));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

EndpointView make_entry(std::string id, bool trusted, bool lan_visible,
    bool relay_visible, std::size_t key_bytes = 32) {
    EndpointView entry;
    entry.device_id = aki::device::DeviceId{std::move(id)};
    entry.public_key.bytes.assign(key_bytes, std::uint8_t{0xAB});
    entry.endpoint_id = "hye1_" + entry.device_id.value;
    entry.trusted = trusted;
    entry.lan_visible = lan_visible;
    entry.relay_visible = relay_visible;
    return entry;
}

bool same_server(const IceServerConfig& left, const IceServerConfig& right) {
    return left.kind == right.kind && left.hostname == right.hostname
        && left.port == right.port && left.username == right.username
        && left.credential == right.credential;
}

}  // namespace

// ---- A 组：diff_lan_discovery 的 relay 来源映射（M7/DEC-028 决策 5）----

TEST_CASE("Relay-only catalog entries map to the Relay discovery source",
    "[unit][relay_integration][m7_relay]") {
    LanDiscoveryState state;
    auto tick = aki::heyaki::diff_lan_discovery(
        std::vector<EndpointView>{
            make_entry("relay-dev", false, false, true)},
        state);

    REQUIRE(tick.discovered.size() == 1);
    REQUIRE(tick.went_offline.empty());
    const auto& device = tick.discovered.front();
    REQUIRE(device.identity.id == aki::device::DeviceId{"relay-dev"});
    REQUIRE(device.identity.public_key.bytes.size() == 32);
    REQUIRE(device.identity.trust_state == aki::device::TrustState::Unknown);
    // relay 目录条目同样是「对端正在运行 Aki」的存活证明（记录签名 + 租约）。
    REQUIRE(device.identity.presence == aki::device::PresenceState::Online);
    REQUIRE(device.method == aki::device::DiscoveryMethod::Relay);
    REQUIRE(device.endpoint.description.starts_with("relay:"));
    REQUIRE(device.endpoint.description
        == "relay:hye1_relay-dev");
    // state 推进：进入 seen 与 live（与 LAN 条目同一 diff 语义）。
    REQUIRE(state.seen.count("relay-dev") == 1);
    REQUIRE(state.live.count("relay-dev") == 1);
}

TEST_CASE("Dual-visible entries keep LanDiscovery precedence over relay",
    "[unit][relay_integration][m7_relay]") {
    LanDiscoveryState state;
    auto tick = aki::heyaki::diff_lan_discovery(
        std::vector<EndpointView>{
            make_entry("both-dev", false, true, true)},
        state);

    REQUIRE(tick.discovered.size() == 1);
    const auto& device = tick.discovered.front();
    // LAN 直连语义优先（lan_discovery.hpp 注释：LAN 可见（含双可见）→ lan:）。
    REQUIRE(device.method == aki::device::DiscoveryMethod::LanDiscovery);
    REQUIRE(device.endpoint.description.starts_with("lan:"));
    REQUIRE(device.endpoint.description == "lan:hye1_both-dev");
    REQUIRE(device.identity.presence == aki::device::PresenceState::Online);
}

TEST_CASE("Relay-only entries without a 32-byte identity key are skipped",
    "[unit][relay_integration][m7_relay]") {
    LanDiscoveryState state;
    auto tick = aki::heyaki::diff_lan_discovery(
        std::vector<EndpointView>{
            make_entry("short-key", false, false, true, 31),
            make_entry("long-key", false, false, true, 33)},
        state);

    // 非 Aki 广播语义保持：指纹面缺失不可确认，跳过且不入 live。
    REQUIRE(tick.discovered.empty());
    REQUIRE(tick.went_offline.empty());
    REQUIRE(state.seen.empty());
    REQUIRE(state.live.empty());
}

TEST_CASE("Vanished relay-only entries fall back to went_offline",
    "[unit][relay_integration][m7_relay]") {
    LanDiscoveryState state;
    auto tick = aki::heyaki::diff_lan_discovery(
        std::vector<EndpointView>{
            make_entry("relay-dev", false, false, true)},
        state);
    REQUIRE(tick.discovered.size() == 1);

    // relay 条目从目录消失（租约过期/对端退出）→ 离线回落。
    auto next = aki::heyaki::diff_lan_discovery(
        std::vector<EndpointView>{
            make_entry("other-dev", false, false, true)},
        state);
    REQUIRE(next.discovered.size() == 1);  // other-dev 首次发现。
    REQUIRE(next.went_offline.size() == 1);
    REQUIRE(next.went_offline.front()
        == aki::device::DeviceId{"relay-dev"});
    REQUIRE(state.live.count("relay-dev") == 0);
}

TEST_CASE("Trusted relay-only entries neither replay nor misreport offline",
    "[unit][relay_integration][m7_relay]") {
    LanDiscoveryState state;
    // tick1：未信任 relay 条目 → discovered，进入 live。
    auto tick1 = aki::heyaki::diff_lan_discovery(
        std::vector<EndpointView>{
            make_entry("relay-dev", false, false, true)},
        state);
    REQUIRE(tick1.discovered.size() == 1);

    // tick2：毕业为信任（配对完成瞬间）——不重放 discovered、不误报
    // went_offline（信任转移不是离线，M5-11 修订对 relay 条目同样成立）。
    auto tick2 = aki::heyaki::diff_lan_discovery(
        std::vector<EndpointView>{
            make_entry("relay-dev", true, false, true)},
        state);
    REQUIRE(tick2.discovered.empty());
    REQUIRE(tick2.went_offline.empty());
    REQUIRE(state.live.count("relay-dev") == 0);

    // tick3：条目消失——live 已在 tick2 移除，不补发二次离线。
    auto tick3 = aki::heyaki::diff_lan_discovery({}, state);
    REQUIRE(tick3.discovered.empty());
    REQUIRE(tick3.went_offline.empty());

    // 首轮即 trusted 的 relay 条目：不合成 discovered、不入 live/seen。
    LanDiscoveryState fresh;
    auto trusted_first = aki::heyaki::diff_lan_discovery(
        std::vector<EndpointView>{
            make_entry("trusted-relay", true, false, true)},
        fresh);
    REQUIRE(trusted_first.discovered.empty());
    REQUIRE(trusted_first.went_offline.empty());
    REQUIRE(fresh.seen.empty());
    REQUIRE(fresh.live.empty());
}

TEST_CASE("Relay entry idempotence follows the seen-set across ticks",
    "[unit][relay_integration][m7_relay]") {
    LanDiscoveryState state;
    auto tick = aki::heyaki::diff_lan_discovery(
        std::vector<EndpointView>{
            make_entry("relay-dev", false, false, true)},
        state);
    REQUIRE(tick.discovered.size() == 1);

    // 跨 tick 仍在目录：不重复合成、不产 went_offline（幂等）。
    auto next = aki::heyaki::diff_lan_discovery(
        std::vector<EndpointView>{
            make_entry("relay-dev", false, false, true)},
        state);
    REQUIRE(next.discovered.empty());
    REQUIRE(next.went_offline.empty());
    REQUIRE(state.seen.count("relay-dev") == 1);
    REQUIRE(state.live.count("relay-dev") == 1);
}

// ---- B 组：validate_relay_enroll_request 静态校验 ----

TEST_CASE("validate_relay_enroll_request rejects malformed requests",
    "[unit][relay_integration][m7_relay]") {
    auto invalid = [](RelayEnrollRequest request) {
        return aki::heyaki::validate_relay_enroll_request(request);
    };
    RelayEnrollRequest base;
    base.relay_url = "wss://relay.example.com/relay";
    base.tenant = "aki";
    base.bootstrap_token = "bootstrap-token";

    // 非 wss:// 前缀。
    SECTION("http scheme") {
        auto request = base;
        request.relay_url = "http://relay.example.com";
        const auto error = invalid(request);
        REQUIRE(error.has_value());
        REQUIRE_FALSE(error->empty());
    }
    SECTION("ws scheme") {
        auto request = base;
        request.relay_url = "ws://relay.example.com";
        REQUIRE(invalid(request).has_value());
    }
    SECTION("empty url") {
        auto request = base;
        request.relay_url.clear();
        REQUIRE(invalid(request).has_value());
    }
    // 仅 "wss://" 前缀本身（主机段为空）。
    SECTION("bare wss prefix") {
        auto request = base;
        request.relay_url = "wss://";
        REQUIRE(invalid(request).has_value());
    }
    SECTION("empty tenant") {
        auto request = base;
        request.tenant.clear();
        REQUIRE(invalid(request).has_value());
    }
    SECTION("empty token") {
        auto request = base;
        request.bootstrap_token.clear();
        REQUIRE(invalid(request).has_value());
    }
    SECTION("missing ca file") {
        auto request = base;
        request.ca_file = std::filesystem::path{
            "/nonexistent/aki-relay-ca.pem"};
        const auto error = invalid(request);
        REQUIRE(error.has_value());
        REQUIRE_FALSE(error->empty());
    }
}

TEST_CASE("validate_relay_enroll_request accepts valid requests",
    "[unit][relay_integration][m7_relay]") {
    RelayEnrollRequest request;
    request.relay_url = "wss://relay.example.com/relay";
    request.tenant = "aki";
    request.bootstrap_token = "bootstrap-token";
    REQUIRE_FALSE(
        aki::heyaki::validate_relay_enroll_request(request).has_value());

    // ca_file 指向真实文件（自签部署形态）。
    const auto root = make_temp_root("ca-ok");
    const auto ca = root / "relay-ca.pem";
    {
        std::ofstream out(ca, std::ios::binary);
        out << "-----BEGIN CERTIFICATE-----\n";
    }
    request.ca_file = ca;
    REQUIRE_FALSE(
        aki::heyaki::validate_relay_enroll_request(request).has_value());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// M8-04（DEC-028 决策 11 阶段 2）：凭据二选一契约。token 与
// enrollment_password 恰好一个在场；password 模式空租户由 validate 非 const
// 引用原位归一为 "default"（与 relay enrollment_default_tenant 缺省对齐），
// token 模式租户仍必填；密码 ≤256B（kRelayEnrollmentPasswordMaxBytes）。
TEST_CASE(
    "validate_relay_enroll_request enforces exactly-one credential and"
    " password bounds",
    "[unit][relay_integration][m8_relay]") {
    // 双凭据同时为空：拒绝（无任何准入凭据）。
    SECTION("no credential at all is rejected") {
        RelayEnrollRequest request;
        request.relay_url = "wss://relay.example.com/relay";
        request.tenant = "aki";
        const auto error =
            aki::heyaki::validate_relay_enroll_request(request);
        REQUIRE(error.has_value());
        REQUIRE_FALSE(error->empty());
        // 双空路径租户未被归一（拒绝先于归一，无副作用推进）。
        REQUIRE(request.tenant == "aki");
    }
    // 双凭据同时非空：拒绝（XOR 契约——密码模式不得携带 token）。
    SECTION("both credentials present is rejected") {
        RelayEnrollRequest request;
        request.relay_url = "wss://relay.example.com/relay";
        request.tenant = "aki";
        request.bootstrap_token = "bootstrap-token";
        request.enrollment_password = "owner-password";
        REQUIRE(aki::heyaki::validate_relay_enroll_request(request)
            .has_value());
    }
    // password 超上限（256B）：拒绝且文案可展示（含上限值）。
    SECTION("oversized enrollment password is rejected") {
        RelayEnrollRequest request;
        request.relay_url = "wss://relay.example.com/relay";
        request.enrollment_password.assign(
            aki::heyaki::kRelayEnrollmentPasswordMaxBytes + 1U, 'x');
        const auto error =
            aki::heyaki::validate_relay_enroll_request(request);
        REQUIRE(error.has_value());
        REQUIRE(error->find("256") != std::string::npos);
    }
    // password 恰在上限（256B）：合法；空租户被原位归一为 "default"。
    SECTION("password at the bound defaults an empty tenant") {
        RelayEnrollRequest request;
        request.relay_url = "wss://relay.example.com/relay";
        request.enrollment_password.assign(
            aki::heyaki::kRelayEnrollmentPasswordMaxBytes, 'x');
        REQUIRE_FALSE(
            aki::heyaki::validate_relay_enroll_request(request).has_value());
        REQUIRE(request.tenant
            == std::string(aki::heyaki::kRelayPasswordDefaultTenant));
        REQUIRE(request.tenant == "default");
    }
    // password 模式显式租户：保留不覆盖（自定义默认租户部署经显式输入）。
    SECTION("password mode keeps an explicit tenant") {
        RelayEnrollRequest request;
        request.relay_url = "wss://relay.example.com/relay";
        request.tenant = "custom-tenant";
        request.enrollment_password = "owner-password";
        REQUIRE_FALSE(
            aki::heyaki::validate_relay_enroll_request(request).has_value());
        REQUIRE(request.tenant == "custom-tenant");
    }
    // token 模式空租户：仍拒绝（既有语义回归）。
    SECTION("token mode still requires a tenant") {
        RelayEnrollRequest request;
        request.relay_url = "wss://relay.example.com/relay";
        request.bootstrap_token = "bootstrap-token";
        const auto error =
            aki::heyaki::validate_relay_enroll_request(request);
        REQUIRE(error.has_value());
        REQUIRE_FALSE(error->empty());
        REQUIRE(request.tenant.empty());  // token 模式不做租户归一
    }
}

TEST_CASE("enroll_relay_profile fails visibly against an unreachable relay",
    "[unit][relay_integration][m7_relay]") {
    const auto root = make_temp_root("enroll-unreachable");
    auto profile = aki::heyaki::LocalProfile::open(root.string(),
        "test-local-password");

    RelayEnrollRequest request;
    // 127.0.0.1:1 —— 本机不可达端口（连接拒绝即时失败；transport 超时上界
    // connect 5s + handshake 5s + close 2s，见 relay_enrollment.hpp 头注）。
    request.relay_url = "wss://127.0.0.1:1";
    request.tenant = "aki-test";
    request.bootstrap_token = "bootstrap-token";

    std::string error;
    const auto result =
        aki::heyaki::enroll_relay_profile(profile, request, error);
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(error.empty());
    // 失败不落记录：profile 无任何 enrollment（RULE-09：拒绝面不产生副作用）。
    REQUIRE(aki::heyaki::relay_enrollment_views(profile).empty());

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// M8-02（DEC-028 决策 11 阶段 1 / DEC-018 凭据纪律）：enroll_relay_profile
// 以非 const 引用收 request，在全部失败退出路径原位擦除调用方持有的
// bootstrap_token（成功路径的真实注册流证据见 test_relay_e2e_loopback）。
// 网络无关纪律沿文件头：仅静态校验拒绝与 127.0.0.1 不可达端口。
TEST_CASE("enroll_relay_profile scrubs the bootstrap token on every failure path",
    "[unit][relay_integration][m8_relay]") {
    const auto root = make_temp_root("enroll-scrub");
    auto profile = aki::heyaki::LocalProfile::open(root.string(),
        "test-local-password");

    // 各失败退出路径的公共断言：失败可见（nullopt + error 非空）、无记录
    // 落库（RULE-09 副作用面）、调用方 request.bootstrap_token 原位为空。
    const auto expect_failed_and_scrubbed =
        [&profile](RelayEnrollRequest& request, std::string& error) {
            REQUIRE_FALSE(request.bootstrap_token.empty());  // 前置：凭据在场
            const auto result =
                aki::heyaki::enroll_relay_profile(profile, request, error);
            REQUIRE_FALSE(result.has_value());
            REQUIRE_FALSE(error.empty());
            REQUIRE(request.bootstrap_token.empty());
            REQUIRE(aki::heyaki::relay_enrollment_views(profile).empty());
        };

    RelayEnrollRequest base;
    base.relay_url = "wss://relay.example.com/relay";
    base.tenant = "aki";
    base.bootstrap_token = "bootstrap-secret-token";

    // 静态校验拒绝：URL 非 wss://（validate_relay_enroll_request 早退）。
    SECTION("non-wss url is rejected and scrubbed") {
        auto request = base;
        request.relay_url = "http://relay.example.com";
        std::string error;
        expect_failed_and_scrubbed(request, error);
    }
    // 静态校验拒绝：tenant 为空。
    SECTION("empty tenant is rejected and scrubbed") {
        auto request = base;
        request.tenant.clear();
        std::string error;
        expect_failed_and_scrubbed(request, error);
    }
    // 校验拒绝：ca_file 指向不存在文件（存在性检查在 validate 面早退）。
    SECTION("missing ca file is rejected and scrubbed") {
        auto request = base;
        request.ca_file = std::filesystem::path{
            "/nonexistent/aki-relay-scrub-ca.pem"};
        std::string error;
        expect_failed_and_scrubbed(request, error);
    }
    // pin 失败：文件存在但非 PEM——validate 通过、relay_certificate_pin
    // 同步拒绝（构造临时文件的写法沿上方 ca-ok 用例）。
    SECTION("non-PEM ca file fails the pin and scrubs") {
        const auto pem_root = make_temp_root("enroll-scrub-notpem");
        const auto not_pem = pem_root / "not-a-cert.pem";
        {
            std::ofstream out(not_pem, std::ios::binary);
            out << "this is not a PEM certificate\n";
        }
        auto request = base;
        request.ca_file = not_pem;
        std::string error;
        expect_failed_and_scrubbed(request, error);
        std::error_code pem_ec;
        std::filesystem::remove_all(pem_root, pem_ec);
    }
    // 上游 WSS 交换失败：127.0.0.1:1 本机不可达端口（连接拒绝；阻塞上界
    // connect 5s + handshake 5s + close 2s，见 relay_enrollment.hpp 头注）。
    SECTION("unreachable relay exchange failure scrubs") {
        auto request = base;
        request.relay_url = "wss://127.0.0.1:1";
        std::string error;
        expect_failed_and_scrubbed(request, error);
    }

    // ---- M8-04（决策 11 阶段 2）密码镜像：同一组失败退出路径，凭据换为
    //      enrollment_password（空租户经 validate 归一，不构成拒绝）。----
    const auto expect_password_failed_and_scrubbed =
        [&profile](RelayEnrollRequest& request, std::string& error) {
            REQUIRE_FALSE(request.enrollment_password.empty());  // 前置
            REQUIRE(request.bootstrap_token.empty());
            const auto result =
                aki::heyaki::enroll_relay_profile(profile, request, error);
            REQUIRE_FALSE(result.has_value());
            REQUIRE_FALSE(error.empty());
            REQUIRE(request.enrollment_password.empty());
            REQUIRE(request.bootstrap_token.empty());
            REQUIRE(aki::heyaki::relay_enrollment_views(profile).empty());
        };

    RelayEnrollRequest password_base;
    password_base.relay_url = "wss://relay.example.com/relay";
    password_base.enrollment_password = "owner-enroll-secret";

    // 静态校验拒绝：URL 非 wss://。
    SECTION("password mode non-wss url is rejected and scrubbed") {
        auto request = password_base;
        request.relay_url = "http://relay.example.com";
        std::string error;
        expect_password_failed_and_scrubbed(request, error);
    }
    // 静态校验拒绝：password 超过 256B 上限（validate 面早退）。
    SECTION("oversized password is rejected and scrubbed") {
        auto request = password_base;
        request.enrollment_password.assign(
            aki::heyaki::kRelayEnrollmentPasswordMaxBytes + 1U, 'x');
        std::string error;
        expect_password_failed_and_scrubbed(request, error);
    }
    // 校验拒绝：ca_file 不存在。
    SECTION("password mode missing ca file is rejected and scrubbed") {
        auto request = password_base;
        request.ca_file = std::filesystem::path{
            "/nonexistent/aki-relay-scrub-ca.pem"};
        std::string error;
        expect_password_failed_and_scrubbed(request, error);
    }
    // pin 失败：文件存在但非 PEM（validate 通过、pin 同步拒绝）。
    SECTION("password mode non-PEM ca file fails the pin and scrubs") {
        const auto pem_root = make_temp_root("enroll-scrub-pw-notpem");
        const auto not_pem = pem_root / "not-a-cert.pem";
        {
            std::ofstream out(not_pem, std::ios::binary);
            out << "this is not a PEM certificate\n";
        }
        auto request = password_base;
        request.ca_file = not_pem;
        std::string error;
        expect_password_failed_and_scrubbed(request, error);
        std::error_code pem_ec;
        std::filesystem::remove_all(pem_root, pem_ec);
    }
    // 上游 WSS 交换失败：127.0.0.1:1 不可达（password 模式无 ca_file →
    // TOFU 首连 verify_peer=false，仍即时连接拒绝）。
    SECTION("password mode unreachable relay exchange failure scrubs") {
        auto request = password_base;
        request.relay_url = "wss://127.0.0.1:1";
        std::string error;
        expect_password_failed_and_scrubbed(request, error);
    }
    // 双凭据拒绝（XOR 违约）：两凭据都必须被原位擦除（拒绝路径同样不落库）。
    SECTION("dual-credential rejection scrubs both secrets") {
        auto request = password_base;
        request.tenant = "aki";
        request.bootstrap_token = "bootstrap-secret-token";
        std::string error;
        const auto result =
            aki::heyaki::enroll_relay_profile(profile, request, error);
        REQUIRE_FALSE(result.has_value());
        REQUIRE_FALSE(error.empty());
        REQUIRE(request.bootstrap_token.empty());
        REQUIRE(request.enrollment_password.empty());
        REQUIRE(aki::heyaki::relay_enrollment_views(profile).empty());
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("revoke_relay_enrollment requires a record and marks it revoked",
    "[unit][relay_integration][m7_relay]") {
    const auto root = make_temp_root("revoke");
    auto profile = aki::heyaki::LocalProfile::open(root.string(),
        "test-local-password");

    // 无记录：false 且 error 非空（无操作可见）。
    std::string error;
    REQUIRE_FALSE(aki::heyaki::revoke_relay_enrollment(
        profile, "wss://relay.example.com", error));
    REQUIRE_FALSE(error.empty());

    // put 造记录（上游公开 API）→ 视图可见未撤销。
    ::heyaki::RelayEnrollmentRecord record;
    record.relay_url = "wss://relay.example.com";
    record.tenant = "aki";
    record.enrollment_generation = 1U;
    record.auto_connect = true;
    record.revoked = false;
    const auto put = profile.store().put_relay_enrollment(record);
    REQUIRE(put.has_value());
    auto views = aki::heyaki::relay_enrollment_views(profile);
    REQUIRE(views.size() == 1);
    REQUIRE(views.front().relay_url == "wss://relay.example.com");
    REQUIRE(views.front().tenant == "aki");
    REQUIRE(views.front().auto_connect);
    REQUIRE_FALSE(views.front().revoked);
    REQUIRE(views.front().enrollment_generation == 1U);

    // 撤销成功；视图反映 revoked=true。
    error.clear();
    REQUIRE(aki::heyaki::revoke_relay_enrollment(
        profile, "wss://relay.example.com", error));
    REQUIRE(error.empty());
    views = aki::heyaki::relay_enrollment_views(profile);
    REQUIRE(views.size() == 1);
    REQUIRE(views.front().revoked);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// ---- C 组：ICE 配置解析/序列化/校验（ice_config.hpp）----

TEST_CASE("parse_ice_servers_content parses valid lines and tolerant noise",
    "[unit][relay_integration][m7_relay]") {
    // 单行合法 turn_udp（含凭据）。
    auto single = aki::heyaki::parse_ice_servers_content(
        "turn_udp turn.example.com 3478 aki-user aki-cred\n");
    REQUIRE(single.invalid_lines == 0);
    REQUIRE(single.servers.size() == 1);
    REQUIRE(single.servers.front().kind == "turn_udp");
    REQUIRE(single.servers.front().hostname == "turn.example.com");
    REQUIRE(single.servers.front().port == 3478);
    REQUIRE(single.servers.front().username == "aki-user");
    REQUIRE(single.servers.front().credential == "aki-cred");

    // 多行：stun 无凭据 + turn_udp；注释/空行/纯空白行跳过；行内注释截断。
    const std::string content =
        "# aki ice servers\n"
        "stun stun.example.com 3478\n"
        "\n"
        "   \t\n"
        "turn_udp turn.example.com 3478 aki-user aki-cred\n"
        "stun stun2.example.com 3479 # inline comment\n"
        "#trailing comment only\n";
    auto multi = aki::heyaki::parse_ice_servers_content(content);
    REQUIRE(multi.invalid_lines == 0);
    REQUIRE(multi.servers.size() == 3);
    REQUIRE(multi.servers[0].kind == "stun");
    REQUIRE(multi.servers[0].hostname == "stun.example.com");
    REQUIRE(multi.servers[0].port == 3478);
    REQUIRE(multi.servers[0].username.empty());
    REQUIRE(multi.servers[0].credential.empty());
    REQUIRE(multi.servers[1].kind == "turn_udp");
    REQUIRE(multi.servers[1].port == 3478);
    REQUIRE(multi.servers[1].username == "aki-user");
    REQUIRE(multi.servers[1].credential == "aki-cred");
    REQUIRE(multi.servers[2].hostname == "stun2.example.com");
    REQUIRE(multi.servers[2].port == 3479);

    // CRLF 行尾容错。
    auto crlf = aki::heyaki::parse_ice_servers_content(
        "turn_udp turn.example.com 3478 u c\r\nstun s.example 3479\r\n");
    REQUIRE(crlf.invalid_lines == 0);
    REQUIRE(crlf.servers.size() == 2);
    REQUIRE(crlf.servers.front().credential == "c");
    REQUIRE(crlf.servers.back().hostname == "s.example");

    // 边界端口：1 与 65535 合法。
    auto bounds = aki::heyaki::parse_ice_servers_content(
        "stun a.example 1\nstun b.example 65535\n");
    REQUIRE(bounds.invalid_lines == 0);
    REQUIRE(bounds.servers.size() == 2);
    REQUIRE(bounds.servers[0].port == 1);
    REQUIRE(bounds.servers[1].port == 65535);
}

TEST_CASE("parse_ice_servers_content counts invalid lines without aborting",
    "[unit][relay_integration][m7_relay]") {
    const std::string content =
        "turn_udp turn.example.com 0 u c\n"          // port 0 越界
        "turn_udp turn.example.com 65536 u c\n"       // port > 65535 越界
        "turn_udp turn.example.com 70000 u c\n"       // port 70000 越界
        "turn_tcp turn.example.com 3478 u c\n"        // kind 非法
        "carrier turn.example.com 3478\n"             // kind 非法
        "turn_udp turn.example.com 3478\n"            // turn_udp 缺凭据
        "turn_udp turn.example.com 3478 aki-user\n"   // 缺 credential
        "turn_udp turn.example.com\n"                 // 缺端口
        "turn_udp\n"                                  // 缺主机/端口
        "turn_udp tur n.example.com 3478 u c\n"       // hostname 含空白截断 → 字段数不符
        "stun\n";                                     // 缺主机/端口
    auto parsed = aki::heyaki::parse_ice_servers_content(content);
    REQUIRE(parsed.invalid_lines == 11);
    REQUIRE(parsed.servers.empty());

    // 非法行不中断：前后的合法行仍被解析。
    auto mixed = aki::heyaki::parse_ice_servers_content(
        "stun stun.example.com 3478\n"
        "bogus\n"
        "turn_udp turn.example.com 3478 u c\n");
    REQUIRE(mixed.invalid_lines == 1);
    REQUIRE(mixed.servers.size() == 2);
    REQUIRE(mixed.servers.front().hostname == "stun.example.com");
    REQUIRE(mixed.servers.back().kind == "turn_udp");
}

TEST_CASE("parse_ice_servers_content rejects non-numeric port tokens",
    "[unit][relay_integration][m7_relay]") {
    // 端口字段必须整体为纯数字：尾部垃圾 / 符号前缀 / 十六进制前缀 /
    // 非数字均应整行计入 invalid_lines，不得被 std::stoul 前缀解析静默
    // 接受为合法端口（修复复验：3478abc / +5 / 0x10 / -1）。
    SECTION("trailing garbage, signs and hex prefixes are invalid") {
        auto parsed = aki::heyaki::parse_ice_servers_content(
            "stun a.example 3478abc\n"
            "stun b.example +5\n"
            "stun c.example 0x10\n"
            "stun d.example -1\n"
            "stun e.example 0x1234\n"
            "stun f.example x3478\n");
        REQUIRE(parsed.servers.empty());
        REQUIRE(parsed.invalid_lines == 6);
    }
    // 纯数字（含前导零）仍合法——修复不收紧合法输入。
    SECTION("pure digit tokens including leading zeros stay valid") {
        auto parsed = aki::heyaki::parse_ice_servers_content(
            "stun a.example 3478\nstun b.example 0003478\n");
        REQUIRE(parsed.invalid_lines == 0);
        REQUIRE(parsed.servers.size() == 2);
        REQUIRE(parsed.servers.front().port == 3478);
        REQUIRE(parsed.servers.back().port == 3478);
    }
}

TEST_CASE("serialize_ice_servers round-trips through the parser",
    "[unit][relay_integration][m7_relay]") {
    std::vector<IceServerConfig> servers;
    IceServerConfig stun;
    stun.kind = "stun";
    stun.hostname = "stun.example.com";
    stun.port = 3478;
    IceServerConfig turn;
    turn.kind = "turn_udp";
    turn.hostname = "turn.example.com";
    turn.port = 3479;
    turn.username = "aki-user";
    turn.credential = "aki-cred";
    IceServerConfig turn2;
    turn2.kind = "turn_udp";
    turn2.hostname = "turn2.example.com";
    turn2.port = 1;
    turn2.username = "u";
    turn2.credential = "c";
    servers.push_back(stun);
    servers.push_back(turn);
    servers.push_back(turn2);

    const auto text = aki::heyaki::serialize_ice_servers(servers);
    auto parsed = aki::heyaki::parse_ice_servers_content(text);
    REQUIRE(parsed.invalid_lines == 0);
    REQUIRE(parsed.servers.size() == servers.size());
    for (std::size_t i = 0; i < servers.size(); ++i) {
        REQUIRE(same_server(parsed.servers[i], servers[i]));
    }
}

TEST_CASE("validate_ice_server covers every rejection branch",
    "[unit][relay_integration][m7_relay]") {
    auto reject = [](IceServerConfig server) {
        return aki::heyaki::validate_ice_server(server).has_value();
    };
    IceServerConfig valid_turn;
    valid_turn.kind = "turn_udp";
    valid_turn.hostname = "turn.example.com";
    valid_turn.port = 3478;
    valid_turn.username = "aki-user";
    valid_turn.credential = "aki-cred";
    IceServerConfig valid_stun;
    valid_stun.kind = "stun";
    valid_stun.hostname = "stun.example.com";
    valid_stun.port = 3478;
    REQUIRE_FALSE(reject(valid_turn));
    REQUIRE_FALSE(reject(valid_stun));

    // kind 非法（turn_tcp 不在文件格式支持集；空 kind 同拒）。
    auto bad_kind = valid_turn;
    bad_kind.kind = "turn_tcp";
    REQUIRE(reject(bad_kind));
    bad_kind.kind.clear();
    REQUIRE(reject(bad_kind));

    // hostname 空或含空白。
    auto bad_host = valid_stun;
    bad_host.hostname.clear();
    REQUIRE(reject(bad_host));
    bad_host.hostname = "turn example.com";
    REQUIRE(reject(bad_host));
    bad_host.hostname = "turn\texample.com";
    REQUIRE(reject(bad_host));

    // port 0（越界下界；上界由 uint16_t 类型承载）。
    auto bad_port = valid_stun;
    bad_port.port = 0;
    REQUIRE(reject(bad_port));

    // turn_udp 须 username + credential。
    auto no_user = valid_turn;
    no_user.username.clear();
    REQUIRE(reject(no_user));
    auto no_cred = valid_turn;
    no_cred.credential.clear();
    REQUIRE(reject(no_cred));

    // 字段含空白（行分隔符约束）。
    auto spaced_user = valid_turn;
    spaced_user.username = "aki user";
    REQUIRE(reject(spaced_user));
    auto spaced_cred = valid_turn;
    spaced_cred.credential = "aki cred";
    REQUIRE(reject(spaced_cred));

    // 错误信息可展示（非空）。
    const auto message = aki::heyaki::validate_ice_server(bad_kind);
    REQUIRE(message.has_value());
    REQUIRE_FALSE(message->empty());
}

TEST_CASE("load_ice_servers_file treats a missing file as empty config",
    "[unit][relay_integration][m7_relay]") {
    const auto missing = make_temp_root("ice-missing") / "ice-servers.txt";
    auto empty = aki::heyaki::load_ice_servers_file(missing.string());
    REQUIRE(empty.servers.empty());
    REQUIRE(empty.invalid_lines == 0);

    // 存在的文件按内容解析。
    const auto root = make_temp_root("ice-present");
    const auto file = root / "ice-servers.txt";
    {
        std::ofstream out(file, std::ios::binary);
        out << "stun stun.example.com 3478\n";
        out << "garbage\n";
    }
    auto parsed = aki::heyaki::load_ice_servers_file(file.string());
    REQUIRE(parsed.servers.size() == 1);
    REQUIRE(parsed.servers.front().hostname == "stun.example.com");
    REQUIRE(parsed.invalid_lines == 1);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
