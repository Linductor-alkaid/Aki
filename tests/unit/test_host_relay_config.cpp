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
// 进程唯一 Executor owner 是 HostRuntime 单例（AGENTS 规则 7/8；
// test_host_runtime 同款 main 模式）；测试线程只做有界轮询（yield），
// 不创建线程。
#include "app/lifecycle/host_runtime.hpp"
#include "heyaki/adapter/local_identity.hpp"
#include "heyaki/session/relay_enrollment.hpp"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
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
