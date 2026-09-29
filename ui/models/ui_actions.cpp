// UiActions 组合根绑定实现（语义见 ui_actions.hpp）。
#include "ui/models/ui_actions.hpp"

#include "app/application/image_flow.hpp"
#include "heyaki/adapter/wire_ids.hpp"

#include <filesystem>
#include <utility>

namespace aki::ui::models {

UiActions make_ui_actions(aki::app::DeviceManager& devices,
    aki::app::ConversationManager& conversations,
    aki::app::MessageManager& messages, aki::app::TransferManager& transfers) {
    UiActions actions;
    // wire 标识生成（M5-05）：heyaki/adapter/wire_ids 规范 id 入口（§6.1
    // 生成入口收敛；纯函数，页面点击回调上下文可调）。
    actions.new_message_id = [] { return aki::heyaki::new_message_id(); };
    actions.new_transfer_id = [] { return aki::heyaki::new_transfer_id(); };
    actions.send_text =
        [&messages](aki::device::DeviceId to,
            aki::conversation::MessageId message_id, std::string text) {
            return messages.send_text(
                std::move(to), std::move(message_id), std::move(text));
        };
    // 图片发送 hash-first 编排（M5-05，DEC-010/DEC-011）：先传输准入，
    // 消息经 hash 延续异步发出（stored_sha256 随 FileMetadata 携带）。
    actions.send_image =
        [&transfers, &messages](aki::device::DeviceId to,
            aki::conversation::MessageId message_id,
            aki::transfer::FileMetadata media,
            aki::transfer::TransferId transfer_id,
            std::filesystem::path source_path) {
            return aki::app::send_image_message_with_hash(transfers, messages,
                to, message_id, media, transfer_id, std::move(source_path))
                == aki::app::ImageSendFlowResult::Submitted;
        };
    actions.send_file =
        [&transfers, &messages](aki::device::DeviceId to,
            aki::conversation::MessageId message_id,
            aki::transfer::FileMetadata media,
            aki::transfer::TransferId transfer_id,
            std::filesystem::path source_path) {
            return aki::app::send_file_message_with_hash(transfers, messages,
                to, message_id, media, transfer_id, source_path)
                == aki::app::ImageSendFlowResult::Submitted;
        };
    actions.start_transfer =
        [&transfers](aki::device::DeviceId to,
            aki::transfer::TransferId transfer_id,
            aki::transfer::FileMetadata file, std::filesystem::path source) {
            return transfers.start_transfer(std::move(to),
                std::move(transfer_id), std::move(file), std::move(source));
        };
    actions.pause_transfer = [&transfers](aki::transfer::TransferId id) {
        return transfers.pause_transfer(std::move(id));
    };
    actions.resume_transfer = [&transfers](aki::transfer::TransferId id) {
        return transfers.resume_transfer(std::move(id));
    };
    actions.cancel_transfer = [&transfers](aki::transfer::TransferId id) {
        return transfers.cancel_transfer(std::move(id));
    };
    actions.start_discovery =
        [&devices](aki::device::DiscoveryMethod method) {
            return devices.start_discovery(method);
        };
    actions.stop_discovery = [&devices] { return devices.stop_discovery(); };
    // 信任三操作（M5-04，DEC-006 映射 3）。
    actions.begin_pairing = [&devices](aki::device::DeviceId device) {
        return devices.begin_pairing(std::move(device));
    };
    actions.confirm_pairing = [&devices](aki::device::DeviceId device,
        std::string password) {
        return devices.confirm_pairing(std::move(device), std::move(password));
    };
    actions.reject_device = [&devices](aki::device::DeviceId device) {
        return devices.reject_device(std::move(device));
    };
    actions.revoke_device = [&devices](aki::device::DeviceId device) {
        return devices.revoke_device(std::move(device));
    };
    actions.set_device_remark = [&devices](aki::device::DeviceId device,
        std::string remark) {
        return devices.set_remark(std::move(device), std::move(remark));
    };
    actions.ensure_conversation =
        [&conversations](aki::device::DeviceId local, aki::device::DeviceId remote) {
            return conversations.ensure_conversation(
                std::move(local), std::move(remote));
        };
    return actions;
}

}  // namespace aki::ui::models
