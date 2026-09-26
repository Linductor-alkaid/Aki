// akiTheme()——按设计 §9.1 主题档位覆写清单装配 EUI-NEO ThemeColorTokens
// （M5-02 实体化；数值权威面为 EUI-NEO 无关的 aki_theme_values.hpp，回归对照
// 由 tests/unit/test_ui_theme_values.cpp + GUI 运行日志对拍承载）。
//
// 页面只消费语义名（akiTheme(mode) / akiSemanticColors(mode)）；不依赖上游
// 默认档（DEC-005：默认档 ≠ aki_ui_design §2.1/§2.2 表，逐项覆写）。
#pragma once

#include "components/theme.h"
#include "core/render/render_types.h"

#include <cstdint>

namespace aki::ui {

enum class ThemeMode : std::uint8_t {
    Light,
    Dark,
};

// §2.3 扩展语义色（ThemeColorTokens 七槽之外的领域语义色；状态视觉语义
// §3 的 success/warning/destructive 与品牌/文本次级档在此提供）。
struct AkiSemanticPalette {
    core::Color primary_foreground;
    core::Color brand;         // 克制使用，永不做整面背景（§2.3 使用纪律）
    core::Color accent;
    core::Color success;       // 状态色只表达真实状态
    core::Color warning;
    core::Color destructive;
    core::Color card;          // 与 ThemeColorTokens.surface 同源（语义别名）
    core::Color menu_hover;
    core::Color surface_overlay;         // 3% fg / 5% 白
    core::Color surface_overlay_strong;  // 5% fg / 10% 白
    core::Color text_subtle;             // text primary × 60%
    core::Color text_subtlest;           // × 40%（light）/ 30%（dark）
};

// 一次性装配覆写后的主题档（light()/dark() 基底 + §9.1 覆写清单逐项覆写）。
[[nodiscard]] components::theme::ThemeColorTokens akiTheme(ThemeMode mode);

// 扩展语义色（与 akiTheme 同源数值）。
[[nodiscard]] AkiSemanticPalette akiSemanticColors(ThemeMode mode);

// 回归对照（RULE-11 本机证据 + 调试自检）：覆写值 vs 上游默认档的关键差分
// 摘要（"22->18" 形态），GUI 宿主启动日志落盘用。
[[nodiscard]] const char* akiThemeOverrideDiffSummary(ThemeMode mode);

}  // namespace aki::ui
