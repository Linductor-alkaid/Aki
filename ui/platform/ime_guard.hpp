// Windows platform adapter: IME owns Backspace while composition is active.
// The pinned input widget handles Backspace itself when its previous-frame
// preedit string is available; this guard closes the native event-order gap.
#pragma once

#ifdef _WIN32
#include "third_party/EUI-NEO/3rd/glfw/include/GLFW/glfw3.h"
#include "third_party/EUI-NEO/core/platform/ime_bridge.h"

namespace aki::ui::platform {

inline GLFWkeyfun& previous_key_callback() {
    static GLFWkeyfun callback = nullptr;
    return callback;
}

inline void install_ime_backspace_guard() {
    static bool installed = false;
    if (installed) return;
    GLFWwindow* window = glfwGetCurrentContext();
    if (window == nullptr) return;
    previous_key_callback() = glfwSetKeyCallback(window,
        [](GLFWwindow* current, int key, int scan_code,
            int action, int modifiers) {
            if (key == GLFW_KEY_BACKSPACE && action != GLFW_RELEASE
                && eui_ime_is_composing(current) != 0) {
                return;
            }
            if (auto callback = previous_key_callback())
                callback(current, key, scan_code, action, modifiers);
        });
    installed = true;
}

}  // namespace aki::ui::platform
#else
namespace aki::ui::platform {
inline void install_ime_backspace_guard() {}
}
#endif
