// 文件终态作业工厂实现（M2-06）。
#include "persistence/storage/file_jobs.hpp"

#include <utility>

namespace aki::persistence {

DbJob make_transfer_complete_job(std::shared_ptr<FileStore> store,
    std::string transfer_id, std::string receive_dir) {
    auto done = std::make_shared<std::promise<void>>();
    return DbJob{
        [store = std::move(store), id = std::move(transfer_id),
            receive = std::move(receive_dir)](Repositories& repos) {
            store->complete_transfer(repos.transfers, id, receive);
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
