// 快照消费面（设计 §9.1 快照消费与唤醒条款，M5-03；EXEC-03）。
//
// compose 上下文的有界消费：以序列号水位去重读取状态 owner 的 DoubleBuffer
// 快照与连接路径 LatestMailbox，有新快照时重派生四域视图模型（纯函数，
// view_models.hpp）。无等待、无轮询、无 IO——单次调用至多一次水位读取 +
// 一次派生；槽位忙（try_load 失败）留待下一帧（DoubleBuffer 覆盖式语义下
// 丢帧不丢状态，§10.1）。
//
// 水位与最近一份派生结果存于页面模型（UiStateSnapshot/UiConsumerWatermark
// 值语义，调用方持有——「页面持有 UI 态」纪律），本单元自身无状态、可被
// 任意宿主（GUI compose / console 驱动 / 单测）复用。EUI-NEO 无关（RULE-10）。
#pragma once

#include "app/state/app_state_owner.hpp"
#include "ui/models/view_models.hpp"

#include <cstdint>

namespace aki::ui::models {

// 消费水位（页面模型持有；0 = 从未消费——首次 consume 必取当前快照）。
struct UiConsumerWatermark {
    std::uint64_t snapshot_sequence = 0;
    std::uint64_t connection_path_sequence = 0;
};

// 最近一份消费与派生结果（页面模型持有；compose 只读，本函数写回）。
struct UiStateSnapshot {
    // 本地设备 id（组合根在首次 consume 前从装配报告填入——传输方向与会话
    // 端点归属判定依赖它；空 id = 未配置，方向派生按 inbound 兜底并可见）。
    aki::device::DeviceId local_device;
    bool has_snapshot = false;
    aki::app::AppState state;
    std::vector<DeviceView> devices;
    std::vector<ConversationView> conversations;
    std::vector<TransferView> transfers;
    aki::device::ConnectionPath latest_connection_path =
        aki::device::ConnectionPath::Unknown;
};

// 有界消费推进（compose 上下文）：返回 true 表示消费到新快照并已重派生
// （in_out 更新）；false 表示水位无变化或槽位忙（快照读取失败时连接路径
// 水位也保持不变——两通道以快照水位为提交点，避免半更新视图）。
[[nodiscard]] bool consume_ui_state(aki::app::AppStateOwner& owner,
    UiConsumerWatermark& watermark, UiStateSnapshot& in_out);

}  // namespace aki::ui::models
