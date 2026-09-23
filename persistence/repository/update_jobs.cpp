// typed 更新 → DB 作业工厂实现（设计第 11.1 节 ①；M2-07）。
#include "persistence/repository/update_jobs.hpp"

#include <memory>
#include <utility>

namespace aki::persistence {

DbJob make_device_upsert_job(aki::device::DeviceIdentity device) {
    auto done = std::make_shared<std::promise<void>>();
    return DbJob{
        [device = std::move(device)](Repositories& repos) {
            repos.devices.upsert(device);
        },
        std::move(done)};
}

DbJob make_conversation_upsert_job(
    aki::conversation::Conversation conversation) {
    auto done = std::make_shared<std::promise<void>>();
    return DbJob{
        [conversation = std::move(conversation)](Repositories& repos) {
            repos.conversations.upsert(conversation);
        },
        std::move(done)};
}

DbJob make_message_upsert_job(aki::conversation::Message message,
    aki::conversation::ConversationId conversation_id) {
    auto done = std::make_shared<std::promise<void>>();
    return DbJob{
        [message = std::move(message),
            conversation_id = std::move(conversation_id)](Repositories& repos) {
            repos.messages.upsert(message, conversation_id);
        },
        std::move(done)};
}

DbJob make_message_delivery_job(aki::conversation::MessageId message,
    aki::conversation::DeliveryState state) {
    auto done = std::make_shared<std::promise<void>>();
    return DbJob{
        [message = std::move(message), state](Repositories& repos) {
            repos.messages.set_delivery_state(message, state);
        },
        std::move(done)};
}

DbJob make_transfer_upsert_job(aki::transfer::Transfer transfer,
    aki::conversation::MessageId message_id) {
    auto done = std::make_shared<std::promise<void>>();
    return DbJob{
        [transfer = std::move(transfer),
            message_id = std::move(message_id)](Repositories& repos) {
            repos.transfers.upsert(transfer, message_id);
        },
        std::move(done)};
}

DbJob make_transfer_progress_job(aki::transfer::TransferId transfer,
    std::uint64_t transferred, std::uint64_t total) {
    auto done = std::make_shared<std::promise<void>>();
    return DbJob{
        [transfer = std::move(transfer), transferred, total](
            Repositories& repos) {
            repos.transfers.update_progress(transfer, transferred, total);
        },
        std::move(done)};
}

DbJob make_transfer_terminal_job(aki::transfer::TransferId transfer,
    aki::transfer::TransferState final_state) {
    auto done = std::make_shared<std::promise<void>>();
    return DbJob{
        [transfer = std::move(transfer), final_state](Repositories& repos) {
            repos.transfers.complete(transfer, final_state);
        },
        std::move(done)};
}

}  // namespace aki::persistence
