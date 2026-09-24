// Heyaki Adapter SPI（设计第 8.1/10 节，DEC-002）：纯虚抽象接口。
// 只使用设计第 3~7 节领域类型（RULE-01 / RULE-10）；真实 Heyaki 在 M3 目标级接入
// （DEC-003 / DEC-006）。所有 bool 返回值都是有界 admission / 投递结果，
// 拒绝必须可见（RULE-09 / EXEC-02）。
#pragma once

#include "conversation/conversation/conversation_types.hpp"
#include "conversation/message/message_types.hpp"
#include "device/device/device_types.hpp"
#include "device/discovery/discovery_types.hpp"
#include "transfer/transfer/transfer_types.hpp"

#include <cstdint>
#include <string_view>

namespace aki::heyaki {

// 入站事件入口（Adapter → 应用）。方法与设计第 10 节 9 类事件一一对应；
// 实现方（M1-05 Manager / 测试桥接）负责把事件转成 Application State 更新与
// 设计第 10.1 节的事件投递。回调须遵守 EXEC-02：只做有界校验与投递，
// 业务 handler 不在本接口的调用线程执行。
class HeyakiAdapterSink {
public:
    virtual ~HeyakiAdapterSink() = default;

    HeyakiAdapterSink(const HeyakiAdapterSink&) = delete;
    HeyakiAdapterSink& operator=(const HeyakiAdapterSink&) = delete;

    // 返回 false 表示投递被拒绝（载荷校验失败或下游背压），调用方必须可见。
    virtual bool on_device_discovered(aki::device::DiscoveredDevice device) = 0;
    virtual bool on_device_connected(aki::device::DeviceId device,
        aki::device::ConnectionPath path) = 0;
    virtual bool on_device_disconnected(aki::device::DeviceId device) = 0;
    virtual bool on_message_received(aki::conversation::Message message) = 0;
    virtual bool on_message_delivered(aki::conversation::ConversationId conversation,
        aki::conversation::MessageId message) = 0;
    // 出站文本的投递回报终态失败面（M3-05；DEC-006 映射 4 的 send_failed /
    // peer_rejected / ack_timeout / session_closed）；协议 acked 走
    // on_message_delivered。映射 SetDeliveryState(Failed)（终态，RULE-08）。
    virtual bool on_message_send_failed(
        aki::conversation::ConversationId conversation,
        aki::conversation::MessageId message) = 0;
    virtual bool on_transfer_started(aki::transfer::Transfer transfer) = 0;
    virtual bool on_transfer_progress(aki::transfer::TransferId transfer,
        std::uint64_t transferred, std::uint64_t total) = 0;
    // final_state 仅取 Completed / Failed / Cancelled（设计第 10.1 节）；
    // 迟到事件不得让已终结的传输回到活动状态（RULE-08，由应用层状态机拒绝）。
    virtual bool on_transfer_completed(aki::transfer::TransferId transfer,
        aki::transfer::TransferState final_state) = 0;
    virtual bool on_connection_path_changed(aki::device::DeviceId device,
        aki::device::ConnectionPath from, aki::device::ConnectionPath to) = 0;

protected:
    HeyakiAdapterSink() = default;
};

// 出站能力（应用 → Adapter）。M1 只固定签名与语义；真实实现在 M3 对齐 DEC-006。
class HeyakiAdapter {
public:
    virtual ~HeyakiAdapter() = default;

    HeyakiAdapter(const HeyakiAdapter&) = delete;
    HeyakiAdapter& operator=(const HeyakiAdapter&) = delete;

    // 设备发现启停（设计第 4 节）。method 标注来源；扫描型来源（LanDiscovery /
    // Relay）启动扫描，记录型来源（KnownDevice / InviteLink / Manual）的接入在
    // M3 细化。返回 false 表示 admission 被拒。
    virtual bool start_discovery(aki::device::DiscoveryMethod method) = 0;
    // 幂等停止；未启动时为 no-op。
    virtual void stop_discovery() = 0;

    // 文本消息发送（设计第 6 节）。message_id 由应用生成并保持稳定（RULE-08）；
    // 发送结果经 on_message_delivered / on_message_received 之外的投递回报异步返回。
    virtual bool send_text_message(const aki::device::DeviceId& to,
        const aki::conversation::MessageId& message_id, std::string_view text) = 0;

    // 文件传输接口面（设计第 7 节）。M4 前仅签名与 TransferId 语义：
    // 一个 TransferId 对应一个传输会话，不可重复启动；文件本体不经本接口
    // 传输（RULE-05）。传输进度与终态经 on_transfer_* 事件异步返回。
    virtual bool start_file_transfer(const aki::device::DeviceId& to,
        const aki::transfer::TransferId& transfer_id,
        const aki::transfer::FileMetadata& file) = 0;
    virtual bool pause_transfer(const aki::transfer::TransferId& transfer_id) = 0;
    virtual bool resume_transfer(const aki::transfer::TransferId& transfer_id) = 0;
    virtual bool cancel_transfer(const aki::transfer::TransferId& transfer_id) = 0;

protected:
    HeyakiAdapter() = default;
};

}  // namespace aki::heyaki
