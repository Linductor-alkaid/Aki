// 快照消费面实现（语义见 ui_state_consumer.hpp）。
#include "ui/models/ui_state_consumer.hpp"

#include <utility>

namespace aki::ui::models {

bool consume_ui_state(aki::app::AppStateOwner& owner,
    UiConsumerWatermark& watermark, UiStateSnapshot& in_out) {
    executor::comm::Snapshot<aki::app::AppState> snapshot;
    // 水位去重：无新快照即返回（compose 只读推进，不阻塞）。
    if (!owner.load_snapshot_newer_than(watermark.snapshot_sequence, snapshot)) {
        // 连接路径摘要独立推进（不触发 Store 快照；DEC-009/§10.1）——
        // 有新路径值时只更新摘要，不动派生视图（路径不在视图内缓存）。
        aki::device::ConnectionPath path;
        std::uint64_t path_sequence = watermark.connection_path_sequence;
        if (owner.try_load_connection_path_newer_than(
                watermark.connection_path_sequence, path, path_sequence)) {
            watermark.connection_path_sequence = path_sequence;
            in_out.latest_connection_path = path;
            for (DeviceView& device : in_out.devices) {
                device.connection_path = path;
            }
        }
        return false;
    }

    // 新快照：提交点先落水位，再派生视图（保证下次消费从本快照之后起）。
    watermark.snapshot_sequence = snapshot.sequence;
    in_out.has_snapshot = true;
    in_out.state = std::move(snapshot.value);
    in_out.devices =
        derive_device_views(in_out.state.devices, in_out.latest_connection_path);
    in_out.conversations = derive_conversation_views(
        in_out.state.conversations, in_out.state.messages);
    in_out.transfers =
        derive_transfer_views(in_out.state.transfers, in_out.local_device);

    // 连接路径同帧补给（同一 compose 内的水位一致面）。
    aki::device::ConnectionPath path;
    std::uint64_t path_sequence = watermark.connection_path_sequence;
    if (owner.try_load_connection_path_newer_than(
            watermark.connection_path_sequence, path, path_sequence)) {
        watermark.connection_path_sequence = path_sequence;
        in_out.latest_connection_path = path;
        for (DeviceView& device : in_out.devices) {
            device.connection_path = path;
        }
    }
    return true;
}

}  // namespace aki::ui::models
