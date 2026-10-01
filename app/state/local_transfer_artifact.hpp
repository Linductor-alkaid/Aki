#pragma once
#include "transfer/transfer/transfer_types.hpp"
#include <algorithm>
#include <string>
#include <string_view>

namespace aki::app {
// Local application facts only; never serialized into a peer message.
struct LocalTransferArtifact {
    aki::transfer::TransferId transfer;
    std::string relative_path;
    std::string sha256;
    std::uint64_t size_bytes = 0;
    bool available = true;
    std::string error{};  // bounded archive failure; never a peer payload
};

[[nodiscard]] inline bool valid_local_transfer_artifact(
    const LocalTransferArtifact& file) noexcept {
    const auto& id = file.transfer.value;
    const auto safe_id = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '-';
    };
    if (id.empty() || id.size() > 64 || !std::all_of(id.begin(), id.end(), safe_id)) return false;
    const std::string_view path = file.relative_path;
    if (!path.starts_with("files/") || path.size() <= 7 + id.size()
        || path.substr(6, id.size()) != id || path[6 + id.size()] != '/') return false;
    const auto name = path.substr(7 + id.size());
    if (name.empty() || name.size() > 128 || std::all_of(name.begin(), name.end(), [](char c) { return c == '.'; })
        || !std::all_of(name.begin(), name.end(), [&](unsigned char c) {
            return safe_id(c) || c == '.';
        })) return false;
    return file.sha256.size() == 64
        && std::all_of(file.sha256.begin(), file.sha256.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}
}  // namespace aki::app
