#include "ui/platform/local_files.hpp"
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <cstdint>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#elif defined(__linux__)
#include <gio/gio.h>
#endif
#include <GLFW/glfw3.h>

namespace aki::ui::platform {
void copy_local_path(const std::string& path) {
    glfwSetClipboardString(nullptr, path.c_str());
}

bool open_file_folder(const std::filesystem::path& file, std::string& error) {
    error.clear();
    std::error_code ec;
    const auto folder = std::filesystem::absolute(file.parent_path(), ec);
    if (file.empty() || ec || !std::filesystem::is_directory(folder, ec)) {
        error = "Local folder is unavailable.";
        return false;
    }
#if defined(_WIN32)
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const auto result = reinterpret_cast<std::intptr_t>(ShellExecuteW(
        nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (SUCCEEDED(apartment)) CoUninitialize();
    if (result > 32) return true;
    error = "Cannot open local folder (Windows error " + std::to_string(result) + ").";
#elif defined(__linux__)
    GError* failure = nullptr;
    gchar* uri = g_filename_to_uri(folder.c_str(), nullptr, &failure);
    const bool opened = uri && g_app_info_launch_default_for_uri(uri, nullptr, &failure);
    if (uri) g_free(uri);
    if (opened) return true;
    error = failure ? failure->message : "Cannot open local folder.";
    if (failure) g_error_free(failure);
#else
    error = "Opening a folder is not supported on this platform.";
#endif
    return false;
}
}  // namespace aki::ui::platform
