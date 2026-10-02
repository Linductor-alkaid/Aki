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

DbJob make_device_name_job(aki::device::DeviceId device, std::string name) {
    auto done = std::make_shared<std::promise<void>>();
    return DbJob{[device = std::move(device), name = std::move(name)](
                     Repositories& repos) {
                     repos.devices.set_display_name(device, name);
                 }, std::move(done)};
}

DbJob make_device_remark_job(aki::device::DeviceId device,
    std::string remark) {
    auto done = std::make_shared<std::promise<void>>();
    return DbJob{[device = std::move(device), remark = std::move(remark)](
                     Repositories& repos) {
                     repos.devices.set_remark(device, remark);
                 }, std::move(done)};
}

DbJob make_device_inbound_trust_job(aki::device::DeviceId device,
    bool inbound_trust) {
    auto done = std::make_shared<std::promise<void>>();
    return DbJob{[device = std::move(device), inbound_trust](
                     Repositories& repos) {
                     repos.devices.set_inbound_trust(device, inbound_trust);
                 }, std::move(done)};
}

DbJob make_conversation_pin_job(aki::conversation::ConversationId id, bool pinned) {
    auto done = std::make_shared<std::promise<void>>();
    return DbJob{[id = std::move(id), pinned](Repositories& repos) {
                     repos.conversations.set_pinned(id, pinned);
                 },
                 std::move(done)};
}

DbJob make_conversation_hidden_job(aki::conversation::ConversationId id, bool hidden) {
    auto done = std::make_shared<std::promise<void>>();
    return DbJob{[id = std::move(id), hidden](Repositories& repos) {
                     repos.conversations.set_hidden(id, hidden);
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
    return DbJob{[message = std::move(message),
                  conversation_id = std::move(conversation_id)](Repositories& repos) {
                     Transaction transaction(repos.database);
                     const bool inserted = !repos.messages.find(message.id).has_value();
                     const auto conversation = repos.conversations.find(conversation_id);
                     repos.messages.upsert(message, conversation_id);
                     if (inserted && conversation && conversation->hidden &&
                         !aki::conversation::is_terminal(conversation->state) &&
                         message.sender == conversation->remote_device &&
                         message.receiver == conversation->local_device) {
                         repos.conversations.set_hidden(conversation_id, false);
                     }
                     transaction.commit();
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
