// 文件终态作业工厂实现（M2-06）。
#include "persistence/storage/file_jobs.hpp"

#include <utility>
#include <stdexcept>

namespace aki::persistence {

DbJob make_transfer_complete_job(std::shared_ptr<FileStore> store,
    std::string transfer_id, std::string receive_dir,
    std::function<void(const CompletedFile&)> on_stored,
    std::function<void(std::string)> on_failure) {
    auto done = std::make_shared<std::promise<void>>();
    return DbJob{
        [store = std::move(store), id = std::move(transfer_id),
            receive = std::move(receive_dir), callback = std::move(on_stored), failure = std::move(on_failure)](Repositories& repos) {
            try {
                store->complete_transfer(repos.transfers, id, receive);
                if (callback) {
                    auto file = repos.transfers.stored_file(aki::transfer::TransferId{id});
                    if (!file) throw std::runtime_error("completed archive record missing");
                    callback(*file);
                }
            } catch (const std::exception& error) {
                if (failure) failure(std::string(error.what()).substr(0, 512));
                throw;
            } catch (...) {
                if (failure) failure("Unknown local archive failure");
                throw;
            }
        },
        std::move(done)};
}

DbJob make_transfer_discard_job(std::shared_ptr<FileStore> store,
    std::string transfer_id) {
    auto done = std::make_shared<std::promise<void>>();
    return DbJob{
        [store = std::move(store), id = std::move(transfer_id)](
            Repositories&) { store->discard_part(id); }, std::move(done)};
}

}  // namespace aki::persistence
