// M3-08（评审补证）：统一真实 Adapter SPI 与入站注入面单测（网络无关）。
//
// 覆盖（工程规范 §7「可以运行」证据等级——Adapter 类在测试中真实构造并执行）：
//   - 构造校验：profile/session/conversation 解析器缺一即 invalid_argument；
//   - 出站 SPI：传输四接口 M4 前 false；send_text_message 有界校验（空载荷
//     拒绝）与不可达 peer admission false；发现来源校验（M3 仅 LanDiscovery）；
//   - 入站注入面（EXEC-02）：deliver_* → sink 分发字段逐项断言（与
//     FakeHeyakiAdapter 的 inject_* 对称）；未接 sink 时静默不投递；
//   - 析构闭合（评审修正回归守卫）：deliver_disconnected 后销毁——不再有
//     未登记的 submit_cancellable 循环，owner.shutdown fully_stopped。
#include "app/lifecycle/executor_owner.hpp"
#include "conversation/codec/image_payload_codec.hpp"
#include "heyaki/adapter/heyaki_node_adapter.hpp"
#include "heyaki/adapter/local_identity.hpp"
#include "heyaki/session/runtime_node.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using aki::app::ExecutorOwner;
using aki::app::ExecutorOwnerOptions;
using aki::conversation::ConversationId;
using aki::conversation::DeliveryState;
using aki::conversation::MessageId;
using aki::device::ConnectionPath;
using aki::device::DeviceId;
using aki::device::DiscoveryMethod;
using aki::heyaki::HeyakiNodeAdapter;
using aki::heyaki::LocalProfile;
using aki::heyaki::NodeSession;

int g_counter = 0;

std::string temp_root(const std::string& tag) {
    auto path = std::filesystem::temp_directory_path()
        / ("aki-node-adapter-" + tag + "-"
            + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())
            + "-" + std::to_string(++g_counter));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return path.string();
}

// 节点域（独立 profile + 独立测试 ExecutorOwner + 借用 Runtime，沿 M3-04 形态）。
struct NodeDomain {
    explicit NodeDomain(const std::string& root)
        : profile(LocalProfile::open(root)),
          executor_options([] {
              ExecutorOwnerOptions options;
              options.executor_config.min_threads = 2;
              options.executor_config.max_threads = 6;
              return options;
          }()),
          owner(executor_options) {
        if (!owner.initialize()) {
            throw std::runtime_error("node domain: executor initialize failed");
        }
        session.emplace(
            NodeSession::create(owner.executor(), {.profile = &profile}));
    }

    LocalProfile profile;
    ExecutorOwnerOptions executor_options;
    ExecutorOwner owner;
    std::optional<NodeSession> session;
};

// 录制 sink：SPI 分发面断言载体（全部 10 方法）。
struct RecordingSink final : aki::heyaki::HeyakiAdapterSink {
    std::vector<aki::device::DiscoveredDevice> discovered;
    std::vector<std::pair<DeviceId, ConnectionPath>> connected;
    std::vector<DeviceId> disconnected;
    std::vector<aki::conversation::Message> received;
    std::vector<std::pair<ConversationId, MessageId>> delivered;
    std::vector<std::pair<ConversationId, MessageId>> send_failed;
    std::vector<std::tuple<DeviceId, ConnectionPath, ConnectionPath>>
        path_changed;
    std::vector<aki::transfer::TransferId> paused;  // M4-05 第 11 方法
    // 传输面记录（M4-05 八相位路由断言载体）。
    std::vector<aki::transfer::Transfer> started;
    std::vector<std::tuple<aki::transfer::TransferId, std::uint64_t,
        std::uint64_t>>
        progress;
    std::vector<std::pair<aki::transfer::TransferId,
        aki::transfer::TransferState>>
        completed;

    bool on_device_discovered(aki::device::DiscoveredDevice device) override {
        discovered.push_back(std::move(device));
        return true;
    }
    bool on_device_connected(DeviceId device, ConnectionPath path) override {
        connected.emplace_back(std::move(device), path);
        return true;
    }
    bool on_device_disconnected(DeviceId device) override {
        disconnected.push_back(std::move(device));
        return true;
    }
    bool on_message_received(aki::conversation::Message message) override {
        received.push_back(std::move(message));
        return true;
    }
    bool on_message_delivered(ConversationId conversation,
        MessageId message) override {
        delivered.emplace_back(std::move(conversation), std::move(message));
        return true;
    }
    bool on_message_send_failed(ConversationId conversation,
        MessageId message) override {
        send_failed.emplace_back(std::move(conversation), std::move(message));
        return true;
    }
    bool on_transfer_started(aki::transfer::Transfer transfer) override {
        started.push_back(std::move(transfer));
        return true;
    }
    bool on_transfer_progress(aki::transfer::TransferId transfer,
        std::uint64_t transferred, std::uint64_t total) override {
        progress.emplace_back(std::move(transfer), transferred, total);
        return true;
    }
    bool on_transfer_completed(aki::transfer::TransferId transfer,
        aki::transfer::TransferState final_state) override {
        completed.emplace_back(std::move(transfer), final_state);
        return true;
    }
    bool on_transfer_paused(aki::transfer::TransferId transfer) override {
        paused.push_back(std::move(transfer));
        return true;
    }
    bool on_connection_path_changed(DeviceId device, ConnectionPath from,
        ConnectionPath to) override {
        path_changed.emplace_back(std::move(device), from, to);
        (void)from;
        return true;
    }
    bool on_pairing_completed(DeviceId device, bool success,
        std::string_view detail) override {
        pairing_results.emplace_back(PairingResult{
            std::move(device), success, std::string(detail)});
        return true;
    }

    struct PairingResult {
        DeviceId device;
        bool success = false;
        std::string detail;
    };
    std::vector<PairingResult> pairing_results;
};

HeyakiNodeAdapter::Options valid_options(NodeDomain& domain,
    bool peer_observation = false) {
    HeyakiNodeAdapter::Options options;
    options.profile = &domain.profile;
    options.session = &*domain.session;
    options.conversation_for =
        [](const DeviceId& remote) {
            return ConversationId{std::string{"conv-"} + remote.value};
        };
    options.peer_observation = peer_observation;
    return options;
}

}  // namespace

TEST_CASE("HeyakiNodeAdapter construction validates required wiring",
    "[unit][heyaki_node_adapter]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    NodeDomain domain(temp_root("ctor"));

    auto options = valid_options(domain);
    options.profile = nullptr;
    REQUIRE_THROWS_AS(HeyakiNodeAdapter(owner.executor(), options),
        std::invalid_argument);

    options = valid_options(domain);
    options.session = nullptr;
    REQUIRE_THROWS_AS(HeyakiNodeAdapter(owner.executor(), options),
        std::invalid_argument);

    options = valid_options(domain);
    options.conversation_for = nullptr;
    REQUIRE_THROWS_AS(HeyakiNodeAdapter(owner.executor(), options),
        std::invalid_argument);

    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("HeyakiNodeAdapter outbound SPI validates and reports honestly",
    "[unit][heyaki_node_adapter]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    NodeDomain domain(temp_root("outbound"));
    HeyakiNodeAdapter adapter{owner.executor(), valid_options(domain)};

    // 传输四接口：M4 前签名语义 false（DEC-006，不伪造事件）；source_path
    // 签名随 M4-02 §7.1⑤ 固化。
    aki::transfer::FileMetadata file{"model.gguf", 1024,
        "application/octet-stream", ""};
    REQUIRE_FALSE(adapter.start_file_transfer(
        DeviceId{"peer-x"}, aki::transfer::TransferId{"t-1"}, file,
        std::filesystem::path{"payloads/model.gguf"}));
    REQUIRE_FALSE(adapter.pause_transfer(aki::transfer::TransferId{"t-1"}));
    REQUIRE_FALSE(adapter.resume_transfer(aki::transfer::TransferId{"t-1"}));
    REQUIRE_FALSE(adapter.cancel_transfer(aki::transfer::TransferId{"t-1"}));

    // send_text_message 有界校验（EXEC-02 出站面）。
    REQUIRE_FALSE(adapter.send_text_message(
        DeviceId{}, MessageId{"m-1"}, "hello"));
    REQUIRE_FALSE(adapter.send_text_message(
        DeviceId{"peer-x"}, MessageId{}, "hello"));
    REQUIRE_FALSE(adapter.send_text_message(
        DeviceId{"peer-x"}, MessageId{"m-1"}, ""));

    // 不可达 peer（无发现端点）→ admission false（确定性，网络无关）。
    REQUIRE_FALSE(adapter.send_text_message(
        DeviceId{"hy1_00000000000000000000000000000000000000000000000000000000000000"},
        MessageId{"m-1"}, "hello"));

    // 发现来源校验：M3 仅 LanDiscovery 扫描型。
    REQUIRE_FALSE(adapter.start_discovery(DiscoveryMethod::Manual));
    REQUIRE_FALSE(adapter.start_discovery(DiscoveryMethod::KnownDevice));

    // LAN 发现启停回转（观察管道启停；接口缺失环境 start 不予断言——补跑
    // 条件沿 M3-04 登记）。
    if (domain.session->has_lan_interfaces()) {
        REQUIRE(adapter.start_discovery(DiscoveryMethod::LanDiscovery));
        REQUIRE(adapter.discovery_running());
        adapter.stop_discovery();
        REQUIRE_FALSE(adapter.discovery_running());
    }

    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("HeyakiNodeAdapter inbound injection dispatches to the sink",
    "[unit][heyaki_node_adapter]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    NodeDomain domain(temp_root("inbound"));
    HeyakiNodeAdapter adapter{owner.executor(), valid_options(domain)};

    RecordingSink sink;
    adapter.set_sink(&sink);
    const DeviceId peer{"peer-b"};

    // M5-04（DEC-015）：初连路径由 diff 映射随事件携带（宿主不再硬编码
    // Lan——投递面按传入值转发）。
    adapter.deliver_connected(peer, ConnectionPath::Lan);
    REQUIRE(sink.connected.size() == 1);
    REQUIRE(sink.connected.front().first == peer);
    REQUIRE(sink.connected.front().second == ConnectionPath::Lan);

    adapter.deliver_inbound(peer, MessageId{"m-1"}, "aki.text", "hello");
    REQUIRE(sink.received.size() == 1);
    {
        const auto& message = sink.received.front();
        REQUIRE(message.id == MessageId{"m-1"});
        REQUIRE(message.sender == peer);
        REQUIRE(message.receiver == domain.profile.identity().id);
        REQUIRE(message.type == aki::conversation::MessageType::Text);
        REQUIRE(message.state == DeliveryState::Sent);
        const auto* text =
            std::get_if<aki::conversation::TextPayload>(&message.payload);
        REQUIRE(text != nullptr);
        REQUIRE(text->text == "hello");
    }

    adapter.deliver_ack(peer, MessageId{"m-1"}, "acked");
    REQUIRE(sink.delivered.size() == 1);
    REQUIRE(sink.delivered.front()
        == std::make_pair(ConversationId{"conv-peer-b"}, MessageId{"m-1"}));

    adapter.deliver_ack(peer, MessageId{"m-2"}, "failed");
    REQUIRE(sink.send_failed.size() == 1);
    REQUIRE(sink.send_failed.front()
        == std::make_pair(ConversationId{"conv-peer-b"}, MessageId{"m-2"}));

    // queued：MM 语义已记录 Sent，不重复推进。
    adapter.deliver_ack(peer, MessageId{"m-3"}, "queued");
    REQUIRE(sink.delivered.size() == 1);
    REQUIRE(sink.send_failed.size() == 1);

    adapter.deliver_disconnected(peer);
    REQUIRE(sink.disconnected.size() == 1);
    REQUIRE(sink.disconnected.front() == peer);

    adapter.deliver_path_changed(peer, ConnectionPath::P2p);
    REQUIRE(sink.path_changed.size() == 1);
    REQUIRE(std::get<0>(sink.path_changed.front()) == peer);
    REQUIRE(std::get<2>(sink.path_changed.front()) == ConnectionPath::P2p);

    // 无关 sink 事件零串扰：discovered/transfer 面未被本用例驱动。
    REQUIRE(sink.discovered.empty());

    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("HeyakiNodeAdapter dispatches image envelopes and rejects bounded "
    "inbound failures visibly",
    "[unit][heyaki_node_adapter]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    NodeDomain domain(temp_root("image"));
    HeyakiNodeAdapter adapter{owner.executor(), valid_options(domain)};
    RecordingSink sink;
    adapter.set_sink(&sink);
    const DeviceId peer{"peer-b"};

    // 合法 aki.image 载荷（DEC-010① 冻结字段号）：Image typed 消息投递 sink，
    // 消息面仅 metadata + TransferId（RULE-05）。transfer_id 用 heyaki 自身
    // 编码器生成（规范形式按构造成立，31 字符 hyt1_ 串）。
    const aki::transfer::FileMetadata media{"photo.png", 2048, "image/png", ""};
    const aki::transfer::TransferId transfer_id{::heyaki::to_string(
        ::heyaki::TransferId(::heyaki::TransferId::Storage{
            std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24},
            std::byte{0x25}, std::byte{0x26}, std::byte{0x27}, std::byte{0x28},
            std::byte{0x29}, std::byte{0x2a}, std::byte{0x2b}, std::byte{0x2c},
            std::byte{0x2d}, std::byte{0x2e}, std::byte{0x2f}, std::byte{0x30}}))};
    REQUIRE(transfer_id.value.size() == 31);
    const auto encoded =
        aki::conversation::codec::encode_image_payload({media, transfer_id});
    REQUIRE(encoded.has_value());
    const std::string payload(reinterpret_cast<const char*>(encoded->data()),
        encoded->size());
    adapter.deliver_inbound(peer, MessageId{"m-img"},
        std::string{aki::conversation::codec::kAkiImageEnvelopeType}, payload);
    REQUIRE(sink.received.size() == 1);
    {
        const auto& message = sink.received.front();
        REQUIRE(message.type == aki::conversation::MessageType::Image);
        REQUIRE(message.id == MessageId{"m-img"});
        REQUIRE(message.state == DeliveryState::Sent);  // MM 强制 Delivered
        const auto* image =
            std::get_if<aki::conversation::ImagePayload>(&message.payload);
        REQUIRE(image != nullptr);
        REQUIRE(image->media == media);
        REQUIRE(image->transfer_id == transfer_id);
    }
    REQUIRE(adapter.inbound_rejections() == 0);

    // 解码失败（缺必填字段的 wire）→ 不投递 sink、拒绝计数可见（RULE-09）。
    adapter.deliver_inbound(peer, MessageId{"m-bad"},
        std::string{aki::conversation::codec::kAkiImageEnvelopeType},
        std::string{reinterpret_cast<const char*>(encoded->data()), 1});
    REQUIRE(sink.received.size() == 1);
    REQUIRE(adapter.inbound_rejections() == 1);

    // 未知信封 type → 同上有界拒绝（设计 §6.1①）。
    adapter.deliver_inbound(peer, MessageId{"m-unknown"}, "aki.video", "x");
    REQUIRE(sink.received.size() == 1);
    REQUIRE(adapter.inbound_rejections() == 2);

    // 无 sink：静默丢弃不崩溃（EXEC-02 观察者缺失不阻塞管道）。
    {
        HeyakiNodeAdapter sinkless{owner.executor(), valid_options(domain)};
        sinkless.deliver_inbound(peer, MessageId{"m-img"},
            std::string{aki::conversation::codec::kAkiImageEnvelopeType},
            payload);
        REQUIRE(sinkless.inbound_rejections() == 0);  // 未接 sink：未走到分发
    }

    // send_image_message 有界校验（EXEC-02 出站面）：空载荷拒绝。
    REQUIRE_FALSE(adapter.send_image_message(DeviceId{}, MessageId{"m-1"},
        media, transfer_id));
    REQUIRE_FALSE(adapter.send_image_message(peer, MessageId{}, media,
        transfer_id));
    REQUIRE_FALSE(adapter.send_image_message(
        peer, MessageId{"m-1"}, aki::transfer::FileMetadata{}, transfer_id));
    REQUIRE_FALSE(adapter.send_image_message(peer, MessageId{"m-1"}, media,
        aki::transfer::TransferId{}));
    // 不可达 peer（无发现端点）→ admission false（确定性，网络无关）。
    REQUIRE_FALSE(adapter.send_image_message(
        DeviceId{"hy1_00000000000000000000000000000000000000000000000000000000000000"},
        MessageId{"m-1"}, media, transfer_id));

    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("HeyakiNodeAdapter maps file event phases to sink methods "
    "(DEC-006 mapping 7 / DEC-012)",
    "[unit][heyaki_node_adapter]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    NodeDomain domain(temp_root("fileevent"));
    HeyakiNodeAdapter adapter{owner.executor(), valid_options(domain)};
    RecordingSink sink;
    adapter.set_sink(&sink);
    const DeviceId peer{"peer-b"};

    // 视图工厂（direction/logical_name 按 pinned 源真实取值）：发送端
    // probing/offered 恒为 push 且 logical_name 为发送侧原名（file_service.cpp
    // :210/:252/:604 传 sender.logical_name）；接收端 transferring/committed
    // 恒为 push（:1147/:1368/:1441，pull_initiated=false）且 logical_name 为
    // 含根前缀的 manifest 形式（= join(root, name)，:556）；接收端 cancelled
    // 恒为 pull（:380）。
    const int push =
        static_cast<int>(::heyaki::FileTransferDirection::push);
    const int pull =
        static_cast<int>(::heyaki::FileTransferDirection::pull);
    auto view = [](const char* id, int phase, std::uint64_t done,
        std::uint64_t total, std::string logical_name, int direction) {
        NodeSession::FileTransferEventView event;
        event.transfer = aki::transfer::TransferId{id};
        event.direction = direction;
        event.phase = phase;
        event.root = "inbox";
        event.logical_name = std::move(logical_name);
        event.bytes_done = done;
        event.bytes_total = total;
        return event;
    };
    const int probing = static_cast<int>(::heyaki::FileTransferPhase::probing);
    const int offered = static_cast<int>(::heyaki::FileTransferPhase::offered);
    const int transferring =
        static_cast<int>(::heyaki::FileTransferPhase::transferring);
    const int verifying =
        static_cast<int>(::heyaki::FileTransferPhase::verifying);
    const int paused_phase =
        static_cast<int>(::heyaki::FileTransferPhase::paused);
    const int committed =
        static_cast<int>(::heyaki::FileTransferPhase::committed);
    const int failed = static_cast<int>(::heyaki::FileTransferPhase::failed);
    const int cancelled =
        static_cast<int>(::heyaki::FileTransferPhase::cancelled);

    // probing/offered（发送端专属事件）：started 发送行（sender=local；
    // 判向不经 direction——事件按协议仅发生于本端 push 侧）。
    adapter.deliver_file_event(
        peer, view("t-out", probing, 0, 0, "docs/model.bin", push));
    adapter.deliver_file_event(
        peer, view("t-out", offered, 0, 4096, "docs/model.bin", push));
    REQUIRE(sink.started.size() == 2);
    {
        const auto& row = sink.started.front();
        REQUIRE(row.id == aki::transfer::TransferId{"t-out"});
        REQUIRE(row.state == aki::transfer::TransferState::Negotiating);
        REQUIRE(row.sender == domain.profile.identity().id);
        REQUIRE(row.receiver == peer);
        REQUIRE(row.file.name == "docs/model.bin");
    }

    // 接收侧首个事件即 transferring（pinned 源：接收端无 probing/offered）
    // ——尽管 direction==push（真实形态），未见 started 的 id 由本事件建行：
    // 接收行 Negotiating、sender=peer/receiver=local、file.name=剥根段
    //（"inbox/docs/model.bin" → "docs/model.bin"，与 heyaki 落盘相对名一致）
    // + 首个进度（回归陷阱：建行与判向不得依赖 direction）。
    adapter.deliver_file_event(peer,
        view("t-rx", transferring, 0, 4096, "inbox/docs/model.bin", push));
    REQUIRE(sink.started.size() == 3);
    {
        const auto& row = sink.started.back();
        REQUIRE(row.id == aki::transfer::TransferId{"t-rx"});
        REQUIRE(row.state == aki::transfer::TransferState::Negotiating);
        REQUIRE(row.sender == peer);
        REQUIRE(row.receiver == domain.profile.identity().id);
        REQUIRE(row.file.name == "docs/model.bin");
        REQUIRE(row.file.size_bytes == 4096);
        REQUIRE(row.transferred == 0);
    }
    // 后续 transferring/verifying：仅进度（不重复建行）。
    adapter.deliver_file_event(peer,
        view("t-rx", transferring, 1024, 4096, "inbox/docs/model.bin", push));
    adapter.deliver_file_event(peer,
        view("t-rx", verifying, 4096, 4096, "inbox/docs/model.bin", push));
    REQUIRE(sink.started.size() == 3);
    REQUIRE(sink.progress.size() == 3);
    REQUIRE(std::get<1>(sink.progress.front()) == 0);
    REQUIRE(std::get<1>(sink.progress.back()) == 4096);

    // paused → 第 11 方法。
    adapter.deliver_file_event(peer,
        view("t-rx", paused_phase, 1024, 4096, "inbox/docs/model.bin", push));
    REQUIRE(sink.paused.size() == 1);
    REQUIRE(sink.paused.front() == aki::transfer::TransferId{"t-rx"});

    // committed/failed/cancelled → on_transfer_completed(终态)；cancelled
    // 恒为 pull（:380）——判向不经 direction，终态事件不建行。
    adapter.deliver_file_event(peer,
        view("t-rx", committed, 4096, 4096, "inbox/docs/model.bin", push));
    adapter.deliver_file_event(peer,
        view("t-rx", failed, 0, 4096, "inbox/docs/model.bin", push));
    adapter.deliver_file_event(peer,
        view("t-rx", cancelled, 0, 4096, "inbox/docs/model.bin", pull));
    REQUIRE(sink.completed.size() == 3);
    REQUIRE(sink.completed[0].second == aki::transfer::TransferState::Completed);
    REQUIRE(sink.completed[1].second == aki::transfer::TransferState::Failed);
    REQUIRE(sink.completed[2].second == aki::transfer::TransferState::Cancelled);

    // 终态先于任何 transferring（接收侧早取消）：仅终态投递、不建行（行
    // 缺失由 owner 拒绝可见——已知边角，不静默伪造）。
    adapter.deliver_file_event(
        peer, view("t-early", cancelled, 0, 16, "inbox/x.bin", pull));
    REQUIRE(sink.started.size() == 3);
    REQUIRE(sink.completed.size() == 4);

    // 有界拒绝（RULE-09）：接收首事件空 logical_name 建行载荷拒绝 + 未知
    // 相位拒绝，均不投递 sink、计数可见。
    adapter.deliver_file_event(
        peer, view("t-bad", transferring, 0, 4096, std::string{}, push));
    adapter.deliver_file_event(
        peer, view("t-bad2", 200, 0, 4096, "inbox/x.bin", push));
    REQUIRE(sink.started.size() == 3);
    REQUIRE(sink.progress.size() == 3);
    REQUIRE(adapter.file_event_rejections() == 2);
    // 常规事件面不触达记名簿容量（终态移除；溢出恒 0——容量路径另有界）。
    REQUIRE(adapter.started_registry_overflows() == 0);

    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("HeyakiNodeAdapter without a sink drops injections silently",
    "[unit][heyaki_node_adapter]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    NodeDomain domain(temp_root("nosink"));
    HeyakiNodeAdapter adapter{owner.executor(), valid_options(domain)};

    // sink 未接：EXEC-02 有界校验后丢弃（观察者缺失不阻塞管道），不崩溃。
    adapter.deliver_discovered(aki::device::DiscoveredDevice{});
    adapter.deliver_connected(DeviceId{"peer-b"}, ConnectionPath::Lan);
    adapter.deliver_disconnected(DeviceId{"peer-b"});
    adapter.deliver_path_changed(DeviceId{"peer-b"}, ConnectionPath::Relay);
    adapter.deliver_inbound(DeviceId{"peer-b"}, MessageId{"m-1"}, "aki.text", "hello");
    adapter.deliver_ack(DeviceId{"peer-b"}, MessageId{"m-1"}, "acked");

    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("HeyakiNodeAdapter destructor closes the session handler path "
    "(disconnect cleanup regression guard)",
    "[unit][heyaki_node_adapter]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    NodeDomain domain(temp_root("dtor"));

    {
        HeyakiNodeAdapter adapter{owner.executor(), valid_options(domain)};
        adapter.deliver_disconnected(DeviceId{"peer-b"});
        // 作用域结束：析构停管道 + 中和 session 消息 handler（this 捕获
        // 生命周期闭合）。修正前该处会启动未登记的 submit_cancellable
        // 重连循环——stop_all 无法取消、shutdown 后仍触碰 session（UAF）。
    }

    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}
