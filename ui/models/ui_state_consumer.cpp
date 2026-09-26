// 快照消费面实现（语义见 ui_state_consumer.hpp）。
#include "ui/models/ui_state_consumer.hpp"

#include <utility>

namespace aki::ui::models {

bool consume_ui_state(aki::app::AppStateOwner& owner,
    UiConsumerWatermark& watermark, UiStateSnapshot& in_out) {
    executor::comm::Snapshot<aki::app::AppState> snapshot;
    // 水位去重：无新快照即返回（compose 只读推进，不阻塞）；路径随快照
    // 派生（DEC-015：逐设备 Store 字段，无独立水位）。
    if (!owner.load_snapshot_newer_than(watermark.snapshot_sequence, snapshot)) {
        return false;
    }

    // 新快照：提交点先落水位，再派生视图（保证下次消费从本快照之后起）。
    watermark.snapshot_sequence = snapshot.sequence;
    in_out.has_snapshot = true;
    in_out.state = std::move(snapshot.value);
    in_out.devices = derive_device_views(in_out.state.devices);
    in_out.conversations = derive_conversation_views(
        in_out.state.conversations, in_out.state.messages);
    in_out.transfers =
        derive_transfer_views(in_out.state.transfers, in_out.local_device);
    return true;
}

}  // namespace aki::ui::models
