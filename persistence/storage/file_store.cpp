// 文件本体存储布局与生命周期实现（M2-06）。
#include "persistence/storage/file_store.hpp"

#include "persistence/repository/repositories.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace aki::persistence {
namespace {

using aki::persistence::TransferRepository;
using aki::transfer::Transfer;
using aki::transfer::TransferState;

constexpr std::size_t kMaxTransferIdLength = 64;
constexpr std::size_t kMaxDiskNameLength = 128;
constexpr std::size_t kStreamChunkSize = 64 * 1024;  // 有界流式缓冲（RULE-09）

[[noreturn]] void throw_invalid_transfer_id(const std::string& transfer_id) {
    throw std::invalid_argument(
        "transfer id must match [A-Za-z0-9_-]{1,64}: '" + transfer_id + "'");
}

bool is_terminal_state(TransferState state) noexcept {
    return state == TransferState::Completed || state == TransferState::Failed
        || state == TransferState::Cancelled;
}

// 接收根供源路径推导（M4-05，DEC-012①）：receive_dir + logical_name 段拼接。
// 段有界校验（纵深防御——heyaki 侧 safe_logical_file_name 已拦）：拒绝空段/
// 绝对路径 / `..` / 反斜杠；段数与总长有界（对齐 heyaki 逻辑名限制）。
// 校验失败返回空串（作业按「供源缺失」明确失败，RULE-09 不静默）。
[[nodiscard]] std::string receive_source_path(const std::string& receive_dir,
    const std::string& logical_name) {
    if (receive_dir.empty() || logical_name.empty()
        || logical_name.size() > 512) {
        return {};
    }
    std::filesystem::path resolved{receive_dir};
    std::size_t segments = 0;
    std::size_t start = 0;
    while (start <= logical_name.size()) {
        const auto end = logical_name.find('/', start);
        const std::string segment =
            logical_name.substr(start, end == std::string::npos
                    ? std::string::npos
                    : end - start);
        if (segment.empty() || segment == "." || segment == ".."
            || segment.find('\\') != std::string::npos) {
            return {};
        }
        resolved /= segment;
        if (++segments > 32) {
            return {};
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return resolved.string();
}

}  // namespace

FileStore::FileStore(std::string data_root)
    : root_(std::move(data_root)),
      files_dir_(root_ + "/files"),
      tmp_dir_(files_dir_ + "/tmp") {
    std::error_code ec;
    std::filesystem::create_directories(tmp_dir_, ec);
    if (ec) {
        throw std::runtime_error("FileStore: cannot create '" + tmp_dir_
            + "': " + ec.message());
    }
}

bool FileStore::valid_transfer_id(const std::string& transfer_id) noexcept {
    if (transfer_id.empty() || transfer_id.size() > kMaxTransferIdLength) {
        return false;
    }
    for (const char c : transfer_id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string FileStore::sanitize_disk_name(const std::string& remote_name) {
    // 取末段路径分量（'/' 与 '\\' 之后）——路径穿越载荷失去分隔符。
    std::size_t start = 0;
    for (std::size_t i = 0; i < remote_name.size(); ++i) {
        if (remote_name[i] == '/' || remote_name[i] == '\\') {
            start = i + 1;
        }
    }
    std::string name;
    for (std::size_t i = start; i < remote_name.size() && name.size()
        < kMaxDiskNameLength; ++i) {
        const char c = remote_name[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        name += ok ? c : '_';
    }
    // 空或全点（"." / ".." / "..."）→ 回退固定名，防目录语义。
    bool all_dots = !name.empty();
    for (const char c : name) {
        if (c != '.') {
            all_dots = false;
            break;
        }
    }
    if (name.empty() || all_dots) {
        name = "file";
    }
    return name;
}

std::string FileStore::part_path(const std::string& transfer_id) const {
    if (!valid_transfer_id(transfer_id)) {
        throw_invalid_transfer_id(transfer_id);
    }
    return tmp_dir_ + "/" + transfer_id + ".part";
}

std::string FileStore::final_path(const std::string& transfer_id,
    const std::string& disk_name) const {
    if (!valid_transfer_id(transfer_id)) {
        throw_invalid_transfer_id(transfer_id);
    }
    return files_dir_ + "/" + transfer_id + "/" + disk_name;
}

void FileStore::write_part(const std::string& transfer_id,
    std::span<const std::byte> data, bool append) const {
    const std::string path = part_path(transfer_id);  // 入口校验
    std::ofstream out(path,
        std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!out.is_open()) {
        throw std::runtime_error(
            "FileStore: cannot open '" + path + "' for writing");
    }
    out.write(reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out.good()) {
        throw std::runtime_error("FileStore: write failed for '" + path + "'");
    }
}

bool FileStore::part_exists(const std::string& transfer_id) const {
    std::error_code ec;
    return std::filesystem::exists(part_path(transfer_id), ec);
}

void FileStore::complete_transfer(TransferRepository& transfers,
    const std::string& transfer_id, const std::string& receive_dir) const {
    const auto row = transfers.find(aki::transfer::TransferId{transfer_id});
    if (!row.has_value()) {
        throw std::runtime_error(
            "FileStore: no transfer row for '" + transfer_id + "'");
    }

    const std::string disk_name = sanitize_disk_name(row->file.name);
    const std::string relative = "files/" + transfer_id + "/" + disk_name;
    const std::string final_abs = files_dir_ + "/" + transfer_id + "/"
        + disk_name;
    const std::string part_abs = part_path(transfer_id);

    // 幂等：行已 Completed 且目标文件存在 → 整组跳过（重复终态宣告计成功）。
    if (row->state == TransferState::Completed
        && std::filesystem::exists(final_abs)) {
        return;
    }

    // 供源解析（M4-05 参数化，DEC-012①）：.part → 接收根推导路径 →
    // final 恢复分支 → 明确失败。
    std::string receive_abs;
    if (!receive_dir.empty()) {
        receive_abs = receive_source_path(receive_dir, row->file.name);
    }
    const bool part_exists_now = std::filesystem::exists(part_abs);
    const bool receive_exists_now =
        !receive_abs.empty() && !part_exists_now
        && std::filesystem::exists(receive_abs);
    if (!part_exists_now && !receive_exists_now
        && !std::filesystem::exists(final_abs)) {
        // 全部供源缺失且无法从最终文件恢复：明确失败（RULE-09，不伪造成功）。
        throw std::runtime_error("FileStore: no source for transfer '"
            + transfer_id + "' (cannot complete): part=" + part_abs
            + (receive_abs.empty() ? std::string{}
                                   : " receive=" + receive_abs));
    }

    std::error_code ec;
    std::filesystem::create_directories(files_dir_ + "/" + transfer_id, ec);
    if (ec) {
        throw std::runtime_error("FileStore: cannot create '"
            + files_dir_ + "/" + transfer_id + "': " + ec.message());
    }

    std::string sha_hex;
    std::uint64_t size_bytes = 0;
    if (part_exists_now || receive_exists_now) {
        // 流式 SHA-256 + 复制到临时名，随后原子改名（同卷）。
        const std::string source_abs =
            part_exists_now ? part_abs : receive_abs;
        const std::string tmp_final = final_abs + ".tmp";
        std::ifstream in(source_abs, std::ios::binary);
        std::ofstream out(tmp_final, std::ios::binary | std::ios::trunc);
        if (!in.is_open() || !out.is_open()) {
            throw std::runtime_error("FileStore: cannot open streams for '"
                + source_abs + "' -> '" + tmp_final + "'");
        }
        Sha256 hash;
        std::vector<char> buffer(kStreamChunkSize);
        while (in.read(buffer.data(),
                   static_cast<std::streamsize>(buffer.size()))
            || in.gcount() > 0) {
            const std::streamsize got = in.gcount();
            hash.update(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(buffer.data()),
                static_cast<std::size_t>(got)));
            out.write(buffer.data(), got);
            size_bytes += static_cast<std::uint64_t>(got);
            if (!out.good()) {
                throw std::runtime_error("FileStore: write failed for '"
                    + tmp_final + "'");
            }
            if (!in.good()) {
                break;
            }
        }
        in.close();
        out.close();
        sha_hex = hash.final_hex();
        // 幂等恢复：目标已存在（上次崩溃残留）先移除，再原子改名。
        std::filesystem::remove(final_abs, ec);
        std::filesystem::rename(tmp_final, final_abs, ec);
        if (ec) {
            throw std::runtime_error("FileStore: rename failed for '"
                + tmp_final + "' -> '" + final_abs + "': " + ec.message());
        }
        // 供源清理折进同一作业（DEC-012①：保持 CompleteTransfer=1 作业）：
        // .part 清理 / 接收根原件删除（跨卷拷贝语义的删除半边）。
        std::filesystem::remove(source_abs, ec);
    } else {
        // 崩溃恢复：改名已发生、回写未完成 → 从最终文件补算哈希回写。
        std::ifstream in(final_abs, std::ios::binary);
        if (!in.is_open()) {
            throw std::runtime_error("FileStore: cannot open '" + final_abs
                + "' for recovery hashing");
        }
        Sha256 hash;
        std::vector<char> buffer(kStreamChunkSize);
        while (in.read(buffer.data(),
                   static_cast<std::streamsize>(buffer.size()))
            || in.gcount() > 0) {
            const std::streamsize got = in.gcount();
            hash.update(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(buffer.data()),
                static_cast<std::size_t>(got)));
            size_bytes += static_cast<std::uint64_t>(got);
            if (!in.good()) {
                break;
            }
        }
        sha_hex = hash.final_hex();
    }

    // TRANSFER 终态更新 + 回写位（M2-04 complete：state 与回写列同语句原子）。
    transfers.complete(aki::transfer::TransferId{transfer_id},
        TransferState::Completed,
        CompletedFile{relative, sha_hex, size_bytes});
}

void FileStore::discard_part(const std::string& transfer_id) const noexcept {
    if (!valid_transfer_id(transfer_id)) {
        return;  // 非法 id：无对应文件，幂等 no-op。
    }
    std::error_code ec;
    std::filesystem::remove(tmp_dir_ + "/" + transfer_id + ".part", ec);
    // 幂等删除：缺失或失败均不抛（清理语义）；失败残留会被启动清扫兜底。
}

std::size_t FileStore::sweep_tmp_orphans(
    const std::vector<Transfer>& active_transfers) const {
    // 活动判定：加载行中存在同 id 且非终态 → 保留；否则（终态/无行）删除。
    std::error_code ec;
    if (!std::filesystem::exists(tmp_dir_, ec)) {
        return 0;
    }
    std::size_t removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(tmp_dir_, ec)) {
        const std::string filename = entry.path().filename().string();
        if (!filename.ends_with(".part")) {
            continue;
        }
        const std::string stem = filename.substr(0, filename.size() - 5);
        bool active = false;
        if (valid_transfer_id(stem)) {
            for (const Transfer& transfer : active_transfers) {
                if (transfer.id.value == stem) {
                    active = !is_terminal_state(transfer.state);
                    break;
                }
            }
        }
        if (active) {
            continue;
        }
        if (std::filesystem::remove(entry.path(), ec)) {
            ++removed;
        }
    }
    return removed;
}

}  // namespace aki::persistence
