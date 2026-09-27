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
#include <filesystem>
#include <string_view>

namespace aki::heyaki {

// 入站事件入口（Adapter → 应用）。方法与设计第 10 节事件面及 M3-05 增补的
// 出站投递回报终态对应（on_message_send_failed 为主路径 9 类事件外的出站
// 失败面，设计第 8.1 节）；实现方（M1-05 Manager / 测试桥接）负责把事件
// 转成 Application State 更新与设计第 10.1 节的事件投递。回调须遵守
// EXEC-02：只做有界校验与投递，业务 handler 不在本接口的调用线程执行。
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
    // 传输暂停投递面（M4-05，设计 §8.1/§7.1⑤；DEC-006 映射 7）：对端驱动
    //（含断线自动暂停，可发生于接收侧）与本地暂停确认同此入口。映射
    // UpsertTransfer(Paused)（不新增 AppEvent 主路径类型，状态经 Store 快照
    // 可见）；发送会话侧同时抑制归档续接。
    virtual bool on_transfer_paused(aki::transfer::TransferId transfer) = 0;
    virtual bool on_connection_path_changed(aki::device::DeviceId device,
        aki::device::ConnectionPath from, aki::device::ConnectionPath to) = 0;
    // 配对一次性结果投递面（M5-04，设计 §8.1 第 12 方法；DEC-006 映射 3）：
    // Node 上下文回调 → Adapter 有界校验 + 投递（EXEC-02）。success 映射
    // UpsertDevice(→ Trusted)、失败映射 UpsertDevice(→ Rejected)（detail
    // 供诊断，不进 Store）；不新增 AppEvent 主路径类型。
    virtual bool on_pairing_completed(aki::device::DeviceId device,
        bool success, std::string_view detail) = 0;

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

    // 图片消息发送面（设计第 6.1/8.1 节，M4-03；DEC-010③）。bool admission
    // 语义同 send_text_message；消息面仅 FileMetadata + TransferId，图片本体经
    // 传输链路（RULE-05）——本接口与传输四接口以 TransferId 关联（发送侧准入
    // 闸门「先传输准入、后发消息」由编排层承载，设计 §6.1②）。载荷超限或
    // TransferId 非规范（编码失败）即 admission false 可见（RULE-09）。
    // 入站不新增 sink 方法：复用 on_message_received（信封 type 分发在
    // Adapter 层）。
    virtual bool send_image_message(const aki::device::DeviceId& to,
        const aki::conversation::MessageId& message_id,
        const aki::transfer::FileMetadata& file,
        const aki::transfer::TransferId& transfer_id) = 0;

    // 文件传输接口面（设计第 7/7.1⑤ 节）。M4-02 起签名含发送侧本地路径
    // `source_path`（std::filesystem::path）——不进入对端可见的 FileMetadata
    // （携带本地路径即信息外泄）；数据链路与路径真实消费在 M4-04 落地。
    // 一个 TransferId 对应一个传输会话，不可重复启动；文件本体不经本接口
    // 传输（RULE-05）。传输进度与终态经 on_transfer_* 事件异步返回。
    virtual bool start_file_transfer(const aki::device::DeviceId& to,
        const aki::transfer::TransferId& transfer_id,
        const aki::transfer::FileMetadata& file,
        const std::filesystem::path& source_path) = 0;
    virtual bool pause_transfer(const aki::transfer::TransferId& transfer_id) = 0;
    virtual bool resume_transfer(const aki::transfer::TransferId& transfer_id) = 0;
    virtual bool cancel_transfer(const aki::transfer::TransferId& transfer_id) = 0;

    // 信任操作面（M5-04，设计 §8.1；DEC-006 映射 3 落地面）。bool 返回值为
    // 有界 admission / 提交结果，拒绝可见（RULE-09）；配对一次性结果经
    // on_pairing_completed 异步返回。口令处理在 Adapter/heyaki 层内部
    // （DEC-016 冻结常量），不进 SPI 签名。
    // 指纹确认后发起配对（→ pair_peer，scope 冻结 {message.send,
    // file.push:inbox}）；提交被拒 = 会话缺失/非 pairing_restricted/重复
    // pending。
    virtual bool confirm_pairing(const aki::device::DeviceId& peer) = 0;
    // 撤销既有信任（→ revoke_trust_grants，撤销该 peer 全部有效 grant）；
    // 无有效 grant 时 false（无操作可见）。
    virtual bool revoke_trust(const aki::device::DeviceId& peer) = 0;

protected:
    HeyakiAdapter() = default;
};

}  // namespace aki::heyaki
