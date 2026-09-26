// akiTheme() 实现（数值来自 aki_theme_values.hpp，§9.1 覆写清单逐项应用）。
#include "ui/theme/aki_theme.hpp"

#include "ui/theme/aki_theme_values.hpp"

#include <algorithm>
#include <string>

namespace aki::ui {
namespace {

core::Color from_hex(std::uint32_t rgb, float alpha = 1.0f) {
    return core::Color{
        static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(rgb & 0xFF) / 255.0f,
        std::clamp(alpha, 0.0f, 1.0f)};
}

core::Color with_alpha(core::Color value, float alpha) {
    value.a = std::clamp(alpha, 0.0f, 1.0f);
    return value;
}

const theme_values::PaletteHex& palette_for(ThemeMode mode) {
    return mode == ThemeMode::Dark ? theme_values::kDarkPalette
                                   : theme_values::kLightPalette;
}

}  // namespace

components::theme::ThemeColorTokens akiTheme(ThemeMode mode) {
    // 基底取上游 light()/dark()（dark 标志与合成函数 fieldVisuals/shadow 的
    // 深浅分支依赖它），随后按 §9.1 覆写清单逐项覆写——不依赖上游默认档位。
    components::theme::ThemeColorTokens tokens =
        mode == ThemeMode::Dark ? components::theme::dark()
                                : components::theme::light();
    const theme_values::PaletteHex& palette = palette_for(mode);

    // §2.3 语义色板 → ThemeColorTokens 七槽。
    tokens.background = from_hex(palette.background);
    tokens.primary = from_hex(palette.primary);
    tokens.surface = from_hex(palette.surface);
    tokens.surfaceHover = from_hex(palette.surface_hover);
    tokens.surfaceActive = from_hex(palette.surface_active);
    tokens.text = from_hex(palette.text);
    tokens.border = from_hex(palette.border);
    tokens.dark = (mode == ThemeMode::Dark);

    // §2.1 排印覆写（title 22→18、subtitle 20→16、body 16→14、caption 12→13、
    // hint 13→12、micro 11→10；label 14 保持）。
    auto& typography = tokens.metrics.typography;
    typography.title = theme_values::kTypographyTitle;
    typography.subtitle = theme_values::kTypographySubtitle;
    typography.body = theme_values::kTypographyBody;
    typography.label = theme_values::kTypographyLabel;
    typography.caption = theme_values::kTypographyCaption;
    typography.hint = theme_values::kTypographyHint;
    typography.micro = theme_values::kTypographyMicro;

    // §2.2 圆角覆写（small 6→4；card/overlay/control 锚定同值）。
    auto& radius = tokens.metrics.radius;
    radius.small = theme_values::kRadiusSmall;
    radius.control = theme_values::kRadiusControl;
    radius.card = theme_values::kRadiusCard;
    radius.overlay = theme_values::kRadiusOverlay;

    // §2.2 控件尺寸覆写（field 35→36、menuItem 34→28）。
    auto& control = tokens.metrics.control;
    control.field = theme_values::kControlField;
    control.menuItem = theme_values::kControlMenuItem;

    // §2.2 间距：与上游默认一致，锚定同值（零覆写声明 + 防漂移）。
    auto& spacing = tokens.metrics.spacing;
    spacing.tiny = theme_values::kSpacingTiny;
    spacing.compact = theme_values::kSpacingCompact;
    spacing.content = theme_values::kSpacingContent;
    spacing.section = theme_values::kSpacingSection;
    spacing.large = theme_values::kSpacingLarge;
    spacing.panel = theme_values::kSpacingPanel;

    return tokens;
}

AkiSemanticPalette akiSemanticColors(ThemeMode mode) {
    const theme_values::PaletteHex& palette = palette_for(mode);
    AkiSemanticPalette colors;
    colors.primary_foreground = from_hex(palette.primary_foreground);
    colors.brand = from_hex(palette.brand);
    colors.accent = from_hex(palette.accent);
    colors.success = from_hex(palette.success);
    colors.warning = from_hex(palette.warning);
    colors.destructive = from_hex(palette.destructive);
    colors.card = from_hex(palette.surface);
    colors.menu_hover = from_hex(palette.surface_hover);
    colors.surface_overlay = from_hex(palette.surface_overlay);
    colors.surface_overlay_strong = from_hex(palette.surface_overlay_strong);
    const core::Color text = from_hex(palette.text);
    colors.text_subtle = with_alpha(text, palette.text_subtle_alpha);
    colors.text_subtlest = with_alpha(text, palette.text_subtlest_alpha);
    return colors;
}

const char* akiThemeOverrideDiffSummary(ThemeMode mode) {
    // 与 M5-01 探针 log_theme_override 同口径的关键差分（上游默认 → 覆写值）；
    // 间距六档与默认一致（零覆写）不在差分内，见 GUI 启动日志完整对拍。
    (void)mode;  // 排印/圆角/尺寸覆写在两档同值；色板差分见 akiSemanticColors。
    return "typography.title 22->18 subtitle 20->16 body 16->14 caption 12->13"
           " hint 13->12 micro 11->10 | radius.small 6->4 | control.field"
           " 35->36 menuItem 34->28";
}

}  // namespace aki::ui
