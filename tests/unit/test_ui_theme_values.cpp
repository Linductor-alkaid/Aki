// M5-02：主题档位覆写回归对照（aki_ui_design §2.1/§2.2/§2.3 → 设计 §9.1
// 覆写清单）。
//
// 测试 exe 不链 eui（DEC-005：console 约定 + tsan ctest 不加载显示栈），
// 因此对照面是 EUI-NEO 无关的数值权威表 ui/theme/aki_theme_values.hpp；
// 本文件按设计文档**独立抄录**期望值（与本表构成双份锚点——任一侧漂移
// 即失败）。上游默认档 vs 覆写值的运行期对拍由 GUI 宿主启动日志归档
// （aki-run.log，M5-01 探针同款；RULE-11）。
#include "ui/theme/aki_theme_values.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {

namespace tv = aki::ui::theme_values;

// §2.3 行内预混合公式（fg 叠加 over bg，逐通道四舍五入）。
constexpr std::uint32_t blend(std::uint32_t bg, std::uint32_t fg, float alpha) {
    const auto channel = [alpha](unsigned int b, unsigned int f) {
        const float mixed =
            static_cast<float>(b) * (1.0f - alpha) + static_cast<float>(f) * alpha;
        return static_cast<unsigned int>(mixed + 0.5f) & 0xFF;
    };
    return (channel((bg >> 16) & 0xFF, (fg >> 16) & 0xFF) << 16)
        | (channel((bg >> 8) & 0xFF, (fg >> 8) & 0xFF) << 8)
        | (channel(bg & 0xFF, fg & 0xFF));
}

}  // namespace

TEST_CASE("Typography overrides match aki_ui_design 2.1", "[ui][theme]") {
    REQUIRE(tv::kTypographyTitle == 18.0f);     // ui-xl（上游默认 22）
    REQUIRE(tv::kTypographySubtitle == 16.0f);  // ui-lg（上游默认 20）
    REQUIRE(tv::kTypographyBody == 14.0f);      // ui-base（上游默认 16）
    REQUIRE(tv::kTypographyLabel == 14.0f);     // =ui-base（按钮/控件档，保持）
    REQUIRE(tv::kTypographyCaption == 13.0f);   // ui-caption（上游默认 12）
    REQUIRE(tv::kTypographyHint == 12.0f);      // ui-sm（上游默认 13）
    REQUIRE(tv::kTypographyMicro == 10.0f);     // ui-xs（上游默认 11）
}

TEST_CASE("Radius and control size overrides match aki_ui_design 2.2",
    "[ui][theme]") {
    REQUIRE(tv::kRadiusSmall == 4.0f);    // rounded-sm（上游默认 6）
    REQUIRE(tv::kRadiusControl == 8.0f);  // rounded-lg
    REQUIRE(tv::kRadiusCard == 12.0f);    // rounded-xl
    REQUIRE(tv::kRadiusOverlay == 16.0f); // rounded-2xl
    REQUIRE(tv::kControlField == 36.0f);    // 上游默认 35
    REQUIRE(tv::kControlMenuItem == 28.0f); // 上游默认 34
}

TEST_CASE("Spacing stays at the 4px grid (zero-override anchor)", "[ui][theme]") {
    REQUIRE(tv::kSpacingTiny == 4.0f);
    REQUIRE(tv::kSpacingCompact == 8.0f);
    REQUIRE(tv::kSpacingContent == 12.0f);
    REQUIRE(tv::kSpacingSection == 16.0f);
    REQUIRE(tv::kSpacingLarge == 20.0f);
    REQUIRE(tv::kSpacingPanel == 24.0f);
}

TEST_CASE("Light palette matches aki_ui_design 2.3 upstream default theme",
    "[ui][theme]") {
    REQUIRE(tv::kLightPalette.background == 0xFAFAFA);  // neutral-50
    REQUIRE(tv::kLightPalette.primary == 0x0A0A0A);
    REQUIRE(tv::kLightPalette.primary_foreground == 0xFAFAFA);
    REQUIRE(tv::kLightPalette.surface == 0xFFFFFF);  // card/popover/menu
    REQUIRE(tv::kLightPalette.surface_hover == 0xF5F5F5);  // menu-hover
    REQUIRE(tv::kLightPalette.text == 0x404040);
    REQUIRE(tv::kLightPalette.brand == 0x38BDF8);  // sky-400
    REQUIRE(tv::kLightPalette.accent == 0xF0F9FF); // sky-50
    REQUIRE(tv::kLightPalette.success == 0x16A34A);      // green-600
    REQUIRE(tv::kLightPalette.warning == 0xCA8A04);      // yellow-600
    REQUIRE(tv::kLightPalette.destructive == 0xDC2626);  // red-600
    REQUIRE(tv::kLightPalette.text_subtle_alpha == 0.60f);
    REQUIRE(tv::kLightPalette.text_subtlest_alpha == 0.40f);

    // 预混合复核：fg(#404040) 叠加 over bg(#fafafa)。
    REQUIRE(tv::kLightPalette.border
        == blend(0xFAFAFA, 0x404040, 0.10f));  // fg 10%
    REQUIRE(tv::kLightPalette.surface_active
        == blend(0xFAFAFA, 0x404040, 0.10f));  // hover/selected fg 10%
    REQUIRE(tv::kLightPalette.surface_overlay
        == blend(0xFAFAFA, 0x404040, 0.03f));  // surface 3% fg
    REQUIRE(tv::kLightPalette.surface_overlay_strong
        == blend(0xFAFAFA, 0x404040, 0.05f));  // surface-hover 5% fg
}

TEST_CASE("Dark palette matches aki_ui_design 2.3 dark values", "[ui][theme]") {
    REQUIRE(tv::kDarkPalette.background == 0x171717);  // neutral-900
    REQUIRE(tv::kDarkPalette.primary == 0xFAFAFA);
    REQUIRE(tv::kDarkPalette.primary_foreground == 0x0A0A0A);
    REQUIRE(tv::kDarkPalette.surface == 0x262626);          // neutral-800
    REQUIRE(tv::kDarkPalette.surface_hover == 0x0A0A0A);    // neutral-950
    REQUIRE(tv::kDarkPalette.text == 0xE5E5E5);
    REQUIRE(tv::kDarkPalette.brand == 0x0EA5E9);  // sky-500
    REQUIRE(tv::kDarkPalette.success == 0x22C55E);      // green-500
    REQUIRE(tv::kDarkPalette.warning == 0xEAB308);      // yellow-500
    REQUIRE(tv::kDarkPalette.destructive == 0xEF4444);  // red-500
    REQUIRE(tv::kDarkPalette.text_subtle_alpha == 0.60f);
    REQUIRE(tv::kDarkPalette.text_subtlest_alpha == 0.30f);

    // 预混合复核：白(#ffffff) 叠加 over bg(#171717)。
    REQUIRE(tv::kDarkPalette.border
        == blend(0x171717, 0xFFFFFF, 0.10f));  // 白 10%
    REQUIRE(tv::kDarkPalette.surface_active
        == blend(0x171717, 0xFFFFFF, 0.10f));  // hover/selected 白 10%
    REQUIRE(tv::kDarkPalette.surface_overlay
        == blend(0x171717, 0xFFFFFF, 0.05f));  // surface 5% 白
    REQUIRE(tv::kDarkPalette.surface_overlay_strong
        == blend(0x171717, 0xFFFFFF, 0.10f));  // surface-hover 10% 白
    // accent：sky-950(#082f49) 50% 叠加 over bg。
    REQUIRE(tv::kDarkPalette.accent == blend(0x171717, 0x082F49, 0.50f));
}

TEST_CASE("Mode invariants: primary inversion and brand step", "[ui][theme]") {
    // §2.3：primary 为黑/白反转对（非品牌蓝）；brand light=sky-400、dark=sky-500。
    REQUIRE(tv::kLightPalette.primary == tv::kDarkPalette.primary_foreground);
    REQUIRE(tv::kDarkPalette.primary == tv::kLightPalette.primary_foreground);
    REQUIRE(tv::kLightPalette.brand != tv::kDarkPalette.brand);
    REQUIRE(tv::kLightPalette.background != tv::kDarkPalette.background);
}
