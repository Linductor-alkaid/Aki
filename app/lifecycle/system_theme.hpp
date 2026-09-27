// 系统主题查询（aki_design §9.1 Settings 页与主题三选装配条款，M5-07；
// DEC-005 已知缺口「无系统主题检测 API」的 Aki 平台层处置路径）。
//
// RULE-10：公开面仅 std/aki 类型（SystemTheme 枚举，ui/theme/theme_mode.hpp）
// ——平台 API（Windows 注册表）封死在本编译单元的条件编译分支内。查询为
// 有界单次注册表读取（点击回调/组合根首帧装配上下文可调，无等待无轮询）。
// 非 Windows 平台/查询失败返回 Unknown（调用方回落 Light 并披露平台支持
// 面——不猜测）。
#pragma once

#include "ui/theme/theme_mode.hpp"

namespace aki::app {

[[nodiscard]] aki::ui::SystemTheme query_system_theme();

}  // namespace aki::app
