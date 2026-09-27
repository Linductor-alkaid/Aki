// RouterSink（设计第 8.3 节，DEC-008；M1-05）。
//
// app/application 内唯一的 HeyakiAdapterSink 实现：回调线程只做有界校验并按
// 路由表把 9 类事件投递到各 Manager 的私有收件箱（EXEC-02：业务处理一律在
// Manager 的执行上下文，本类不在回调线程触碰 Application State）。
// 返回值为各路 admission 的合取——任一路被拒即对调用方可见（RULE-09）；
// 唯一的双 Manager 扇出是 connected/disconnected，主路径事件各只投递一次
// （由 DeviceManager 发布）。
//
// 线程契约：与 HeyakiAdapterSink 相同（回调线程）；各 Manager 收件箱线程安全，
// Sink 不持有事件状态，可重入。
#pragma once

#include "app/application/conversation_manager.hpp"
#include "app/application/device_manager.hpp"
#include "app/application/message_manager.hpp"
#include "app/application/transfer_manager.hpp"
#include "heyaki/adapter/heyaki_adapter.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace aki::app {

class RouterSink final : public aki::heyaki::HeyakiAdapterSink {
public:
    RouterSink(DeviceManager& devices, ConversationManager& conversations,
        MessageManager& messages, TransferManager& transfers)
        : devices_(devices),
          conversations_(conversations),
          messages_(messages),
          transfers_(transfers) {}

    bool on_device_discovered(aki::device::DiscoveredDevice device) override {
        return devices_.enqueue_discovered(std::move(device));
    }

    bool on_device_connected(aki::device::DeviceId device,
        aki::device::ConnectionPath path) override {
        const bool device_admitted = devices_.enqueue_connected(device, path);
        const bool conversation_admitted = conversations_.enqueue_peer_connected(device);
        return device_admitted && conversation_admitted;
    }

    bool on_device_disconnected(aki::device::DeviceId device) override {
        const bool device_admitted = devices_.enqueue_disconnected(device);
        const bool conversation_admitted = conversations_.enqueue_peer_disconnected(device);
        return device_admitted && conversation_admitted;
    }

    bool on_message_received(aki::conversation::Message message) override {
        return messages_.enqueue_message_received(std::move(message));
    }

    bool on_message_delivered(aki::conversation::ConversationId conversation,
        aki::conversation::MessageId message) override {
        return messages_.enqueue_message_delivered(std::move(conversation), std::move(message));
    }

    bool on_message_send_failed(aki::conversation::ConversationId conversation,
        aki::conversation::MessageId message) override {
        return messages_.enqueue_message_send_failed(
            std::move(conversation), std::move(message));
    }

    bool on_transfer_started(aki::transfer::Transfer transfer) override {
        return transfers_.enqueue_transfer_started(std::move(transfer));
    }

    bool on_transfer_progress(aki::transfer::TransferId transfer,
        std::uint64_t transferred, std::uint64_t total) override {
        return transfers_.enqueue_transfer_progress(
            std::move(transfer), transferred, total);
    }

    bool on_transfer_completed(aki::transfer::TransferId transfer,
        aki::transfer::TransferState final_state) override {
        return transfers_.enqueue_transfer_completed(std::move(transfer), final_state);
    }

    // 传输暂停面（M4-05，§8.1 第 11 方法/DEC-012④）：UpsertTransfer(Paused)
    // 经 TM 已知行缓存承载（不新增 AppEvent 主路径类型）。
    bool on_transfer_paused(aki::transfer::TransferId transfer) override {
        return transfers_.enqueue_transfer_paused(std::move(transfer));
    }

    bool on_connection_path_changed(aki::device::DeviceId device,
        aki::device::ConnectionPath from, aki::device::ConnectionPath to) override {
        return devices_.enqueue_connection_path_changed(
            std::move(device), from, to);
    }

    // 配对一次性结果（M5-04，sink 第 12 方法）：路由到 DM 信任转移
    //（detail 供诊断日志面，不进 Store）。
    bool on_pairing_completed(aki::device::DeviceId device, bool success,
        std::string_view detail) override {
        return devices_.enqueue_pairing_completed(
            std::move(device), success, std::string(detail));
    }

private:
    DeviceManager& devices_;
    ConversationManager& conversations_;
    MessageManager& messages_;
    TransferManager& transfers_;
};

}  // namespace aki::app
