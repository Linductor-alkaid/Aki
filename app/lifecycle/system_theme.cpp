// 系统主题查询实现（语义见 system_theme.hpp；平台条件编译单元——
// persistence/storage/data_root.cpp 先例：平台分支条件编译隔离，公开面
// 仅 std/aki 类型）。
//
// Windows：用户「应用浅色/深色」偏好 =
// HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize
// 的 AppsUseLightTheme（DWORD，0 = 深色）；注册表缺失/类型不符/读失败
// 一律 Unknown（调用方回落 Light 并披露——不猜测，RULE-09 同精神）。
#include "app/lifecycle/system_theme.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace aki::app {

#if defined(_WIN32)

aki::ui::SystemTheme query_system_theme() {
    DWORD light = 1;
    DWORD size = sizeof(light);
    const LSTATUS status = ::RegGetValueA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        "AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    if (status != ERROR_SUCCESS) {
        return aki::ui::SystemTheme::Unknown;
    }
    return light == 0 ? aki::ui::SystemTheme::Dark
                      : aki::ui::SystemTheme::Light;
}

#else

aki::ui::SystemTheme query_system_theme() {
    // 非 Windows 平台：无零依赖的系统主题查询面（XDG portal 需 dbus 依赖，
    // 不引入——DEC-005 供应链纪律）；Unknown → 调用方回落 Light 并披露。
    return aki::ui::SystemTheme::Unknown;
}

#endif

}  // namespace aki::app
