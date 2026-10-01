#pragma once
#include "app/state/local_transfer_artifact.hpp"
#include <filesystem>
#include <string>

namespace aki::ui::models {
// No filesystem IO: resolve only a validated archive key under the injected root.
[[nodiscard]] inline std::filesystem::path local_file_path(
    const std::string& root, const std::string& relative) {
    if (root.empty() || !relative.starts_with("files/")) return {};
    const auto end = relative.find('/', 6);
    if (end == std::string::npos) return {};
    aki::app::LocalTransferArtifact file{{relative.substr(6, end - 6)}, relative,
        std::string(64, '0')};
    if (!aki::app::valid_local_transfer_artifact(file)) return {};
    return std::filesystem::path{root} / relative;
}
}  // namespace aki::ui::models
