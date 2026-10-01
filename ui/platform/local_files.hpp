#pragma once
#include <filesystem>
#include <string>
namespace aki::ui::platform {
// Synchronous desktop handoff in the UI event context. The desktop owns the
// file manager; this does not create an Aki worker or detached child process.
[[nodiscard]] bool open_file_folder(const std::filesystem::path& file, std::string& error);
void copy_local_path(const std::string& path);
}
