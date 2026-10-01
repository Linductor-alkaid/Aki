#pragma once
#include "ui/platform/window_backend_policy.hpp"

#if defined(__linux__)
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include "third_party/EUI-NEO/3rd/glfw/include/GLFW/glfw3.h"
#include <clocale>
#include <cstdlib>
#endif

namespace aki::ui::platform {

// DEC-025 / EUI-20261001-001. This only declares initialization policy;
// the framework remains the sole GLFW lifecycle owner.
inline void configure_window_backend_before_init() {
#if defined(__linux__)
    (void)std::setlocale(LC_CTYPE, "");
    const char* display = std::getenv("DISPLAY");
    if (linux_window_backend(display != nullptr && display[0] != '\0')
        == LinuxWindowBackend::X11) {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    }
#endif
}

// DEC-024 / EUI-20260929-002 compatibility boundary. Call on the main thread
// after GLFW initialization and before creating any window. The pinned runner
// first queries dslAppConfig() in exactly that interval; its window backend
// preserves these string hints. Remove when EUI-NEO exposes an app ID option.
inline void configure_application_identity() {
#if defined(__linux__)
    // Must match packaging/linux/aki.desktop (basename and StartupWMClass).
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "aki");
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "aki");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "aki");
#endif
}

}  // namespace aki::ui::platform
