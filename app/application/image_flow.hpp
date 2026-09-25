// 图片消息发送编排（M4-03，设计 §6.1②；DEC-010②）。
//
// 发送侧准入期闸门的编排层承载——唯一跨 MessageManager/TransferManager 的
// 传导点（两 Manager 各自单飞泵，DEC-008 职责切分不变：MM 只写 messages、
// TM 只写 transfers，闸门以 enqueue admission 返回值为序在编排层组合）。
// 闸门判据是 **enqueue 级 admission**（收件箱受理与否）——两接口均为异步
// 命令面，业务级拒绝（重复 TransferId 等）发生在 TM 排空 handler 内、晚于
// 本编排返回（经 handler_rejections 可见，RULE-09），闸门不可见：
//   1. 先传输准入（TM.start_transfer enqueue admission）；失败（TM 收件箱
//      满）→ 不发送消息（Adapter 零 send 调用），消息行经 MM 记 Failed；
//   2. 后消息准入（MM.send_image enqueue admission）；失败（MM 收件箱满）→
//      对刚准入的传输发 cancel_transfer（传输行 Cancelled——本地确定性
//      决策，时序处于 Negotiating 前的准入窗口、无竞态）。
// 两笔补偿写入自身也是 enqueue admission、同样可能被收件箱拒（两级降级，
// AGENTS 规则 10：拒绝不吞掉、经返回值可见）：补偿 Failed 行被拒 → 无消息
// 行（TransferAdmissionFailedRowLost）；补偿取消被拒 → 传输持续在飞、
// 传输行停留 Queued（MessageAdmissionFailedCancelLost）。
// 运行期零传导（§6.1②）：peer ack 事件与传输终态事件互不跨通道回写，
// 本编排不订阅任何运行期事件。
//
// 本函数不创建任务、不触碰 Application State（只调用两 Manager 的公开
// enqueue 面）；线程契约同两 Manager（任意上下文可调，enqueue 线程安全）。
// source_path 为发送侧本地文件路径（§7.1⑤），真实消费随 M4-04。
#pragma once

#include "app/application/message_manager.hpp"
#include "app/application/transfer_manager.hpp"
#include "conversation/message/message_types.hpp"
#include "device/device/device_types.hpp"
#include "transfer/transfer/transfer_types.hpp"

#include <filesystem>
#include <utility>

namespace aki::app {

enum class ImageSendFlowResult {
    Submitted,        // 双侧准入成功（消息/传输行由各自 Manager 记录）
    TransferAdmissionFailed,  // 传输 enqueue 拒绝：消息行经 MM 记 Failed、零 send
    TransferAdmissionFailedRowLost,  // 传输 enqueue 拒绝 + 补偿 Failed 行写入亦被
                                     // MM 收件箱拒：无消息行（两级降级可见）
    MessageAdmissionFailed,  // 消息 enqueue 拒绝：补偿 cancel_transfer 入列受理
    MessageAdmissionFailedCancelLost,  // 消息 enqueue 拒绝 + 补偿取消亦被 TM 收件
                                      // 箱拒：传输持续在飞、传输行停留 Queued
};

// 图片消息 + 关联传输的发送侧编排（唯一入口：宿主/组合根调用）。
[[nodiscard]] inline ImageSendFlowResult send_image_message_with_transfer(
    TransferManager& transfers, MessageManager& messages,
    const aki::device::DeviceId& to,
    const aki::conversation::MessageId& message_id,
    const aki::transfer::FileMetadata& file,
    const aki::transfer::TransferId& transfer_id,
    const std::filesystem::path& source_path = {}) {
    if (!transfers.start_transfer(to, transfer_id, file, source_path)) {
        // 闸门第 1 步失败（TM enqueue 拒绝）：不发送消息；补偿写 Failed 行
        //（transfer_admitted = false → MM 跳过 Adapter 调用）。补偿写入自身
        // 也是 enqueue admission——被 MM 收件箱拒时消息行不落库，降级结果经
        // 返回值可见（AGENTS 规则 10），不 (void) 吞掉。
        return messages.send_image(to, message_id, file, transfer_id,
                   /*transfer_admitted=*/false)
            ? ImageSendFlowResult::TransferAdmissionFailed
            : ImageSendFlowResult::TransferAdmissionFailedRowLost;
    }
    if (!messages.send_image(to, message_id, file, transfer_id)) {
        // 闸门第 2 步失败（MM enqueue 拒绝）：补偿取消（幂等；受理则传输行
        // Cancelled）。补偿取消自身也是 enqueue admission——被 TM 收件箱拒时
        // 传输会话持续在飞、传输行停留 Queued，降级结果经返回值可见。
        return transfers.cancel_transfer(transfer_id)
            ? ImageSendFlowResult::MessageAdmissionFailed
            : ImageSendFlowResult::MessageAdmissionFailedCancelLost;
    }
    return ImageSendFlowResult::Submitted;
}

}  // namespace aki::app
