// 统一真实 Adapter（DEC-006 全映射；M3-08；设计第 8.1/8.3 节）。
//
// 将 M3-04~06 的分件（发现观察管道、peer_sessions diff 管道、NodeSession
// 消息面）组装为单一 HeyakiAdapter SPI 实现类：
//   - 出站：start/stop_discovery → LAN 发现观察管道启停；send_text_message →
//     aki.text 信封（peer_acked）；send_image_message → aki.image 信封
//     （peer_acked，M4-03——消息面仅 metadata + TransferId，RULE-05）；传输
//     四接口 M4 前签名语义（false + 记录，DEC-006：TransferId 一个会话不可
//     重复启动的语义随 M4 数据面落地）；
//   - 入站（EXEC-02：回调只做有界校验 + 经 sink 投递）：发现观察管道 →
//     on_device_discovered；peer_sessions diff → on_device_connected/
//     on_device_disconnected（presence，DM）/ on_connection_path_changed
//     （LatestMailbox，DM）；消息 inbound → 信封 type 分发（aki.text/
//     aki.image → on_message_received；未知 type / 解码失败 → 有界拒绝计数
//     可观测，M4-03）；ack → acked→on_message_delivered、失败四态→
//     on_message_send_failed。
//
// 断线重连是 Application 层职责（RULE-01 层向：heyaki/adapter 仅依赖领域
// 类型与 heyaki 会话面，不依赖 app/application——DEC-002 固定方向 UI→
// Application→Domain→Heyaki Adapter→Heyaki）：本 Adapter 经 SPI
// on_device_disconnected 如实上报断开，重连循环由组合根的
// ReconnectCoordinator 承载（main.cpp 7.5 装配：peer_sessions disconnected
// → stop_all 生命周期归宿主关闭钩子，M3-07 记录）。
//
// 会话归属解析（on_message_delivered/on_message_send_failed 的
// ConversationId）经注入回调（组合根提供 CM/MM 同方案解析器）——适配层
// 不承载 id 方案知识。
//
// RULE-10：heyaki 类型封死本层；对组合根公开面仅 SPI（aki 领域/std）。
// 线程契约：sink 回调在 executor timer / Node 上下文触发——消费方（RouterSink
// → Manager）有界校验 + 投递（EXEC-02）。
#pragma once

#include "heyaki/adapter/heyaki_adapter.hpp"
#include "heyaki/adapter/lan_discovery.hpp"
#include "heyaki/adapter/peer_sessions_pipeline.hpp"
#include "heyaki/adapter/local_identity.hpp"
#include "heyaki/session/runtime_node.hpp"

#include "conversation/codec/image_payload_codec.hpp"

#include <executor/executor.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace aki::heyaki {

class HeyakiNodeAdapter final : public aki::heyaki::HeyakiAdapter {
public:
    struct Options {
        LocalProfile* profile = nullptr;
        NodeSession* session = nullptr;
        // 对端设备 → 会话 id 解析（CM/MM 同方案；注入避免 id 方案入层）。
        std::function<aki::conversation::ConversationId(
            const aki::device::DeviceId&)>
            conversation_for;
        // presence/path 观察管道开关（宿主 smoke 置 false 保持确定性；
        // 真实部署 M4/M5 开启）。
        bool peer_observation = true;
    };

    HeyakiNodeAdapter(executor::Executor& executor, Options options)
        : options_(std::move(options)) {
        if (options_.profile == nullptr || options_.session == nullptr
            || !options_.conversation_for) {
            throw std::invalid_argument(
                "heyaki node adapter: profile/session/conversation resolver "
                "are required");
        }
        local_id_ = options_.session->local_id();
        discovery_ = std::make_unique<LanDiscoveryPipeline>(executor,
            *options_.session,
            [this](const aki::device::DiscoveredDevice& device) {
                deliver_discovered(device);
            });
        if (options_.peer_observation) {
            peer_pipeline_ = std::make_unique<PeerSessionPipeline>(executor,
                *options_.session,
                aki::heyaki::PeerSessionEvents{
                    .on_connected =
                        [this](const aki::device::DeviceId& peer) {
                            deliver_connected(peer);
                        },
                    .on_disconnected =
                        [this](const aki::device::DeviceId& peer) {
                            deliver_disconnected(peer);
                        },
                    .on_connection_path_changed =
                        [this](const aki::device::DeviceId& peer,
                            aki::device::ConnectionPath path) {
                            deliver_path_changed(peer, path);
                        },
                });
        }
        options_.session->set_message_handlers(
            [this](const aki::device::DeviceId& peer,
                const aki::conversation::MessageId& id,
                const std::string& type,
                const std::string& payload) {
                deliver_inbound(peer, id, type, payload);
            },
            [this](const aki::device::DeviceId& peer,
                const aki::conversation::MessageId& id,
                const std::string& event) { deliver_ack(peer, id, event); });
    }

    HeyakiNodeAdapter(const HeyakiNodeAdapter&) = delete;
    HeyakiNodeAdapter& operator=(const HeyakiNodeAdapter&) = delete;

    // 析构闭合（EXEC-01 步骤 1 等价纪律）：停发现/peer 观察管道 + 中和已
    // 登记的消息 handler（session 侧包装器对空 function 直接返回，见
    // runtime_node.hpp set_message_handlers）——handler 捕获 this，不中和则
    // 存活的 session 在本对象销毁后收到入站/回报即 use-after-free。
    ~HeyakiNodeAdapter() override {
        stop_discovery();
        if (peer_pipeline_ != nullptr) {
            peer_pipeline_->stop();
        }
        if (options_.session != nullptr) {
            options_.session->set_message_handlers({}, {});
        }
    }

    void set_sink(HeyakiAdapterSink* sink) noexcept { sink_ = sink; }

    // ---- 入站注入面（EXEC-02：有界校验 + sink 投递；与 FakeHeyakiAdapter
    // 的 inject_* 对称）——断线不在此层触发重连：重连是 Application 层职责
    // （组合根协调器），本类仅经 SPI 如实上报 on_device_disconnected。----

    void deliver_discovered(const aki::device::DiscoveredDevice& device) {
        if (sink_ != nullptr) {
            (void)sink_->on_device_discovered(device);
        }
    }

    void deliver_connected(const aki::device::DeviceId& peer) {
        if (sink_ != nullptr) {
            (void)sink_->on_device_connected(peer,
                aki::device::ConnectionPath::Lan);
        }
    }

    void deliver_disconnected(const aki::device::DeviceId& peer) {
        if (sink_ != nullptr) {
            (void)sink_->on_device_disconnected(peer);
        }
    }

    void deliver_path_changed(const aki::device::DeviceId& peer,
        aki::device::ConnectionPath path) {
        if (sink_ != nullptr) {
            (void)sink_->on_connection_path_changed(peer,
                aki::device::ConnectionPath::Unknown, path);
        }
    }

    // 入站信封分发（M4-03，设计 §6.1①/§8.1：信封 type 分发收敛在本层）：
    // aki.text → TextPayload；aki.image → codec 解码 → ImagePayload；未知
    // type 或解码失败 → 有界拒绝可见（不投递 sink、inbound_rejections 计数，
    // RULE-09——EXEC-02 回调线程只做有界校验 + 投递/拒绝，不解析重负载）。
    void deliver_inbound(const aki::device::DeviceId& peer,
        const aki::conversation::MessageId& id, const std::string& type,
        const std::string& payload) {
        if (sink_ == nullptr) {
            return;
        }
        aki::conversation::Message message;
        message.id = id;
        message.sender = peer;
        message.receiver = local_id_;
        message.state = aki::conversation::DeliveryState::Sent;  // MM 强制 Delivered
        if (type == "aki.text") {
            message.type = aki::conversation::MessageType::Text;
            message.payload = aki::conversation::TextPayload{payload};
        } else if (type
            == aki::conversation::codec::kAkiImageEnvelopeType) {
            const auto* data = reinterpret_cast<const std::byte*>(payload.data());
            const auto decoded = aki::conversation::codec::decode_image_payload(
                std::span<const std::byte>(data, payload.size()));
            if (!decoded.value.has_value()) {
                inbound_rejections_.fetch_add(1);  // 有界拒绝可见（不投递 sink）
                return;
            }
            message.type = aki::conversation::MessageType::Image;
            message.payload = std::move(*decoded.value);
        } else {
            inbound_rejections_.fetch_add(1);  // 未知信封 type：同上可见拒绝
            return;
        }
        (void)sink_->on_message_received(std::move(message));
    }

    void deliver_ack(const aki::device::DeviceId& peer,
        const aki::conversation::MessageId& id, const std::string& event) {
        if (sink_ == nullptr) {
            return;
        }
        const auto conversation = options_.conversation_for(peer);
        if (event == "acked") {
            (void)sink_->on_message_delivered(conversation, id);
        } else if (event == "failed") {
            (void)sink_->on_message_send_failed(conversation, id);
        }
        // queued：MM 语义已记录 Sent，无需重复推进。
    }

    // ---- 出站（HeyakiAdapter SPI）----

    [[nodiscard]] bool start_discovery(
        aki::device::DiscoveryMethod method) override {
        if (method != aki::device::DiscoveryMethod::LanDiscovery) {
            return false;  // M3 仅 LAN 扫描型来源（记录型来源分期，§8.1）
        }
        if (peer_pipeline_ != nullptr) {
            (void)peer_pipeline_->start(std::chrono::milliseconds{200});  // presence/path 观察随发现启停
        }
        return discovery_->start(std::chrono::milliseconds{200});
    }

    void stop_discovery() override {
        discovery_->stop();
        if (peer_pipeline_ != nullptr) {
            peer_pipeline_->stop();
        }
    }

    [[nodiscard]] bool send_text_message(const aki::device::DeviceId& to,
        const aki::conversation::MessageId& message_id,
        std::string_view text) override {
        if (to.empty() || message_id.empty() || text.empty()) {
            return false;  // 有界校验（EXEC-02 出站面）
        }
        return options_.session->send_text(to, message_id, text);
    }

    // 图片消息出站（M4-03，DEC-010①）：aki.image 信封经 NodeSession send_image
    //（MessageId 双射 + codec 编码 + TransferId 规范校验，任一失败 admission
    // false 可见）；图片本体不经本路径（RULE-05，传输面随 M4-04 接线）。
    [[nodiscard]] bool send_image_message(const aki::device::DeviceId& to,
        const aki::conversation::MessageId& message_id,
        const aki::transfer::FileMetadata& file,
        const aki::transfer::TransferId& transfer_id) override {
        if (to.empty() || message_id.empty() || file.name.empty()
            || transfer_id.empty()) {
            return false;  // 有界校验（EXEC-02 出站面）
        }
        return options_.session->send_image(to, message_id, file, transfer_id);
    }

    // 传输四接口（M4 前签名语义，DEC-006：文件数据链路 M4）：
    // admission false + TransferId 语义（一个 TransferId 一个会话）随 M4
    // 数据面落地；本版本不伪造进度/终态事件。source_path 签名随 M4-02 按
    // §7.1⑤ 固化，真实消费随 M4-04。
    [[nodiscard]] bool start_file_transfer(const aki::device::DeviceId& to,
        const aki::transfer::TransferId& transfer_id,
        const aki::transfer::FileMetadata& file,
        const std::filesystem::path& source_path) override {
        (void)to;
        (void)transfer_id;
        (void)file;
        (void)source_path;
        return false;  // M4
    }

    [[nodiscard]] bool pause_transfer(
        const aki::transfer::TransferId& transfer_id) override {
        (void)transfer_id;
        return false;  // M4
    }

    [[nodiscard]] bool resume_transfer(
        const aki::transfer::TransferId& transfer_id) override {
        (void)transfer_id;
        return false;  // M4
    }

    [[nodiscard]] bool cancel_transfer(
        const aki::transfer::TransferId& transfer_id) override {
        (void)transfer_id;
        return false;  // M4
    }

    // ---- 观测（EXEC-06）----

    [[nodiscard]] bool discovery_running() const noexcept {
        return discovery_ != nullptr && discovery_->running();
    }

    [[nodiscard]] std::uint64_t discovered_count() const noexcept {
        return discovery_ != nullptr ? discovery_->discovered_count() : 0;
    }

    // 入站有界拒绝计数（M4-03，设计 §6.1①：未知信封 type / 图片载荷解码
    // 失败——不投递 sink、经此可观测，RULE-09）。
    [[nodiscard]] std::uint64_t inbound_rejections() const noexcept {
        return inbound_rejections_.load();
    }

private:
    Options options_;
    aki::device::DeviceId local_id_;
    HeyakiAdapterSink* sink_ = nullptr;
    std::unique_ptr<LanDiscoveryPipeline> discovery_;
    std::unique_ptr<PeerSessionPipeline> peer_pipeline_;
    // 跨上下文计数：入站回调在 Node 上下文、读取在宿主/测试上下文（EXEC-06
    // 可观测；域计数不复刻 Executor 监控）。
    std::atomic<std::uint64_t> inbound_rejections_{0};
};

// SPI 同接口编译期断言（验收 ③：Fake 与真实 Adapter 同一接口契约；
// Fake 侧断言见 test_heyaki_adapter）。
static_assert(std::is_base_of_v<aki::heyaki::HeyakiAdapter, HeyakiNodeAdapter>,
    "HeyakiNodeAdapter must implement the M1 HeyakiAdapter SPI");

}  // namespace aki::heyaki
