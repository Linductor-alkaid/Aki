// UiActions 组合根绑定实现（语义见 ui_actions.hpp）。
#include "ui/models/ui_actions.hpp"

#include <filesystem>
#include <utility>

namespace aki::ui::models {

UiActions make_ui_actions(aki::app::DeviceManager& devices,
    aki::app::ConversationManager& conversations,
    aki::app::MessageManager& messages, aki::app::TransferManager& transfers) {
    UiActions actions;
    actions.send_text =
        [&messages](aki::device::DeviceId to,
            aki::conversation::MessageId message_id, std::string text) {
            return messages.send_text(
                std::move(to), std::move(message_id), std::move(text));
        };
    actions.send_image =
        [&messages](aki::device::DeviceId to,
            aki::conversation::MessageId message_id,
            aki::transfer::FileMetadata media,
            aki::transfer::TransferId transfer_id, bool transfer_admitted) {
            return messages.send_image(std::move(to), std::move(message_id),
                std::move(media), std::move(transfer_id), transfer_admitted);
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
    actions.confirm_pairing = [&devices](aki::device::DeviceId device) {
        return devices.confirm_pairing(std::move(device));
    };
    actions.reject_device = [&devices](aki::device::DeviceId device) {
        return devices.reject_device(std::move(device));
    };
    actions.revoke_device = [&devices](aki::device::DeviceId device) {
        return devices.revoke_device(std::move(device));
    };
    actions.ensure_conversation =
        [&conversations](aki::device::DeviceId local, aki::device::DeviceId remote) {
            return conversations.ensure_conversation(
                std::move(local), std::move(remote));
        };
    return actions;
}

}  // namespace aki::ui::models
