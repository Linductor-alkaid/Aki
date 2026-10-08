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

#include "conversation/codec/image_payload_codec.hpp"
#include "conversation/message/message_types.hpp"
#include "device/device/device_types.hpp"
#include "heyaki/adapter/local_identity.hpp"
#include "heyaki/adapter/wire_ids.hpp"
#include "transfer/transfer/transfer_types.hpp"

#include <heyaki/node.hpp>
#include <heyaki/runtime.hpp>

#include <kairo/executor.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

// 生产连通配置（M7，DEC-028 决策 1）：automatic——LAN 信令优先、不可达时
// relay 兜底（relay 分支仅在 profile 存在有效 enrollment 时激活，无记录时
// 与 lan_only 的可见行为一致）。公告/租约节奏取 heyaki 缺省（5s 公告/15s
// 租约），不沿用 fast 配置的 100ms 测试节奏。
[[nodiscard]] inline ::heyaki::LanConfiguration
production_lan_configuration() {
    ::heyaki::LanConfiguration configuration;
    configuration.connectivity_mode =
        ::heyaki::ConnectivityMode::automatic;
    return configuration;
}

// 端点目录条目（aki/std 公开面；LAN 与 relay 合并目录来源标记——同一
// device 可两者同时在目录，public_key 优先取 LAN 条目、缺失时回落 relay
// 条目（同源 identity_public_key，M7/DEC-028 决策 5）。
struct EndpointView {
    aki::device::DeviceId device_id;
    aki::device::PublicKey public_key;  // identity_public_key（指纹数据）
    std::string endpoint_id;
    bool trusted = false;
    bool lan_visible = false;    // LAN 公告条目存在
    bool relay_visible = false;  // relay 目录条目存在
};

// 关闭证据（DEC-006 借用模式断言：executor_shutdown_performed == false）。
struct NodeSessionShutdownReport {
    bool node_stopped = false;
    bool runtime_stopped = false;
    bool runtime_executor_shutdown_performed = false;
    bool runtime_drain_timed_out = false;
};

// relay 控制面运行态（M7，DEC-028 决策 8；aki/std 公开面——数值与语义名
// 均来自上游 RelayNodeState，语义解释收敛在本层）。state 语义：
// disabled=未启用（无有效 enrollment）/ starting=连接中 / ready=登录且
// 租约有效 / degraded=missed_heartbeat_limit 内重连退避 / failed=不可用
//（last_error 非空）/ stopped=已关闭。
struct RelayStatusView {
    bool enabled = false;
    int state = 0;           // RelayNodeState 数值
    std::string state_name;  // relay_node_state_name 语义名（诊断展示）
    std::string relay_url;
    std::string tenant;
    std::string last_error;  // 空 = 无错误
    // M8-05：relay 下发短时效 ICE（relay_ice_config_v1）的可观测计数
    //（上游 RelayNodeSnapshot——仅计数，凭据材料不进快照）。servers_active
    // = 当前持有的未过期 relay 下发服务器数；expires 为最新凭据到期
    // UNIX 秒（0 = 无）。静态 ice-servers.txt 配置不在此列（设置页高级区
    // 展示），两者由上游 merge_relay_ice_servers 合并（静态优先、下发
    // 追加、过期剔除、relay 断连回落静态）。
    std::uint64_t ice_config_updates = 0;
    std::uint64_t ice_config_rejected = 0;
    std::size_t ice_config_servers_active = 0;
    std::uint64_t ice_config_expires_unix_seconds = 0;
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
        // 接收根配置（M4-05，DEC-012：组合根把接收根目录配置在数据根内，
        // 如 <data_root>/receive/<root>，使接收侧合并同卷可 rename；根逻辑名
        // 与对端 push_root 对应——Aki 默认单根 "inbox"）。空 = 不接收
        //（对端 push 将被拒）。
        std::vector<::heyaki::FileRootConfig> file_receive_roots = {};
        // DEC-023: explicit host policy, independent of persisted device grants.
        bool basic_communication = false;
        bool pairing_approval_enabled = false;
        std::chrono::milliseconds pairing_deadline{0};
        // HEY-20261001-001: 0 retains the upstream bounded 30s offer window.
        std::chrono::milliseconds file_offer_timeout{0};
        // ICE/TURN 服务器（M7，DEC-028 决策 7）：非空时组装
        // path_policy_override（default_peer_path_policy(当前 mode) +
        // ice_servers，validate_peer_path_policy 前置校验失败即装配失败
        // 可见）；空 = 沿用上游按 mode 的缺省策略。
        std::vector<::heyaki::NodeIceServer> ice_servers = {};
    };

    // 前置：executor 已 Running（ExecutorOwner.initialize() 之后）。
    [[nodiscard]] static NodeSession create(kairo::Executor& executor,
        const Options& options) {
        namespace hh = ::heyaki;
        hh::RuntimeConfig runtime_config{};
        runtime_config.worker_name = options.worker_name;
        auto runtime_result = hh::Runtime::create_borrowed(executor, runtime_config);
        if (!runtime_result) {
            const auto* error = runtime_result.error_if();
            throw std::runtime_error(
                std::string("node session: create_borrowed failed: ")
                + std::string(hh::error_code_name(error->code())) + ": "
                + std::string(error->safe_detail()));
        }

        // pinned release 拒绝缺省成员的 designated initializer：全成员列出
        // （与 heyaki 自身双节点测试一致）。
        // Runtime 包装对象堆置且地址稳定（M3-04 CI run 35922249364 ASan
        // stack-use-after-return 实测）：NodeConfig.runtime 为非拥有指针，
        // Node 内部异步路径（如 ShellPtyCoordinator drain、expiry timer）在
        // 返回 create() 之后仍经该指针访问 Runtime。若指向本函数栈上
        // Result 内的临时对象，函数返回即悬垂。unique_ptr 指针对象跨
        // NodeSession 移动保持地址不变；成员声明序（runtime_ 在 node_ 之前）
        // 保证析构时 Node 先于 Runtime 消亡。
        auto runtime = std::make_unique<hh::Runtime>(
            std::move(*runtime_result.value_if()));

        // ICE 服务器注入（M7，DEC-028 决策 7）：非空时以当前连通模式的
        // 缺省策略为基线填入 ice_servers；validate 失败（如 lan_only 下配
        // ICE、TURN/TLS 无后端）以异常拒绝装配——失败可见不静默（RULE-09）。
        std::optional<::heyaki::PeerPathPolicy> path_policy{};
        if (!options.ice_servers.empty()) {
            const auto mode = options.lan_override.connectivity_mode;
            auto base = ::heyaki::default_peer_path_policy(mode);
            if (!base) {
                const auto* error = base.error_if();
                throw std::runtime_error(
                    std::string("node session: default path policy failed: ")
                    + std::string(hh::error_code_name(error->code())));
            }
            base.value_if()->ice_servers = options.ice_servers;
            auto valid = ::heyaki::validate_peer_path_policy(
                *base.value_if(), mode);
            if (!valid) {
                const auto* error = valid.error_if();
                throw std::runtime_error(
                    std::string("node session: ice servers rejected: ")
                    + std::string(hh::error_code_name(error->code())) + ": "
                    + std::string(error->safe_detail()));
            }
            path_policy = std::move(*base.value_if());
        }

        hh::NodeConfig config{.profile = &options.profile->store(),
            .runtime = runtime.get(),
            .application_id = options.application_id,
            .lan_override = options.lan_override,
            .runtime_config = runtime_config,
            .signaling_validator = {},
            .signaling_handler = {},
            .relay_override = std::nullopt,
            .path_policy_override = std::move(path_policy),
            .pairing_failure_threshold = 0U,
            .pairing_backoff_base = std::chrono::milliseconds{0},
            .pairing_backoff_max = std::chrono::milliseconds{0},
            .pairing_grant_ttl_milliseconds = 0U,
            .pairing_approval_enabled = options.pairing_approval_enabled,
            .basic_communication = options.basic_communication,
            .pairing_deadline = options.pairing_deadline,
            .event_subscriber_queue_items = 0U,
            .event_max_subscriptions_per_peer = 0U,
            .file_receive_roots = options.file_receive_roots,
            .file_max_peer_receive_bytes = 0U,
            .file_offer_timeout = options.file_offer_timeout,
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
            std::move(*node_result.value_if()), options.profile,
            aki::device::DeviceId{hh::to_string(
                options.profile->store().device_id())});
    }

    NodeSession(std::unique_ptr<::heyaki::Runtime> runtime,
        ::heyaki::Node node, LocalProfile* profile,
        aki::device::DeviceId local_id)
        : runtime_(std::move(runtime)),
          node_(std::move(node)),
          profile_(profile),
          local_id_(local_id) {}

    NodeSession(NodeSession&& other) noexcept
        : runtime_(std::move(other.runtime_)),
          node_(std::move(other.node_)),
          profile_(other.profile_),
          local_id_(other.local_id_),
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

    // 本机设备 id（DEC-006 映射 1；profile 身份的 aki 规范形式）。
    [[nodiscard]] aki::device::DeviceId local_id() const {
        return local_id_;
    }

    [[nodiscard]] bool has_lan_interfaces() const {
        return !node_.snapshot().interfaces.empty();
    }

    // 热生效（M8-08，HEY-20261006-001 收口）：按 profile 当前首条有效
    //（auto_connect && !revoked）enrollment 记录热更新运行中 Node 的 relay
    // 配置——字段映射镜像上游 load_relay_config_from_profile（有 pin 时
    // tls_verify_peer=false，登录期 TOFU pin 校验）；无有效记录 → 传
    // nullopt 断开控制面（已认证会话的直连传输保留，上游
    // update_relay_config 语义：strand 投递、非法配置保留旧连接且错误经
    // 快照可见、与关闭竞争为 no-op）。失败 false + error 可见（RULE-09）。
    [[nodiscard]] bool apply_relay_enrollment_now(std::string& error) {
        std::optional<::heyaki::RelayNodeConfig> relay_config{};
        if (profile_ == nullptr) {
            error = "node session: profile unavailable for relay update";
            return false;
        }
        auto records = profile_->store().relay_enrollments();
        if (!records) {
            error = std::string(records.error_if()->safe_detail());
            return false;
        }
        for (const auto& record : *records.value_if()) {
            if (!record.auto_connect || record.revoked) {
                continue;
            }
            ::heyaki::RelayNodeConfig config;
            config.enabled = true;
            config.relay_url = record.relay_url;
            config.relay_pin = record.relay_pin;
            config.tenant = record.tenant;
            config.enrollment_generation = record.enrollment_generation;
            if (config.relay_pin) {
                config.tls_verify_peer = false;
            }
            relay_config = config;
            break;
        }
        auto updated = node_.update_relay_config(std::move(relay_config));
        if (!updated) {
            error = std::string(updated.error_if()->safe_detail());
            return false;
        }
        return true;
    }

    // relay 控制面运行态快照（NodeSnapshot.relay 的 aki 公开面投影）。
    [[nodiscard]] RelayStatusView relay_status() const {
        const auto snapshot = node_.snapshot();
        RelayStatusView view;
        view.enabled = snapshot.relay.enabled;
        view.state = static_cast<int>(snapshot.relay.state);
        view.state_name = std::string(
            ::heyaki::relay_node_state_name(snapshot.relay.state));
        view.relay_url = snapshot.relay.relay_url;
        view.tenant = snapshot.relay.tenant;
        if (snapshot.relay.last_error.has_value()) {
            view.last_error = snapshot.relay.last_error->safe_detail();
        }
        view.ice_config_updates = snapshot.relay.ice_config_updates;
        view.ice_config_rejected = snapshot.relay.ice_config_rejected;
        view.ice_config_servers_active =
            snapshot.relay.ice_config_servers_active;
        view.ice_config_expires_unix_seconds =
            snapshot.relay.ice_config_expires_unix_seconds;
        return view;
    }

    // 端点目录条目（含 trusted 标记与 identity_public_key 指纹；LAN/relay
    // 合并目录——public_key 优先 LAN 条目，纯 relay 条目回落
    // relay->identity_public_key，M7/DEC-028 决策 5）。
    [[nodiscard]] std::vector<EndpointView> endpoints() const {
        std::vector<EndpointView> out;
        for (const auto& entry : node_.endpoints()) {
            EndpointView view;
            view.device_id =
                aki::device::DeviceId{::heyaki::to_string(entry.key.device_id)};
            view.endpoint_id = ::heyaki::to_string(entry.key.endpoint_id);
            view.trusted = entry.trusted;
            view.lan_visible = entry.lan.has_value();
            view.relay_visible = entry.relay.has_value();
            std::span<const std::byte> identity_key{};
            if (entry.lan.has_value()) {
                identity_key = entry.lan->identity_public_key;
            } else if (entry.relay.has_value()) {
                identity_key = entry.relay->identity_public_key;
            }
            for (const std::byte byte : identity_key) {
                view.public_key.bytes.push_back(
                    std::to_integer<std::uint8_t>(byte));
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

    // ---- M3-06：peer_sessions 观察面（aki/std 公开面）----
    // state/data_path/signaling_route 为 heyaki 枚举的数值（映射语义见
    // adapter 层 map_connection_path 与 DEC-006 映射 5；数值不跨层解释）。
    struct TrustDirections {
        bool issued = false;
        bool received = false;
        bool operator==(const TrustDirections&) const = default;
    };

    struct PeerSessionView {
        aki::device::DeviceId device_id;
        std::string endpoint_id;
        int state = 0;            // NodePeerSessionState 数值
        int data_path = 0;        // NodeDataPathKind 数值
        int signaling_route = 0;  // SignalingRouteKind 数值
        bool authenticated = false;
        bool pairing_restricted = false;
        bool closed = false;
        bool basic_communication = false;
        std::vector<std::string> policy_scopes;
        std::vector<std::string> authorized_scopes;
        // Unknown query results never mean grant revocation. M5-41 samples
        // both directions even when the authorized session state is unchanged.
        std::optional<TrustDirections> trust_directions;
        // DEC-006 映射 6：同 SessionId、epoch+1 的重建可观测性。
        std::string session_id;   // heyaki::to_string(SessionId) 规范形式
        std::uint64_t session_epoch = 1;
    };

    [[nodiscard]] std::vector<PeerSessionView> peer_session_views() const {
        std::vector<PeerSessionView> out;
        for (const auto& session : node_.peer_sessions()) {
            PeerSessionView view;
            view.device_id =
                aki::device::DeviceId{::heyaki::to_string(session.peer.device_id)};
            view.endpoint_id = ::heyaki::to_string(session.peer.endpoint_id);
            view.state = static_cast<int>(session.state);
            view.data_path = static_cast<int>(session.data_path);
            view.signaling_route = static_cast<int>(session.signaling_route);
            view.authenticated =
                session.state == ::heyaki::NodePeerSessionState::authenticated;
            view.pairing_restricted =
                session.state == ::heyaki::NodePeerSessionState::pairing_restricted;
            view.closed =
                session.state == ::heyaki::NodePeerSessionState::closed;
            view.basic_communication = session.basic_communication;
            view.policy_scopes = session.policy_scopes;
            view.authorized_scopes = session.authorized_scopes;
            if (view.authenticated || view.pairing_restricted) {
                view.trust_directions = trust_directions_for_key(session.peer);
            }
            view.session_id = ::heyaki::to_string(session.session_id);
            view.session_epoch = session.session_epoch;
            out.push_back(std::move(view));
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

    // 关闭与对端的会话（M3-07 断线恢复测试入口；对已认证会话强制断开）。
    [[nodiscard]] bool close_lan(const aki::device::DeviceId& peer) {
        auto key = endpoint_key_of(peer);
        if (!key.has_value()) {
            return false;
        }
        auto closed = node_.close_lan(*key);
        return closed.has_value();
    }

    // DEC-006 映射 6：会话重建——同 SessionId、epoch+1（协议 1.2 原地
    // 重协商；接口变化时 heyaki 亦自动触发）。仅对已认证会话有效，失败
    //（会话缺失等）返回 false 可见（RULE-09）。
    [[nodiscard]] bool restart_session(const aki::device::DeviceId& peer) {
        auto key = endpoint_key_of(peer);
        if (!key.has_value()) {
            return false;
        }
        auto restarted = node_.restart_session(*key);
        return restarted.has_value();
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

    // 通用建链（M7，DEC-028 决策 6）：Node::connect 按 ConnectivityMode
    // 自动选路（automatic：LAN 优先、目录无 LAN 条目时 relay 信令兜底）。
    // 纯 relay 可见对端的唯一发起路径；配对前置语义与 connect_lan 相同
    //（未信任对端进入 pairing_restricted）。
    [[nodiscard]] bool connect_peer(const aki::device::DeviceId& peer) {
        auto key = endpoint_key_of(peer);
        if (!key.has_value()) {
            return false;
        }
        auto connected = node_.connect(*key);
        return connected.has_value();
    }

    // 对端当前是否在 LAN/Relay 目录可见（租约内存活 = 正在运行 Aki）。
    // 重连对账（DEC-021）的发起门：目录不可见时 connect_lan 必然被拒。
    [[nodiscard]] bool endpoint_visible(
        const aki::device::DeviceId& peer) const {
        for (const auto& entry : node_.endpoints()) {
            if (::heyaki::to_string(entry.key.device_id) == peer.value) {
                return true;
            }
        }
        return false;
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

    // Verified control link, including the pre-authorization pairing state.
    [[nodiscard]] bool session_linked(
        const aki::device::DeviceId& peer) const {
        for (const auto& session : node_.peer_sessions()) {
            if (::heyaki::to_string(session.peer.device_id) == peer.value
                && (session.state
                        == ::heyaki::NodePeerSessionState::authenticated
                    || session.state
                        == ::heyaki::NodePeerSessionState::pairing_restricted)) {
                return true;
            }
        }
        return false;
    }

    // DEC-006 映射 3：指纹确认 → pair_peer（scope：message.send + M4-05 起
    // 文件推送独立 scope file.push:<root>，DEC-012⑥——缺省申请全集）。
    // v1.1.1 admission is bounded and returns strand validation errors;
    // accepted attempts report one terminal outcome through the observer.
    // Authorized sessions may request a reverse grant or repair an old grant.
    [[nodiscard]] bool pair_peer(const aki::device::DeviceId& peer,
        const std::string& password,
        std::vector<std::string> scopes = {"message.send", "file.push:inbox"}) {
        auto key = endpoint_key_of(peer);
        if (!key) return false;
        return node_.pair_peer(*key, password, std::move(scopes)).has_value();
    }

    [[nodiscard]] bool rotate_local_password(std::string_view password,
        std::string& error) {
        if (password == kLegacyPairingPassword) {
            error = "Choose a different pairing password";
            return false;
        }
        auto rotated = node_.rotate_authorization_password(password);
        if (!rotated) {
            error = std::string(rotated.error_if()->safe_detail());
            return false;
        }
        error.clear();
        return true;
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

    // 对向信任查询（DEC-021 四态数据源）：本机 TrustStore 中与该 peer 的
    // 双向有效 grant（SQL 已过滤 revoked 与过期）。issued = 本机签发给
    // 对端（「本机信任对方」）；received = 对端签发给本机（「对方已信任
    // 本机」的 heyaki 权威记录）。查不到 endpoint key（对端不在目录且无
    // 会话）时返回 std::nullopt，由调用方按无信息处理而非 false。
    [[nodiscard]] std::optional<TrustDirections> trust_grants(
        const aki::device::DeviceId& peer) {
        auto key = endpoint_key_of(peer);
        if (!key.has_value()) {
            return std::nullopt;
        }
        return trust_directions_for_key(*key);
    }

    // ---- M3-05/M4-03：消息面（DEC-006 映射 4 + 图片面扩展；aki/std 公开面）----

    // aki MessageId ↔ heyaki MessageId 双射（DEC-006 冻结常量）：权威形式为
    // heyaki::to_string / parse_message_id 的规范字符串（hym1_ 前缀编码）；
    // 解码失败为 nullopt——调用方按 admission 拒绝处理（RULE-09）。
    [[nodiscard]] static std::optional<::heyaki::MessageId> to_heyaki_message_id(
        const aki::conversation::MessageId& message_id) {
        auto decoded = ::heyaki::parse_message_id(message_id.value);
        if (!decoded || decoded.error != ::heyaki::IdentifierDecodeError::none) {
            return std::nullopt;
        }
        return decoded.value;
    }

    [[nodiscard]] static aki::conversation::MessageId to_aki_message_id(
        const ::heyaki::MessageId& message_id) {
        return aki::conversation::MessageId{::heyaki::to_string(message_id)};
    }

    // aki TransferId ↔ heyaki TransferId 双射（DEC-010；hyt1_ 规范串，同上
    // 语义）：非规范形式解码失败 → nullopt（admission 拒绝可见）。
    [[nodiscard]] static std::optional<::heyaki::TransferId> to_heyaki_transfer_id(
        const aki::transfer::TransferId& transfer_id) {
        auto decoded = ::heyaki::parse_transfer_id(transfer_id.value);
        if (!decoded || decoded.error != ::heyaki::IdentifierDecodeError::none) {
            return std::nullopt;
        }
        return decoded.value;
    }

    // 新 TransferId 生成（DEC-011 ④ 定案，M4-04）：16 随机字节（全零重抽）→
    // heyaki 规范串（hyt1_ + 26 base32，按构造规范）——ad-hoc 串（如 "t-1"）
    // 在 push_file 转换与 codec/DEC-010 谓词处被拒。M5-05 起实现委托
    // heyaki/adapter/wire_ids.hpp 的统一入口（§6.1 生成入口单一；UI 出站面
    // 经同一入口绑定），本静态成员保持既有调用方兼容。
    [[nodiscard]] static aki::transfer::TransferId new_transfer_id() {
        return aki::heyaki::new_transfer_id();
    }

    // 出站文本（DEC-006 映射 4）：MessageEnvelope{message_id = aki MessageId
    // 16B 双射, type = "aki.text", delivery_mode = peer_acked}；Result 失败
    //（peer_offline / 会话缺失等）返回 false——SPI false + 拒绝可见（RULE-09），
    // 终态结果经 set_message_handlers 的 ack 回调异步到达。
    [[nodiscard]] bool send_text(const aki::device::DeviceId& peer,
        const aki::conversation::MessageId& message_id,
        std::string_view text) {
        auto key = endpoint_key_of(peer);
        if (!key.has_value()) {
            return false;
        }
        auto wire_id = to_heyaki_message_id(message_id);
        if (!wire_id.has_value()) {
            return false;  // 非 16B hex：编码契约违反，admission 拒绝
        }
        ::heyaki::MessageEnvelope envelope;
        envelope.message_id = *wire_id;
        envelope.type = "aki.text";  // DEC-006 冻结常量
        envelope.delivery_mode =
            ::heyaki::MessageDeliveryMode::peer_acked;
        const auto* data = reinterpret_cast<const std::byte*>(text.data());
        envelope.payload.assign(data, data + text.size());
        auto sent = node_.send_message(*key, std::move(envelope));
        return sent.has_value();
    }

    // 出站图片（M4-03，DEC-010①/DEC-006 映射 4 图片面扩展）：
    // MessageEnvelope{message_id 16B 双射, type = "aki.image", schema_version =
    // 1（aki 载荷 schema 版本，协议层仅校验非零）, delivery_mode = peer_acked,
    // payload = ImagePayload 冻结字段号编码（conversation/codec）}。有界校验
    //（RULE-09，任一失败 admission false 可见）：TransferId 非规范
    //（heyaki::parse_transfer_id 权威校验——codec 谓词之外的双射权威）/
    // 载荷超限（codec 编码 nullopt）/ peer 不可达 / MessageId 非规范。
    // 图片本体不经本路径（RULE-05，传输面经 DEC-006 映射 7）。
    [[nodiscard]] bool send_image(const aki::device::DeviceId& peer,
        const aki::conversation::MessageId& message_id,
        const aki::transfer::FileMetadata& media,
        const aki::transfer::TransferId& transfer_id) {
        auto key = endpoint_key_of(peer);
        if (!key.has_value()) {
            return false;
        }
        auto wire_id = to_heyaki_message_id(message_id);
        if (!wire_id.has_value()) {
            return false;
        }
        if (!to_heyaki_transfer_id(transfer_id).has_value()) {
            return false;  // 非规范 hyt1_ 形式：编码契约违反，admission 拒绝
        }
        auto payload = aki::conversation::codec::encode_image_payload(
            aki::conversation::ImagePayload{media, transfer_id});
        if (!payload.has_value()) {
            return false;  // 载荷超限/字段非法：codec 有界拒绝
        }
        ::heyaki::MessageEnvelope envelope;
        envelope.message_id = *wire_id;
        envelope.type = std::string(
            aki::conversation::codec::kAkiImageEnvelopeType);  // DEC-006 冻结常量
        envelope.schema_version =
            aki::conversation::codec::kAkiImagePayloadSchemaVersion;
        envelope.delivery_mode =
            ::heyaki::MessageDeliveryMode::peer_acked;
        envelope.payload = std::move(*payload);
        auto sent = node_.send_message(*key, std::move(envelope));
        return sent.has_value();
    }

    [[nodiscard]] bool send_file(const aki::device::DeviceId& peer,
        const aki::conversation::MessageId& message_id,
        const aki::transfer::FileMetadata& media,
        const aki::transfer::TransferId& transfer_id) {
        auto key = endpoint_key_of(peer);
        auto wire_id = to_heyaki_message_id(message_id);
        if (!key || !wire_id || !to_heyaki_transfer_id(transfer_id)) return false;
        auto payload = aki::conversation::codec::encode_image_payload(
            aki::conversation::ImagePayload{media, transfer_id});
        if (!payload) return false;
        ::heyaki::MessageEnvelope envelope;
        envelope.message_id = *wire_id;
        envelope.type = std::string(aki::conversation::codec::kAkiFileEnvelopeType);
        envelope.schema_version =
            aki::conversation::codec::kAkiImagePayloadSchemaVersion;
        envelope.delivery_mode = ::heyaki::MessageDeliveryMode::peer_acked;
        envelope.payload = std::move(*payload);
        return node_.send_message(*key, std::move(envelope)).has_value();
    }

    // 入站与投递回报（DEC-006 映射 4；Node 上下文回调，消费方有界处理 +
    // 投递，EXEC-02）：
    //   inbound：协议层已去重 + ACK 应答后的信封——payload 为信封原始字节，
    //   type 为信封 type（aki.text / aki.image / …）。M4-03 起入站不再压平为
    //   text 串：信封 type 分发收敛在 Adapter 层（设计 §6.1①/§8.1——aki 层
    //   包装不承载应用语义，未知 type 的有界拒绝由 Adapter 计数可观测）；
    //   ack event 映射：queued→queued、acked→acked、send_failed/peer_rejected/
    //   ack_timeout/session_closed→failed（aki DeliveryState 语义域）。
    void set_message_handlers(
        std::function<void(const aki::device::DeviceId& peer,
            const aki::conversation::MessageId& message_id,
            const std::string& type,
            const std::string& payload)>
            inbound,
        std::function<void(const aki::device::DeviceId& peer,
            const aki::conversation::MessageId& message_id,
            const std::string& event)>
            ack) {
        node_.set_message_inbound_handler(
            [inbound = std::move(inbound)](
                const ::heyaki::DeviceEndpointKey& peer,
                const ::heyaki::MessageEnvelope& envelope) {
                if (!inbound) {
                    return;
                }
                const std::string payload(
                    reinterpret_cast<const char*>(envelope.payload.data()),
                    envelope.payload.size());
                inbound(
                    aki::device::DeviceId{
                        ::heyaki::to_string(peer.device_id)},
                    to_aki_message_id(envelope.message_id), envelope.type,
                    payload);
            });
        node_.set_message_ack_observer(
            [ack = std::move(ack)](const ::heyaki::DeviceEndpointKey& peer,
                const ::heyaki::MessageId& message_id,
                ::heyaki::MessageDeliveryEvent event,
                const std::optional<::heyaki::Error>&) {
                if (!ack) {
                    return;
                }
                const char* mapped = "failed";
                switch (event) {
                    case ::heyaki::MessageDeliveryEvent::queued:
                        mapped = "queued";
                        break;
                    case ::heyaki::MessageDeliveryEvent::acked:
                        mapped = "acked";
                        break;
                    case ::heyaki::MessageDeliveryEvent::send_failed:
                    case ::heyaki::MessageDeliveryEvent::peer_rejected:
                    case ::heyaki::MessageDeliveryEvent::ack_timeout:
                    case ::heyaki::MessageDeliveryEvent::session_closed:
                        mapped = "failed";
                        break;
                }
                ack(aki::device::DeviceId{::heyaki::to_string(peer.device_id)},
                    to_aki_message_id(message_id), mapped);
            });
    }

    // ---- M4-04：文件传输面（DEC-006 映射 7；aki/std 公开面）----

    // 出站 push（发送侧真实数据链路）：push_file(peer, root, logical_name,
    // source_path, transfer_id)——wire 侧 heyaki 自读源文件（probing 相位在其
    // blocking worker），进度/终态经 set_file_event_observer 回报（M4-05 路由
    // 接线）。TransferId 须为规范形式（双射转换，非规范 admission false，
    // DEC-011 ④）；root 为对端逻辑根名（组合根注入 Adapter 选项，§7.1⑤）。
    [[nodiscard]] bool push_file(const aki::device::DeviceId& peer,
        const std::string& root, const std::string& logical_name,
        const std::filesystem::path& source_path,
        const aki::transfer::TransferId& transfer_id) {
        auto key = endpoint_key_of(peer);
        if (!key.has_value()) {
            return false;
        }
        auto wire_id = to_heyaki_transfer_id(transfer_id);
        if (!wire_id.has_value()) {
            return false;  // 非规范 hyt1_ 形式：编码契约违反，admission 拒绝
        }
        auto pushed =
            node_.push_file(*key, root, logical_name, source_path, *wire_id);
        return pushed.has_value();
    }

    // 已建会话/目录中的对端设备（aki 规范形式；控制面遍历用，M4-05）。
    [[nodiscard]] std::vector<aki::device::DeviceId> known_peers() const {
        std::vector<aki::device::DeviceId> peers;
        for (const auto& entry : node_.endpoints()) {
            peers.push_back(aki::device::DeviceId{
                ::heyaki::to_string(entry.key.device_id)});
        }
        for (const auto& session : node_.peer_sessions()) {
            peers.push_back(aki::device::DeviceId{
                ::heyaki::to_string(session.peer.device_id)});
        }
        return peers;
    }

    // 传输控制面（M4-05，DEC-006 映射 7）：pause/resume/cancel——TransferId
    // 须规范形式；peer 无会话/传输不存在时 Result 失败 → false 可见
    //（RULE-09）。取消为幂等语义。
    [[nodiscard]] bool pause_file_transfer(const aki::device::DeviceId& peer,
        const aki::transfer::TransferId& transfer_id) {
        auto key = endpoint_key_of(peer);
        auto wire_id = to_heyaki_transfer_id(transfer_id);
        if (!key.has_value() || !wire_id.has_value()) {
            return false;
        }
        return node_.pause_file_transfer(*key, *wire_id).has_value();
    }

    [[nodiscard]] bool resume_file_transfer(const aki::device::DeviceId& peer,
        const aki::transfer::TransferId& transfer_id) {
        auto key = endpoint_key_of(peer);
        auto wire_id = to_heyaki_transfer_id(transfer_id);
        if (!key.has_value() || !wire_id.has_value()) {
            return false;
        }
        return node_.resume_file_transfer(*key, *wire_id).has_value();
    }

    [[nodiscard]] bool cancel_file_transfer(const aki::device::DeviceId& peer,
        const aki::transfer::TransferId& transfer_id) {
        auto key = endpoint_key_of(peer);
        auto wire_id = to_heyaki_transfer_id(transfer_id);
        if (!key.has_value() || !wire_id.has_value()) {
            return false;
        }
        return node_.cancel_file_transfer(*key, *wire_id).has_value();
    }

    // ---- M4-05：文件事件观察面（DEC-006 映射 7/DEC-012④；aki/std 公开面）----
    // phase/direction 为 heyaki 枚举的数值（八态映射语义在 Adapter 层，
    // DEC-006 映射 7；数值不跨层解释——同 peer_sessions 观察面先例）。
    // root/logical_name 为 wire 逻辑值（绝对路径由 Aki 依自身接收根配置推导）。
    struct FileTransferEventView {
        aki::transfer::TransferId transfer;
        int direction = 0;  // FileTransferDirection 数值
        int phase = 0;      // FileTransferPhase 数值
        std::string root;
        std::string logical_name;
        std::uint64_t bytes_done = 0;
        std::uint64_t bytes_total = 0;
        std::string error;
    };

    // 文件事件观察注册（Node 上下文回调，消费方有界处理 + 投递，EXEC-02）。
    void set_file_event_observer(
        std::function<void(const aki::device::DeviceId& peer,
            const FileTransferEventView& event)>
            observer) {
        node_.set_file_event_observer(
            [observer = std::move(observer)](
                const ::heyaki::DeviceEndpointKey& peer,
                const ::heyaki::FileTransferEvent& event) {
                if (!observer) {
                    return;
                }
                FileTransferEventView view;
                view.transfer =
                    aki::transfer::TransferId{::heyaki::to_string(
                        event.transfer_id)};
                view.direction = static_cast<int>(event.direction);
                view.phase = static_cast<int>(event.phase);
                view.root = event.root;
                view.logical_name = event.logical_name;
                view.bytes_done = event.bytes_done;
                view.bytes_total = event.bytes_total;
                if (event.error.has_value()) {
                    view.error = event.error->safe_detail();
                }
                observer(
                    aki::device::DeviceId{::heyaki::to_string(peer.device_id)},
                    view);
            });
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
    [[nodiscard]] std::optional<TrustDirections>
    trust_directions_for_key(const ::heyaki::DeviceEndpointKey& key) const {
        auto grants = node_.trust_grants_for(key);
        if (!grants) {
            return std::nullopt;
        }
        TrustDirections result;
        for (const auto& grant : *grants.value_if()) {
            if (grant.revoked)
                continue;
            if (grant.direction == ::heyaki::TrustGrantDirection::issued) {
                result.issued = true;
            } else {
                result.received = true;
            }
        }
        return result;
    }

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
    // profile_ 为非拥有指针（HostRuntime Impl 持有 profile 且声明先于
    // node_session，存续覆盖会话；热生效读记录用，M8-08）。
    std::unique_ptr<::heyaki::Runtime> runtime_;
    ::heyaki::Node node_;
    LocalProfile* profile_ = nullptr;
    aki::device::DeviceId local_id_{};
    std::atomic<bool> shutdown_done_{false};
    NodeSessionShutdownReport last_report_{};
};

}  // namespace aki::heyaki
