// Aki 主题覆写权威数值表（aki_ui_design §2.1/§2.2/§2.3；设计 §9.1 主题档位
// 覆写清单；M5-02 实体化）。
//
// 本文件是 **EUI-NEO 无关** 的纯数值面：ui/theme/aki_theme.hpp 据此装配
// ThemeColorTokens；回归对照（测试 exe 不链 eui 的约束，DEC-005）以本表为
// 对照锚点——tests/unit/test_ui_theme_values.cpp 持有按设计文档独立抄录的
// 期望值，与本表逐项对拍，防止档位漂移；GUI 宿主运行日志另留「上游默认 →
// 覆写值」对拍证据（M5-01 探针同款）。
#pragma once

#include <cstdint>

namespace aki::ui::theme_values {

// ---- TypographyTokens（§2.1 text-ui-* 体系；默认档 ≠ 本表，逐项覆写）----
inline constexpr float kTypographyTitle = 18.0f;     // ui-xl（上游默认 22）
inline constexpr float kTypographySubtitle = 16.0f;  // ui-lg（上游默认 20）
inline constexpr float kTypographyBody = 14.0f;      // ui-base（上游默认 16）
inline constexpr float kTypographyLabel = 14.0f;     // =ui-base 按钮/控件档（保持）
inline constexpr float kTypographyCaption = 13.0f;   // ui-caption（上游默认 12）
inline constexpr float kTypographyHint = 12.0f;      // ui-sm（上游默认 13）
inline constexpr float kTypographyMicro = 10.0f;     // ui-xs（上游默认 11）
// ui-2xs 9 仅限图轴刻度类「家具」——按组件局部 fontSize 承载，不入档位表。

// ---- RadiusTokens（§2.2 圆角 px 绑定；card/overlay/control 与默认一致）----
inline constexpr float kRadiusSmall = 4.0f;   // rounded-sm（上游默认 6）
inline constexpr float kRadiusControl = 8.0f; // rounded-lg（与默认一致，锚定）
inline constexpr float kRadiusCard = 12.0f;   // rounded-xl（与默认一致，锚定）
inline constexpr float kRadiusOverlay = 16.0f; // rounded-2xl（与默认一致，锚定）

// ---- ControlSizeTokens（§2.2 控件高 24/28/32/36）----
inline constexpr float kControlField = 36.0f;     // 上游默认 35
inline constexpr float kControlMenuItem = 28.0f;  // 上游默认 34

// ---- SpacingTokens（§2.2 4px 基频；与上游默认一致，零覆写——回归对照锚点）----
inline constexpr float kSpacingTiny = 4.0f;
inline constexpr float kSpacingCompact = 8.0f;
inline constexpr float kSpacingContent = 12.0f;
inline constexpr float kSpacingSection = 16.0f;
inline constexpr float kSpacingLarge = 20.0f;
inline constexpr float kSpacingPanel = 24.0f;

// ---- 语义色板（§2.3，值取自上游 Tailwind 默认调色板；hex 0xRRGGBB）----
// ThemeColorTokens 绑定 + AkiSemanticPalette 扩展语义色（aki_theme.hpp）。
struct PaletteHex {
    // ThemeColorTokens 七槽（§2.3 行 → 槽位绑定）。
    std::uint32_t background;        // background
    std::uint32_t primary;           // primary（黑/白，非品牌蓝）
    std::uint32_t surface;           // card / popover / menu
    std::uint32_t surface_hover;     // menu-hover
    std::uint32_t surface_active;    // hover/selected 叠加
    std::uint32_t text;              // text primary
    std::uint32_t border;            // border（fg 10% / 白 10% 预混合）
    // 扩展语义色（AkiSemanticPalette）。
    std::uint32_t primary_foreground;
    std::uint32_t brand;             // sky-400 / sky-500
    std::uint32_t accent;            // sky-50 / sky-950 50% 叠加（预混合）
    std::uint32_t success;           // green-600 / green-500
    std::uint32_t warning;           // yellow-600 / yellow-500
    std::uint32_t destructive;       // red-600 / red-500
    std::uint32_t surface_overlay;   // 3% fg / 5% 白叠加（预混合）
    std::uint32_t surface_overlay_strong;  // 5% fg / 10% 白叠加（预混合）
    // 文本次级不透明度（alpha 通道值，0.0~1.0）：subtle / subtlest。
    float text_subtle_alpha;
    float text_subtlest_alpha;
};

// Light（上游默认主题）：预混合值按 §2.3 行内公式（fg=#404040，bg=#fafafa）。
inline constexpr PaletteHex kLightPalette{
    0xFAFAFA,  // background neutral-50
    0x0A0A0A,  // primary
    0xFFFFFF,  // surface（card）
    0xF5F5F5,  // surface_hover（menu-hover）
    0xE7E7E7,  // surface_active（fg 10% 叠加）
    0x404040,  // text primary
    0xE7E7E7,  // border（fg 10%）
    0xFAFAFA,  // primary_foreground
    0x38BDF8,  // brand sky-400
    0xF0F9FF,  // accent sky-50
    0x16A34A,  // success green-600
    0xCA8A04,  // warning yellow-600
    0xDC2626,  // destructive red-600
    0xF4F4F4,  // surface_overlay（3% fg）
    0xF1F1F1,  // surface_overlay_strong（5% fg）
    0.60f,     // text subtle
    0.40f,     // text subtlest
};

// Dark：预混合值按 §2.3 行内公式（白叠加，bg=#171717）。
inline constexpr PaletteHex kDarkPalette{
    0x171717,  // background neutral-900
    0xFAFAFA,  // primary
    0x262626,  // surface（card neutral-800）
    0x0A0A0A,  // surface_hover（menu-hover neutral-950）
    0x2E2E2E,  // surface_active（白 10% 叠加）
    0xE5E5E5,  // text primary
    0x2E2E2E,  // border（白 10%）
    0x0A0A0A,  // primary_foreground
    0x0EA5E9,  // brand sky-500
    0x102330,  // accent（sky-950 #082F49 50% 叠加）
    0x22C55E,  // success green-500
    0xEAB308,  // warning yellow-500
    0xEF4444,  // destructive red-500
    0x232323,  // surface_overlay（5% 白）
    0x2E2E2E,  // surface_overlay_strong（10% 白）
    0.60f,     // text subtle
    0.30f,     // text subtlest
};

}  // namespace aki::ui::theme_values
