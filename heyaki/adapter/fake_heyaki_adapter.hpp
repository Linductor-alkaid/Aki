// FakeHeyakiAdapter（DEC-002）：测试与冒烟宿主用的假实现。
// 以 inject_* 编程式注入设计第 10 节 9 类入站事件；注入路径即 EXEC-02 回调路径
// （有界校验 + 投递，结果经返回值可见），不做任何真实 I/O，不创建线程或队列。
#pragma once

#include "heyaki/adapter/heyaki_adapter.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aki::heyaki {

// 线程契约：Fake 自身不同步；inject_* 与出站调用由宿主串行化（EXEC-02 的回调
// 线程纪律由宿主保证）。测试经 pinned executor 的 future 同步后并发安全。
class FakeHeyakiAdapter final : public HeyakiAdapter {
public:
    struct SentText {
        aki::device::DeviceId to;
        aki::conversation::MessageId message_id;
        std::string text;
    };

    // 图片消息出站记录（M4-03；消息面仅 metadata + TransferId，RULE-05）。
    struct SentImage {
        aki::device::DeviceId to;
        aki::conversation::MessageId message_id;
        aki::transfer::FileMetadata file;
        aki::transfer::TransferId transfer_id;
    };

    struct TransferCommand {
        enum class Kind { Start, Pause, Resume, Cancel };
        Kind kind = Kind::Start;
        aki::device::DeviceId peer;  // 仅 Start 时填充。
        aki::transfer::TransferId transfer_id;
        // M4-04（M4-02 登记的路径真实消费落地）：Start 命令记录发送侧本地
        // 路径供断言——Fake 本体不做真实 I/O（DEC-002 纪律不变），归档读取
        // 由 app 层经 TransferIo 承载面执行。
        std::filesystem::path source_path;
    };

    // Sink 由应用侧（Manager / 测试桥接）持有并提供，生命周期由调用方保证。
    void set_sink(HeyakiAdapterSink* sink) noexcept { sink_ = sink; }
    [[nodiscard]] HeyakiAdapterSink* sink() const noexcept { return sink_; }

    // ---- HeyakiAdapter 出站（应用 → Adapter）----

    bool start_discovery(aki::device::DiscoveryMethod method) override {
        discovery_running_ = true;
        discovery_methods_.push_back(method);
        return true;
    }

    void stop_discovery() override { discovery_running_ = false; }

    bool send_text_message(const aki::device::DeviceId& to,
        const aki::conversation::MessageId& message_id, std::string_view text) override {
        // 有界校验（EXEC-02）：空标识 / 空文本直接拒绝，不进入业务路径。
        if (to.empty() || message_id.empty() || text.empty()) {
            return false;
        }
        sent_texts_.push_back(SentText{to, message_id, std::string(text)});
        return true;
    }

    // 图片消息出站（M4-03，设计 §6.1/§8.1；沿 M4-02 参数化接受先例——只做
    // 有界校验与记录，不校验 TransferId 规范形式（生成入口随 M4-04 定案，
    // DEC-010 风险），不消费图片本体（无真实 I/O）。
    bool send_image_message(const aki::device::DeviceId& to,
        const aki::conversation::MessageId& message_id,
        const aki::transfer::FileMetadata& file,
        const aki::transfer::TransferId& transfer_id) override {
        if (to.empty() || message_id.empty() || file.name.empty()
            || transfer_id.empty()) {
            return false;  // 有界校验（EXEC-02 出站面）
        }
        sent_images_.push_back(
            SentImage{to, message_id, file, transfer_id});
        return true;
    }

    bool start_file_transfer(const aki::device::DeviceId& to,
        const aki::transfer::TransferId& transfer_id,
        const aki::transfer::FileMetadata& file,
        const std::filesystem::path& source_path) override {
        if (to.empty() || transfer_id.empty() || file.name.empty()) {
            return false;
        }
        // TransferId 语义（设计第 8.1 节）：一个会话不可重复启动。
        if (!transfer_sessions_.insert(transfer_id.value).second) {
            return false;
        }
        transfer_commands_.push_back(TransferCommand{TransferCommand::Kind::Start,
            to, transfer_id, source_path});
        return true;
    }

    bool pause_transfer(const aki::transfer::TransferId& transfer_id) override {
        return control_transfer(transfer_id, TransferCommand::Kind::Pause);
    }

    bool resume_transfer(const aki::transfer::TransferId& transfer_id) override {
        return control_transfer(transfer_id, TransferCommand::Kind::Resume);
    }

    bool cancel_transfer(const aki::transfer::TransferId& transfer_id) override {
        return control_transfer(transfer_id, TransferCommand::Kind::Cancel);
    }

    // ---- 编程式注入（Adapter → 应用，EXEC-02：有界校验 + 投递）----
    // 未设置 sink 或载荷校验失败返回 false；投递结果透传 sink 的返回值。

    bool inject_device_discovered(aki::device::DiscoveredDevice device) {
        if (sink_ == nullptr || device.identity.id.empty()) {
            return false;
        }
        return sink_->on_device_discovered(std::move(device));
    }

    bool inject_device_connected(aki::device::DeviceId device,
        aki::device::ConnectionPath path) {
        if (sink_ == nullptr || device.empty()) {
            return false;
        }
        return sink_->on_device_connected(std::move(device), path);
    }

    bool inject_device_disconnected(aki::device::DeviceId device) {
        if (sink_ == nullptr || device.empty()) {
            return false;
        }
        return sink_->on_device_disconnected(std::move(device));
    }

    bool inject_message_received(aki::conversation::Message message) {
        if (sink_ == nullptr || message.id.empty()) {
            return false;
        }
        return sink_->on_message_received(std::move(message));
    }

    bool inject_message_delivered(aki::conversation::ConversationId conversation,
        aki::conversation::MessageId message) {
        if (sink_ == nullptr || conversation.empty() || message.empty()) {
            return false;
        }
        return sink_->on_message_delivered(std::move(conversation), std::move(message));
    }

    bool inject_transfer_started(aki::transfer::Transfer transfer) {
        if (sink_ == nullptr || transfer.id.empty()) {
            return false;
        }
        return sink_->on_transfer_started(std::move(transfer));
    }

    bool inject_transfer_progress(aki::transfer::TransferId transfer,
        std::uint64_t transferred, std::uint64_t total) {
        if (sink_ == nullptr || transfer.empty()) {
            return false;
        }
        return sink_->on_transfer_progress(std::move(transfer), transferred, total);
    }

    bool inject_transfer_completed(aki::transfer::TransferId transfer,
        aki::transfer::TransferState final_state) {
        // final_state 仅取终态（设计第 10.1 节）；非法载荷在有界校验处拒绝。
        if (sink_ == nullptr || transfer.empty() || !aki::transfer::is_terminal(final_state)) {
            return false;
        }
        return sink_->on_transfer_completed(std::move(transfer), final_state);
    }

    bool inject_connection_path_changed(aki::device::DeviceId device,
        aki::device::ConnectionPath from, aki::device::ConnectionPath to) {
        if (sink_ == nullptr || device.empty()) {
            return false;
        }
        return sink_->on_connection_path_changed(std::move(device), from, to);
    }

    // ---- 观测（测试 / 冒烟宿主断言用）----

    [[nodiscard]] bool discovery_running() const noexcept { return discovery_running_; }
    [[nodiscard]] const std::vector<aki::device::DiscoveryMethod>& discovery_started()
        const noexcept {
        return discovery_methods_;
    }
    [[nodiscard]] const std::vector<SentText>& sent_texts() const noexcept {
        return sent_texts_;
    }
    [[nodiscard]] const std::vector<SentImage>& sent_images() const noexcept {
        return sent_images_;
    }
    [[nodiscard]] const std::vector<TransferCommand>& transfer_commands() const noexcept {
        return transfer_commands_;
    }
    [[nodiscard]] bool transfer_session_known(const std::string& transfer_id) const {
        return transfer_sessions_.count(transfer_id) != 0;
    }

private:
    bool control_transfer(const aki::transfer::TransferId& transfer_id,
        TransferCommand::Kind kind) {
        if (transfer_id.empty() || !transfer_session_known(transfer_id.value)) {
            return false;  // 未启动的会话不可控（TransferId 语义）。
        }
        transfer_commands_.push_back(
            TransferCommand{kind, aki::device::DeviceId{}, transfer_id, {}});
        return true;
    }

    HeyakiAdapterSink* sink_ = nullptr;
    // 跨线程状态标志：写入发生在 Adapter 出站调用方上下文（可为 executor
    // worker 线程，如 Manager drain），读取发生在宿主/测试等待轮询上下文
    // （TSAN 实测并发读写裸 bool 报 data race）——以 atomic 保证同步。
    std::atomic<bool> discovery_running_{false};
    std::vector<aki::device::DiscoveryMethod> discovery_methods_;
    std::vector<SentText> sent_texts_;
    std::vector<SentImage> sent_images_;
    std::vector<TransferCommand> transfer_commands_;
    std::set<std::string> transfer_sessions_;
};

}  // namespace aki::heyaki
