// 主题档位与选择语义（aki_ui_design §4 Settings 页 segmented 三选；
// aki_design §9.1 Settings 页与主题三选装配条款，M5-07）。
//
// 本头文件为 EUI-NEO 无关的纯 std 面（RULE-10）：ThemeMode 为 akiTheme()/
// akiSemanticColors() 的档位参数（原 aki_theme.hpp 内定义，M5-07 迁出——
// 主题选择的解析状态机须可被网络无关单测直链消费，DEC-005「测试 exe 不链
// eui」）；ThemeSetting 为页面持有的三选 UI 态；SystemTheme 为平台查询
// 结果（app/lifecycle/system_theme，Windows 用户偏好/其余 Unknown）。
#pragma once

#include <cstdint>

namespace aki::ui {

// 生效档位（akiTheme()/akiSemanticColors() 参数）。
enum class ThemeMode : std::uint8_t {
    Light,
    Dark,
};

// 页面持有的主题三选（§4 Settings 页 segmented：跟随系统/浅/深）。
enum class ThemeSetting : std::uint8_t {
    FollowSystem,
    Light,
    Dark,
};

// 平台系统主题查询结果（Unknown = 平台不支持或查询失败）。
enum class SystemTheme : std::uint8_t {
    Light,
    Dark,
    Unknown,
};

// 生效档位解析（纯函数，§9.1 Settings 页装配条款）：Light/Dark 直取；
// FollowSystem → 系统查询结果，Unknown 回落 Light（不猜测——回落可见，
// 页内披露平台支持面）。
[[nodiscard]] constexpr ThemeMode resolve_effective_theme(
    ThemeSetting setting, SystemTheme system) noexcept {
    switch (setting) {
    case ThemeSetting::Light:
        return ThemeMode::Light;
    case ThemeSetting::Dark:
        return ThemeMode::Dark;
    case ThemeSetting::FollowSystem:
        break;
    }
    return system == SystemTheme::Dark ? ThemeMode::Dark : ThemeMode::Light;
}

}  // namespace aki::ui
