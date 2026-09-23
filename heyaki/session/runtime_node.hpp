// Heyaki Node/Runtime 会话接线（DEC-006 运行期 executor 协调；M3-04 首批
// 落地；设计第 8.2/8.3 节；§14 heyaki/session 目录自本里程碑落地）。
//
// DEC-006 硬约束的接线形态：
//   - 借用注入唯一：Runtime::create_borrowed(ExecutorOwner.executor(), cfg)，
//     NodeConfig.runtime = &runtime；禁止 create_owned / runtime=nullptr
//     （进程内第二 executor 实例，违反 EXEC-01/AGENTS 规则 7-8）。
//   - 前置：宿主 executor 已 Running——create_borrowed 对未运行 executor 以
//     borrowed_executor_not_running 拒绝（本接线转为异常，失败可见不静默）。
//   - 定容：borrowed 模式下 RuntimeConfig 的 executor 线程参数不生效，合并
//     负载（heyaki async pool + blocking workers）只在 Aki ExecutorOwner 的
//     ExecutorConfig 显式定容（组合根职责）。
//   - 关闭顺序：Node::shutdown()（对外部 runtime 只 request_stop）+ 宿主显式
//     Runtime::shutdown()（回收挂在 Aki executor 上的 heyaki worker）编入
//     第 8.3 节关闭钩子停止生产者段（EXEC-01 步骤 1，早于 owner 步骤 2/3/5）；
//     断言 RuntimeShutdownReport.executor_shutdown_performed == false。
//
// RULE-10：本头文件属 heyaki/ 层；对组合根的公开面仅 aki/std 类型
// （EndpointView / NodeSessionShutdownReport / std::string），heyaki 类型
// 封死在实现细节内。并发契约：Node/Runtime 方法由调用方串行化（组合根/
// adapter 上下文）；观察管道的 poll 在 executor timer 上下文执行（见
// lan_discovery.hpp）。
#pragma once

#include "device/device/device_types.hpp"
#include "heyaki/adapter/local_identity.hpp"

#include <heyaki/node.hpp>
#include <heyaki/runtime.hpp>

#include <executor/executor.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace aki::heyaki {

// 测试/受控环境的快速 LAN 配置（heyaki 自身双节点测试同型：lan_only、
// 100ms 公告、短租约）。
[[nodiscard]] inline ::heyaki::LanConfiguration fast_lan_configuration() {
    ::heyaki::LanConfiguration configuration;
    configuration.connectivity_mode = ::heyaki::ConnectivityMode::lan_only;
    configuration.announcement_interval = std::chrono::milliseconds{100};
    configuration.announcement_jitter = std::chrono::milliseconds{0};
    configuration.presence_lease = std::chrono::milliseconds{1000};
    configuration.interface_refresh_interval = std::chrono::seconds{2};
    configuration.announcement_rate_per_second = 100U;
    configuration.per_source_announcement_rate = 100U;
    return configuration;
}

// 端点目录条目（aki/std 公开面；LAN 来源）。
struct EndpointView {
    aki::device::DeviceId device_id;
    aki::device::PublicKey public_key;  // identity_public_key（指纹数据）
    std::string endpoint_id;
    bool trusted = false;
};

// 关闭证据（DEC-006 借用模式断言：executor_shutdown_performed == false）。
struct NodeSessionShutdownReport {
    bool node_stopped = false;
    bool runtime_stopped = false;
    bool runtime_executor_shutdown_performed = false;
    bool runtime_drain_timed_out = false;
};

// 借用型 Node/Runtime 会话：构造即创建（失败抛 std::runtime_error，含
// borrowed_executor_not_running 等语义），shutdown() 幂等。
class NodeSession {
public:
    struct Options {
        LocalProfile* profile = nullptr;  // 常驻 profile（LocalProfile::open）
        std::string application_id = kAkiApplicationId;
        ::heyaki::LanConfiguration lan_override = fast_lan_configuration();
        // 同一 executor 上多个借用 Runtime 时须互异（blocking worker 名唯一）。
        std::string worker_name = "heyaki-asio";
    };

    // 前置：executor 已 Running（ExecutorOwner.initialize() 之后）。
    [[nodiscard]] static NodeSession create(executor::Executor& executor,
        const Options& options) {
        namespace hh = ::heyaki;
        auto runtime_result = hh::Runtime::create_borrowed(executor);
        if (!runtime_result) {
            const auto* error = runtime_result.error_if();
            throw std::runtime_error(
                std::string("node session: create_borrowed failed: ")
                + std::string(hh::error_code_name(error->code())) + ": "
                + std::string(error->safe_detail()));
        }

        // pinned release 拒绝缺省成员的 designated initializer：全成员列出
        // （与 heyaki 自身双节点测试一致）。
        hh::RuntimeConfig runtime_config{};
        runtime_config.worker_name = options.worker_name;

        // Runtime 包装对象堆置且地址稳定（M3-04 CI run 35922249364 ASan
        // stack-use-after-return 实测）：NodeConfig.runtime 为非拥有指针，
        // Node 内部异步路径（如 ShellPtyCoordinator drain、expiry timer）在
        // 返回 create() 之后仍经该指针访问 Runtime。若指向本函数栈上
        // Result 内的临时对象，函数返回即悬垂。unique_ptr 指针对象跨
        // NodeSession 移动保持地址不变；成员声明序（runtime_ 在 node_ 之前）
        // 保证析构时 Node 先于 Runtime 消亡。
        auto runtime = std::make_unique<hh::Runtime>(
            std::move(*runtime_result.value_if()));

        hh::NodeConfig config{.profile = &options.profile->store(),
            .runtime = runtime.get(),
            .application_id = options.application_id,
            .lan_override = options.lan_override,
            .runtime_config = runtime_config,
            .signaling_validator = {},
            .signaling_handler = {},
            .relay_override = std::nullopt,
            .path_policy_override = std::nullopt,
            .pairing_failure_threshold = 0U,
            .pairing_backoff_base = std::chrono::milliseconds{0},
            .pairing_backoff_max = std::chrono::milliseconds{0},
            .pairing_grant_ttl_milliseconds = 0U,
            .event_subscriber_queue_items = 0U,
            .event_max_subscriptions_per_peer = 0U,
            .file_receive_roots = {},
            .file_max_peer_receive_bytes = 0U,
            .shell_profiles = {},
            .gateway_profiles = {},
            .gateway_confirm_sink = {}};
        auto node_result = hh::Node::create(std::move(config));
        if (!node_result) {
            const auto* error = node_result.error_if();
            throw std::runtime_error(
                std::string("node session: Node::create failed: ")
                + std::string(hh::error_code_name(error->code())) + ": "
                + std::string(error->safe_detail()));
        }
        return NodeSession(std::move(runtime),
            std::move(*node_result.value_if()));
    }

    NodeSession(NodeSession&& other) noexcept
        : runtime_(std::move(other.runtime_)),
          node_(std::move(other.node_)),
          shutdown_done_(other.shutdown_done_.load(std::memory_order_relaxed)) {
        other.shutdown_done_.store(true, std::memory_order_relaxed);
    }

    NodeSession& operator=(NodeSession&&) = delete;
    NodeSession(const NodeSession&) = delete;
    NodeSession& operator=(const NodeSession&) = delete;

    ~NodeSession() {
        if (!shutdown_done_.exchange(true)) {
            (void)shutdown();
        }
    }

    [[nodiscard]] bool has_lan_interfaces() const {
        return !node_.snapshot().interfaces.empty();
    }

    // 端点目录条目（含 trusted 标记与 identity_public_key 指纹）。
    [[nodiscard]] std::vector<EndpointView> endpoints() const {
        std::vector<EndpointView> out;
        for (const auto& entry : node_.endpoints()) {
            EndpointView view;
            view.device_id =
                aki::device::DeviceId{::heyaki::to_string(entry.key.device_id)};
            view.endpoint_id = ::heyaki::to_string(entry.key.endpoint_id);
            view.trusted = entry.trusted;
            if (entry.lan.has_value()) {
                for (const std::byte byte : entry.lan->identity_public_key) {
                    view.public_key.bytes.push_back(
                        std::to_integer<std::uint8_t>(byte));
                }
            }
            out.push_back(std::move(view));
        }
        return out;
    }

    // 诊断面（aki/std；std::pair<peer, <state 枚举值, restricted>>）。
    [[nodiscard]] std::vector<
        std::pair<std::string, std::pair<int, bool>>>
    peer_session_diagnostics() const {
        std::vector<std::pair<std::string, std::pair<int, bool>>> out;
        for (const auto& session : node_.peer_sessions()) {
            out.push_back({::heyaki::to_string(session.peer.device_id),
                {static_cast<int>(session.state),
                    session.pairing_restricted}});
        }
        return out;
    }

    // DEC-006 映射 3：pairing_restricted 会话存在 → Unknown→Pending 触发。
    [[nodiscard]] bool session_pairing_restricted(
        const aki::device::DeviceId& peer) const {
        for (const auto& session : node_.peer_sessions()) {
            if (::heyaki::to_string(session.peer.device_id) == peer.value
                && session.state
                    == ::heyaki::NodePeerSessionState::pairing_restricted) {
                return true;
            }
        }
        return false;
    }

    // 一次性配对结果观察（Node 上下文回调；消费方保持有界处理，EXEC-02）。
    void set_pairing_observer(
        std::function<void(const aki::device::DeviceId& peer, bool success,
            const std::string& detail)>
            observer) {
        node_.set_pairing_observer(
            [observer = std::move(observer)](
                const ::heyaki::DeviceEndpointKey& peer,
                const ::heyaki::NodePairingOutcome& outcome) {
                if (!observer) {
                    return;
                }
                const aki::device::DeviceId peer_id{
                    ::heyaki::to_string(peer.device_id)};
                if (outcome) {
                    observer(peer_id, true, {});
                } else {
                    observer(peer_id, false,
                        std::string(outcome.error_if()->safe_detail()));
                }
            });
    }

    // 主动建链（LAN；pairing 前置——未信任对端会话进入 pairing_restricted）。
    [[nodiscard]] bool connect_lan(const aki::device::DeviceId& peer) {
        auto key = endpoint_key_of(peer);
        if (!key.has_value()) {
            return false;
        }
        auto connected = node_.connect_lan(*key);
        return connected.has_value();
    }

    // 会话已认证（pairing 成功后的稳态）。
    [[nodiscard]] bool session_authenticated(
        const aki::device::DeviceId& peer) const {
        for (const auto& session : node_.peer_sessions()) {
            if (::heyaki::to_string(session.peer.device_id) == peer.value
                && session.state
                    == ::heyaki::NodePeerSessionState::authenticated) {
                return true;
            }
        }
        return false;
    }

    // DEC-006 映射 3：指纹确认 → pair_peer（scope 冻结 message.send）。
    // 一次性结果经 set_pairing_observer 注册；false = 提交被拒（会话缺失/
    // 非 pairing_restricted/重复 pending，RULE-09 可见）。
    [[nodiscard]] bool pair_peer(const aki::device::DeviceId& peer,
        const std::string& password) {
        auto key = endpoint_key_of(peer);
        if (!key.has_value()) {
            return false;
        }
        auto submitted = node_.pair_peer(*key, password, {"message.send"});
        return submitted.has_value();
    }

    // DEC-006 映射 3：revoke_trust_grant → Revoked（撤销该 peer 全部有效
    // grant）；无 grant 时返回 false（无操作可见）。
    [[nodiscard]] bool revoke_trust_grants(const aki::device::DeviceId& peer) {
        auto key = endpoint_key_of(peer);
        if (!key.has_value()) {
            return false;
        }
        auto grants = node_.trust_grants_for(*key);
        if (!grants || grants.value_if()->empty()) {
            return false;
        }
        for (const auto& grant : *grants.value_if()) {
            if (!grant.revoked) {
                auto revoked = node_.revoke_trust_grant(grant.grant_id);
                if (!revoked) {
                    return false;
                }
            }
        }
        return true;
    }

    // 关闭（幂等）：Node::shutdown → Runtime::shutdown。borrowed 模式下
    // runtime 不代收官宿主 executor（executor_shutdown_performed == false，
    // DEC-006 断言点）。
    NodeSessionShutdownReport shutdown() {
        if (shutdown_done_.exchange(true)) {
            return last_report_;
        }
        NodeSessionShutdownReport report;
        auto node_report = node_.shutdown();
        report.node_stopped = node_report.stopped;
        auto runtime_report = runtime_->shutdown();
        report.runtime_stopped =
            runtime_report.final_phase == ::heyaki::RuntimePhase::stopped;
        report.runtime_executor_shutdown_performed =
            runtime_report.executor_shutdown_performed;
        report.runtime_drain_timed_out =
            runtime_report.callback_drain_timed_out
            || runtime_report.operation_drain_timed_out
            || runtime_report.worker_stop_timed_out
            || runtime_report.executor_drain_timed_out;
        last_report_ = report;
        return report;
    }

private:
    NodeSession(std::unique_ptr<::heyaki::Runtime> runtime, ::heyaki::Node node)
        : runtime_(std::move(runtime)), node_(std::move(node)) {}

    [[nodiscard]] std::optional<::heyaki::DeviceEndpointKey> endpoint_key_of(
        const aki::device::DeviceId& peer) const {
        for (const auto& entry : node_.endpoints()) {
            if (::heyaki::to_string(entry.key.device_id) == peer.value) {
                return entry.key;
            }
        }
        // 已知对端可能不在目录（目录只收 LAN/Relay 公告）——回退 peer_sessions。
        for (const auto& session : node_.peer_sessions()) {
            if (::heyaki::to_string(session.peer.device_id) == peer.value) {
                return session.peer;
            }
        }
        return std::nullopt;
    }

    // 声明序即析构序约束：reverse 析构先 node_ 后 runtime_——Node 内部持有
    // runtime_.get() 非拥有指针（见 create 内注释），Node 必须先消亡。
    std::unique_ptr<::heyaki::Runtime> runtime_;
    ::heyaki::Node node_;
    std::atomic<bool> shutdown_done_{false};
    NodeSessionShutdownReport last_report_{};
};

}  // namespace aki::heyaki
