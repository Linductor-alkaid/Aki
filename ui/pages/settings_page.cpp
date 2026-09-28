// Settings 页实现（语义见 settings_page.hpp；aki_ui_design §4 Settings 页
// 组件映射：segmented 三选 + 只读设置项；§9.1 compose 三不纪律——
// FollowSystem 的系统主题查询仅在点击回调上下文（有界注册表读取）执行，
// compose 只读派生）。
#include "ui/pages/settings_page.hpp"

#include "ui/pages/main_window.hpp"

#include "app/lifecycle/system_theme.hpp"
#include "components/segmented.h"
#include "components/text.h"

#include <algorithm>
#include <string>
#include <utility>

namespace aki::ui {
namespace {

using components::theme::ThemeColorTokens;

// 段标签（§2.5 容忍翻译变长；segmented 宽度三等分）。
constexpr const char* kThemeSegments[] = {"Follow system", "Light", "Dark"};

void set_feedback(MainWindowModel& model, std::string text) {
    model.last_action_feedback = std::move(text);
}

}  // namespace

void composeSettingsPage(eui::Ui& ui, const ThemeColorTokens& tokens,
    const AkiSemanticPalette& semantic, float x, float y, float width,
    float height, MainWindowModel& model) {
    (void)height;  // 纵向自然布局，无底部锚定元素（M5-07 形态）。
    const auto& metrics = tokens.metrics;
    const float pad_x = x + metrics.spacing.section;
    const float content_width = width - metrics.spacing.section * 2.0f;
    const float segmented_width = std::min(420.0f, content_width);

    components::text(ui, "aki.settings.title")
        .text("Settings")
        .position(pad_x, y + metrics.spacing.section)
        .fontSize(metrics.typography.title)
        .fontWeight(600)
        .color(tokens.text)
        .build();

    // ---- 主题三选（§4 segmented 映射；页面持有 UI 态）----
    float row_y = y + metrics.spacing.section + metrics.typography.title
        + metrics.spacing.section;
    components::text(ui, "aki.settings.theme.label")
        .text("Theme")
        .position(pad_x, row_y)
        .fontSize(metrics.typography.body)
        .fontWeight(600)
        .color(tokens.text)
        .build();
    row_y += metrics.typography.body + metrics.spacing.compact;
    ui.stack("aki.settings.theme.segmented.wrap")
        .position(pad_x, row_y)
        .size(segmented_width, metrics.control.field)
        .content([&] {
            components::segmented(ui, "aki.settings.theme.segmented")
                .size(segmented_width, metrics.control.field)
                .items({kThemeSegments[0], kThemeSegments[1],
                    kThemeSegments[2]})
                .selected(static_cast<int>(model.theme_setting))
                .fontSize(metrics.typography.caption)
                // 上游 SegmentedStyle 深色档的选中文字与 indicator 同为
                // primary（白上白不可见，GUI 实测）——选中文字改用 §2.3
                // primary-foreground 配对（浅=primary 反白、深=黑）。
                .style([&] {
                    components::SegmentedStyle style(tokens);
                    style.selectedText = semantic.primary_foreground;
                    return style;
                }())
                .onChange([&model](int index) {
                    // 点击回调上下文（主线程事件处理，非 compose 树构建内）。
                    const auto setting = static_cast<ThemeSetting>(index);
                    aki::ui::SystemTheme system =
                        aki::ui::SystemTheme::Light;
                    if (setting == ThemeSetting::FollowSystem) {
                        // 有界注册表读取（点击回调；Windows）——查询失败
                        // Unknown 由 resolve 回落 Light。
                        system = aki::app::query_system_theme();
                    }
                    model.theme_setting = setting;
                    model.theme =
                        resolve_effective_theme(setting, system);
                    set_feedback(model,
                        setting == ThemeSetting::FollowSystem
                            ? std::string("theme: follow system (")
                                + (system == aki::ui::SystemTheme::Dark
                                        ? "dark"
                                        : "light")
                                + ")"
                            : std::string("theme: ")
                                + kThemeSegments[index]);
                })
                .build();
        })
        .build();
    row_y += metrics.control.field + metrics.spacing.compact;
    components::text(ui, "aki.settings.theme.disclosure")
        .text("Follow system uses Windows settings.\nOther systems use Light.\nTheme choice lasts this session.")
        .position(pad_x, row_y)
        .fontSize(metrics.typography.caption)
        .wrap(true)
        .maxWidth(content_width)
        .color(tokens.text)
        .build();

    // ---- 最小设置项（只读展示）----
    row_y += metrics.typography.caption * 3.0f + metrics.spacing.section;
    components::text(ui, "aki.settings.data.label")
        .text("Data directory")
        .position(pad_x, row_y)
        .fontSize(metrics.typography.body)
        .fontWeight(600)
        .color(tokens.text)
        .build();
    row_y += metrics.typography.body + metrics.spacing.tiny;
    components::text(ui, "aki.settings.data.value")
        .text(model.data_directory.empty()
                ? "(data directory unavailable)"
                : model.data_directory)
        .position(pad_x, row_y)
        .fontSize(metrics.typography.caption)
        .fontFamily("Mono")
        .wrap(true)
        .maxWidth(content_width)
        .color(semantic.text_subtle)
        .build();

    row_y += metrics.typography.caption * 3.0f + metrics.spacing.content;
    components::text(ui, "aki.settings.identity.label")
        .text("Local device")
        .position(pad_x, row_y)
        .fontSize(metrics.typography.body)
        .fontWeight(600)
        .color(tokens.text)
        .build();
    row_y += metrics.typography.body + metrics.spacing.tiny;
    components::text(ui, "aki.settings.identity.value")
        .text(model.state_view.local_device.empty()
                ? "(identity unavailable)"
                : model.state_view.local_device.value)
        .position(pad_x, row_y)
        .fontSize(metrics.typography.caption)
        .fontFamily("Mono")
        .wrap(true)
        .maxWidth(content_width)
        .color(semantic.text_subtle)
        .build();
}

}  // namespace aki::ui
