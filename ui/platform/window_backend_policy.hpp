#pragma once

namespace aki::ui::platform {
enum class LinuxWindowBackend { Automatic, X11 };

// DEC-025: the pinned Wayland backend has no text-input protocol. Use XIM
// when a display is available; do not force an unavailable X11 server.
[[nodiscard]] constexpr LinuxWindowBackend linux_window_backend(
    bool has_x11_display) noexcept {
    return has_x11_display ? LinuxWindowBackend::X11
                           : LinuxWindowBackend::Automatic;
}
}  // namespace aki::ui::platform
