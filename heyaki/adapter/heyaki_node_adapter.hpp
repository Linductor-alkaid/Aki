// 统一真实 Adapter（DEC-006 全映射；M3-08；设计第 8.1/8.3 节）。
//
// 将 M3-04~06 的分件（发现观察管道、peer_sessions diff 管道、NodeSession
// 消息面）组装为单一 HeyakiAdapter SPI 实现类：
//   - 出站：start/stop_discovery → LAN 发现观察管道启停；send_text_message →
//     aki.text 信封（peer_acked）；send_image_message → aki.image 信封
//     （peer_acked，M4-03——消息面仅 metadata + TransferId，RULE-05）；
//     start_file_transfer → heyaki push_file（M4-04 真实接线：wire 侧 heyaki
//     自读 source_path，root 经 Options 注入；进度/终态事件路由随 M4-05）；
//     pause/resume/cancel 仍 M4 前 false（M4-05）；
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
#include <mutex>
#include <set>
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
        // push_file 的对端逻辑根名（§7.1⑤：root 由组合根经存储配置注入，
        // 非 SPI 参数——Aki 侧固定使用会话默认文件根；接收侧 M4-05 配置
        // 同名接收根）。
        std::string push_root = "inbox";
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
        // 文件事件观察（M4-05，DEC-006 映射 7/DEC-012④）：Node 上下文回调 →
        // 八相位映射分发（EXEC-02 有界校验 + 投递）。析构中和（handler 捕获
        // this，与消息 handler 同纪律）。
        options_.session->set_file_event_observer(
            [this](const aki::device::DeviceId& peer,
                const NodeSession::FileTransferEventView& event) {
                deliver_file_event(peer, event);
            });
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
            options_.session->set_file_event_observer(nullptr);
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

    // 传输出站（M4-04 接线，DEC-006 映射 7/§7.1⑤）：start_file_transfer →
    // heyaki push_file（wire 侧 heyaki 自读源文件；进度/终态经
    // set_file_event_observer 回报，路由随 M4-05）。TransferId 须规范形式
    //（双射转换失败 admission false，DEC-011 ④）；source_path 不进入对端
    // 可见的 FileMetadata。pause/resume/cancel 仍为 M4 前 false 语义——
    // 接线随 M4-05（本项仅发送侧发起面）。
    [[nodiscard]] bool start_file_transfer(const aki::device::DeviceId& to,
        const aki::transfer::TransferId& transfer_id,
        const aki::transfer::FileMetadata& file,
        const std::filesystem::path& source_path) override {
        if (to.empty() || transfer_id.empty() || file.name.empty()
            || source_path.empty()) {
            return false;  // 有界校验（EXEC-02 出站面）
        }
        return options_.session->push_file(
            to, options_.push_root, file.name, source_path, transfer_id);
    }

    // ---- 传输出站（M4-05 接线，DEC-006 映射 7）：NodeSession 控制面透传 ----

    [[nodiscard]] bool pause_transfer(
        const aki::transfer::TransferId& transfer_id) override {
        return control_file_transfer(
            transfer_id, [this](const aki::device::DeviceId& peer,
                                const aki::transfer::TransferId& id) {
                return options_.session->pause_file_transfer(peer, id);
            });
    }

    [[nodiscard]] bool resume_transfer(
        const aki::transfer::TransferId& transfer_id) override {
        return control_file_transfer(
            transfer_id, [this](const aki::device::DeviceId& peer,
                                const aki::transfer::TransferId& id) {
                return options_.session->resume_file_transfer(peer, id);
            });
    }

    [[nodiscard]] bool cancel_transfer(
        const aki::transfer::TransferId& transfer_id) override {
        return control_file_transfer(
            transfer_id, [this](const aki::device::DeviceId& peer,
                                const aki::transfer::TransferId& id) {
                return options_.session->cancel_file_transfer(peer, id);
            });
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

    // ---- 文件事件入站分发（M4-05，DEC-006 映射 7/DEC-012④ 修订；EXEC-02：
    // 有界校验 + 投递——八相位映射为纯翻译，业务推进在 Manager 上下文）----
    //
    // 相位映射（§7.1⑤ 修订，按 pinned 源定论）：`probing`/`offered` 为
    // **发送端专属事件**（file_service.cpp 仅发送侧发出 :210/:252/:604）→
    // on_transfer_started（发送行——行由 StartTransferWork 先建、TM 以泵内
    // 已知行缓存合并推进，本处构造的 sender/receiver 不参与；行缺失时补建
    // sender=local）。**接收端首个事件是 `transferring`**（:1147/:1368，
    // 接收侧无 probing/offered）：未见 started 的 id 由首个 transferring/
    // verifying 承担接收行建行（on_transfer_started，Negotiating 行：
    // sender=peer、receiver=local、file.name=**剥根段** logical_name、
    // size=bytes_total）+ on_transfer_progress；后续 transferring/verifying →
    // on_transfer_progress。判向**不经 direction**（pinned 源：接收侧 push
    // 事件 direction 恒为 push :1147/:1368/:1441、cancelled 恒为 pull :380
    // ——DEC-012 风险④成立）：新行仅由入站 transferring/verifying 产生（恒
    // 接收行），probing/offered 恒发送行。

    void deliver_file_event(const aki::device::DeviceId& peer,
        const NodeSession::FileTransferEventView& event) {
        if (sink_ == nullptr) {
            return;
        }
        if (event.transfer.empty()) {
            file_event_rejections_.fetch_add(1);
            return;  // 有界校验（EXEC-02）
        }
        switch (static_cast<::heyaki::FileTransferPhase>(event.phase)) {
            case ::heyaki::FileTransferPhase::probing:
            case ::heyaki::FileTransferPhase::offered: {
                if (event.logical_name.empty()
                    || event.logical_name.size()
                        > ::heyaki::max_logical_name_bytes) {
                    file_event_rejections_.fetch_add(1);
                    return;  // 有界校验（建行载荷）
                }
                aki::transfer::Transfer row;
                row.id = event.transfer;
                row.file = aki::transfer::FileMetadata{event.logical_name,
                    event.bytes_total, std::string{}, std::string{}};
                row.transferred = event.bytes_done;
                row.total = event.bytes_total;
                row.state = aki::transfer::TransferState::Negotiating;
                row.sender = local_id_;
                row.receiver = peer;
                if (sink_->on_transfer_started(std::move(row))) {
                    note_started(event.transfer.value);
                }
                return;
            }
            case ::heyaki::FileTransferPhase::transferring:
            case ::heyaki::FileTransferPhase::verifying: {
                // 接收侧首个事件即 transferring（pinned 源）：未见 started 的
                // id 由本事件建行（剥根段 logical_name——wire manifest
                // logical_name = join(root, name) :556，heyaki 落盘为剥根段
                // 相对名 :1075 而事件携带含根前缀 :1147/:1368/:1441，行名须
                // 与供源推导（file_store receive_source_path）一致）。
                if (!started_seen(event.transfer.value)) {
                    if (event.logical_name.empty()
                        || event.logical_name.size()
                            > ::heyaki::max_logical_name_bytes) {
                        file_event_rejections_.fetch_add(1);
                        return;  // 有界校验（建行载荷）
                    }
                    aki::transfer::Transfer row;
                    row.id = event.transfer;
                    row.file =
                        aki::transfer::FileMetadata{strip_wire_root(
                                                         event.root,
                                                         event.logical_name),
                            event.bytes_total, std::string{}, std::string{}};
                    row.transferred = event.bytes_done;
                    row.total = event.bytes_total;
                    row.state = aki::transfer::TransferState::Negotiating;
                    row.sender = peer;
                    row.receiver = local_id_;  // 接收行建行（DEC-012②）
                    if (sink_->on_transfer_started(std::move(row))) {
                        note_started(event.transfer.value);
                    }
                }
                (void)sink_->on_transfer_progress(
                    event.transfer, event.bytes_done, event.bytes_total);
                return;
            }
            case ::heyaki::FileTransferPhase::paused:
                (void)sink_->on_transfer_paused(event.transfer);
                return;
            case ::heyaki::FileTransferPhase::committed:
                release_started(event.transfer.value);
                (void)sink_->on_transfer_completed(
                    event.transfer, aki::transfer::TransferState::Completed);
                return;
            case ::heyaki::FileTransferPhase::failed:
                release_started(event.transfer.value);
                (void)sink_->on_transfer_completed(
                    event.transfer, aki::transfer::TransferState::Failed);
                return;
            case ::heyaki::FileTransferPhase::cancelled:
                // 接收侧 cancelled 恒为 pull（:380）——判向不经 direction，
                // 终态事件不建行（行缺失由 owner 拒绝可见，已知边角）。
                release_started(event.transfer.value);
                (void)sink_->on_transfer_completed(
                    event.transfer, aki::transfer::TransferState::Cancelled);
                return;
        }
        file_event_rejections_.fetch_add(1);  // 未知相位：可见拒绝
    }

    // 文件事件有界拒绝计数（M4-05，RULE-09）。
    [[nodiscard]] std::uint64_t file_event_rejections() const noexcept {
        return file_event_rejections_.load();
    }

    // started 记名簿容量溢出（接收建行跳过、计数可见——RULE-09）。
    [[nodiscard]] std::uint64_t started_registry_overflows() const noexcept {
        return started_registry_overflows_.load();
    }

private:
    // started 记名簿（M4-05 修订）：已向 sink 投递 started 的 TransferId——
    // 接收侧首个 transferring/verifying 据此一次性建行。容量有界（终态移除；
    // 满即跳过建行、计数可见——进度对未知行被 owner 拒绝可见，RULE-09）。
    // deliver_file_event 在 Node 上下文触发，互斥保护跨上下文安全。
    static constexpr std::size_t kMaxStartedIds = 1024;

    [[nodiscard]] bool started_seen(const std::string& id) {
        std::lock_guard<std::mutex> guard(started_mutex_);
        return started_ids_.count(id) != 0;
    }

    void note_started(const std::string& id) {
        std::lock_guard<std::mutex> guard(started_mutex_);
        if (started_ids_.size() < kMaxStartedIds) {
            started_ids_.insert(id);
        } else {
            started_registry_overflows_.fetch_add(1);
        }
    }

    void release_started(const std::string& id) {
        std::lock_guard<std::mutex> guard(started_mutex_);
        (void)started_ids_.erase(id);
    }

    // wire 根段剥离（§7.1⑤ 修订）：wire manifest logical_name =
    // join(root, name)（file_service.cpp :556），heyaki 落盘/接收根相对名为
    // 剥根段形态（:1075），而事件携带含根前缀（:1147/:1368/:1441）——行名
    // 与供源推导（file_store receive_source_path）须用剥根段相对名。前缀
    // 不符（发送端 rootless 事件）原样返回。
    [[nodiscard]] static std::string strip_wire_root(
        const std::string& root, const std::string& logical_name) {
        if (root.empty() || logical_name.size() <= root.size() + 1U) {
            return logical_name;
        }
        if (logical_name.compare(0, root.size() + 1U, root + "/") != 0) {
            return logical_name;
        }
        return logical_name.substr(root.size() + 1U);
    }

    Options options_;
    aki::device::DeviceId local_id_;
    HeyakiAdapterSink* sink_ = nullptr;
    std::unique_ptr<LanDiscoveryPipeline> discovery_;
    std::unique_ptr<PeerSessionPipeline> peer_pipeline_;
    // 跨上下文计数：入站回调在 Node 上下文、读取在宿主/测试上下文（EXEC-06
    // 可观测；域计数不复刻 Executor 监控）。
    std::atomic<std::uint64_t> inbound_rejections_{0};
    std::atomic<std::uint64_t> file_event_rejections_{0};
    std::atomic<std::uint64_t> started_registry_overflows_{0};
    std::mutex started_mutex_;
    std::set<std::string> started_ids_;

    // 传输控制面公共路径（M4-05）：TransferId 规范转换 + 对已建文件会话的
    // 对端遍历尝试（控制 SPI 无 peer 参数；单一活跃对端为主用形态，多对端
    // 同 id 冲突由 TransferId 全局唯一性排除）；任一失败 admission false
    // 可见（RULE-09）。
    [[nodiscard]] bool control_file_transfer(
        const aki::transfer::TransferId& transfer_id,
        const std::function<bool(const aki::device::DeviceId&,
            const aki::transfer::TransferId&)>& invoke) {
        if (transfer_id.empty()) {
            return false;  // 有界校验（EXEC-02 出站面）
        }
        for (const auto& peer : options_.session->known_peers()) {
            if (invoke(peer, transfer_id)) {
                return true;
            }
        }
        return false;
    }
};

// SPI 同接口编译期断言（验收 ③：Fake 与真实 Adapter 同一接口契约；
// Fake 侧断言见 test_heyaki_adapter）。
static_assert(std::is_base_of_v<aki::heyaki::HeyakiAdapter, HeyakiNodeAdapter>,
    "HeyakiNodeAdapter must implement the M1 HeyakiAdapter SPI");

}  // namespace aki::heyaki
