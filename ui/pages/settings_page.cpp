#include "ui/i18n.hpp"
// Settings 页实现（语义见 settings_page.hpp；aki_ui_design §4 Settings 页
// 组件映射：segmented 三选 + 只读设置项；§9.1 compose 三不纪律——
// FollowSystem 的系统主题查询仅在点击回调上下文（有界注册表读取）执行，
// compose 只读派生）。
#include "ui/pages/settings_page.hpp"
#include "ui/components/secure_input.hpp"

#include "components/dialog.h"

#include "ui/pages/main_window.hpp"

#include "app/lifecycle/system_theme.hpp"
#include "components/input.h"
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
    float height, float screen_width, float screen_height,
    MainWindowModel& model) {
    (void)height;  // 纵向自然布局，无底部锚定元素（M5-07 形态）。
    const auto& metrics = tokens.metrics;
    const float pad_x = x + metrics.spacing.section;
    const float content_width = width - metrics.spacing.section * 2.0f;
    const float segmented_width = std::min(420.0f, content_width);

    components::text(ui, "aki.settings.title")
        .text(tr("Settings"))
        .position(pad_x, y + metrics.spacing.section)
        .fontSize(metrics.typography.title)
        .fontWeight(600)
        .color(tokens.text)
        .build();
    const float language_x = pad_x + content_width - 186.0f;
    components::text(ui, "aki.settings.language.label")
        .text(tr("Language"))
        .position(language_x, y + metrics.spacing.compact)
        .fontSize(metrics.typography.caption)
        .color(semantic.text_subtle).build();
    for (int index = 0; index < 2; ++index) {
        const auto selected = index == 0 ? Language::Chinese
                                         : Language::English;
        components::button(ui, "aki.settings.language." + std::to_string(index))
            .position(language_x + index * 94.0f,
                y + metrics.spacing.compact + metrics.typography.caption
                    + metrics.spacing.tiny)
            .size(88.0f, metrics.control.menuItem)
            .text(tr(index == 0 ? "Chinese" : "English"))
            .theme(tokens, model.language == selected)
            .textColor(model.language == selected
                ? semantic.primary_foreground : tokens.text)
            .onClick([&model, selected] {
                if (model.actions && model.actions->set_language
                    && model.actions->set_language(
                        selected == Language::Chinese ? "zh-CN" : "en")) {
                    model.language = selected;
                    set_language(selected);
                } else {
                    model.last_action_feedback = tr("Language save failed");
                }
            }).build();
    }

    // ---- 主题三选（§4 segmented 映射；页面持有 UI 态）----
    float row_y = y + metrics.spacing.section + metrics.typography.title
        + metrics.spacing.section;
    components::text(ui, "aki.settings.theme.label")
        .text(tr("Theme"))
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
                .items({tr(kThemeSegments[0]), tr(kThemeSegments[1]),
                    tr(kThemeSegments[2])})
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
        .text(tr("Follow system uses Windows settings.\nOther systems use Light.\nTheme choice lasts this session."))
        .position(pad_x, row_y)
        .fontSize(metrics.typography.caption)
        .wrap(true)
        .maxWidth(content_width)
        .color(tokens.text)
        .build();

    // ---- 最小设置项（只读展示）----
    row_y += metrics.typography.caption * 3.0f + metrics.spacing.section;
    components::text(ui, "aki.settings.data.label")
        .text(tr("Data directory"))
        .position(pad_x, row_y)
        .fontSize(metrics.typography.body)
        .fontWeight(600)
        .color(tokens.text)
        .build();
    row_y += metrics.typography.body + metrics.spacing.tiny;
    components::text(ui, "aki.settings.data.value")
        .text(model.data_directory.empty()
                ? tr("(data directory unavailable)")
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
        .text(tr("Local device"))
        .position(pad_x, row_y)
        .fontSize(metrics.typography.body)
        .fontWeight(600)
        .color(tokens.text)
        .build();
    row_y += metrics.typography.body + metrics.spacing.tiny;
    components::text(ui, "aki.settings.identity.value")
        .text(model.state_view.local_device.empty()
                ? tr("(identity unavailable)")
                : model.state_view.local_device.value)
        .position(pad_x, row_y)
        .fontSize(metrics.typography.caption)
        .fontFamily("Mono")
        .wrap(true)
        .maxWidth(content_width)
        .color(semantic.text_subtle)
        .build();

    // ---- 本机名改名（M5-16）：草稿为空时回显状态中的当前名；保存成功清空
    // 草稿回显新名。改名经 HostRuntime 字段级更新并热更新名称广播。
    if (model.actions && model.actions->set_device_name) {
        std::string current_name;
        for (const auto& device : model.state_view.devices) {
            if (device.id == model.state_view.local_device
                && !device.display_name.empty()) {
                current_name = device.display_name;
                break;
            }
        }
        row_y += metrics.typography.caption + metrics.spacing.content;
        const float name_width = std::min(320.0f, content_width);
        components::input(ui, "aki.settings.name")
            .position(pad_x, row_y)
            .size(name_width, metrics.control.field)
            .theme(tokens)
            .value(model.settings_device_name_draft.empty()
                ? current_name : model.settings_device_name_draft)
            .placeholder(tr("Device name"))
            .onChange([&model](const std::string& value) {
                model.settings_device_name_draft = value;
            }).build();
        components::button(ui, "aki.settings.name.save")
            .position(pad_x + name_width + metrics.spacing.compact, row_y)
            .size(96.0f, metrics.control.field)
            .text(tr("Save"))
            .theme(tokens, false)
            .onClick([&model] {
                if (model.settings_device_name_draft.empty()) return;
                if (model.actions->set_device_name(
                        model.settings_device_name_draft)) {
                    model.settings_device_name_draft.clear();
                    set_feedback(model, tr("Device name updated"));
                } else {
                    set_feedback(model, tr("Device name save failed"));
                }
            }).build();
    }

    row_y += metrics.typography.caption + metrics.spacing.section;
    if (model.actions && model.actions->set_local_pairing_password) {
        components::button(ui, "aki.settings.password.open")
            .position(pad_x, row_y)
            .size(std::min(260.0f, content_width), metrics.control.field)
            .text(tr("Change local pairing password"))
            .theme(tokens, false)
            .onClick([&model] {
                model.password_feedback.clear();
                model.password_change_open = true;
            })
            .build();
    }

    if (model.password_change_open && model.actions) {
        const float dialog_width = std::min(560.0f, screen_width - 32.0f);
        components::dialog(ui, "aki.settings.password.dialog")
            .open(true).theme(tokens).screen(screen_width, screen_height)
            .size(dialog_width, 330.0f)
            .content([&] {
                components::text(ui, "aki.settings.password.title")
                    .text(tr("Change local pairing password"))
                    .position(metrics.spacing.section,
                        metrics.spacing.section)
                    .fontSize(metrics.typography.subtitle).fontWeight(600)
                    .color(tokens.text).build();
                components::text(ui, "aki.settings.password.help")
                    .text(tr("Other devices will need the new password when pairing."))
                    .position(metrics.spacing.section, 63.0f)
                    .fontSize(metrics.typography.caption)
                    .color(semantic.text_subtle).build();
                secureInput(ui, "aki.settings.password.new",
                    model.local_password_draft, tokens,
                    metrics.spacing.section, 105.0f,
                    dialog_width - metrics.spacing.section * 2.0f,
                    metrics.control.field, tr("New password (8+ characters)"));
                secureInput(ui, "aki.settings.password.confirm",
                    model.local_password_confirm, tokens,
                    metrics.spacing.section, 165.0f,
                    dialog_width - metrics.spacing.section * 2.0f,
                    metrics.control.field, tr("Confirm new password"));
                if (!model.password_feedback.empty()) {
                    components::text(ui, "aki.settings.password.error")
                        .text(tr(model.password_feedback))
                        .position(metrics.spacing.section, 219.0f)
                        .fontSize(metrics.typography.caption)
                        .color(semantic.destructive).build();
                }
                components::button(ui, "aki.settings.password.save")
                    .position(metrics.spacing.section, 260.0f)
                    .size(160.0f, metrics.control.field)
                    .text(tr("Save password"))
                    .theme(tokens, true)
                    .textColor(semantic.primary_foreground)
                    .onClick([&model] {
                        if (utf8_scalar_count(model.local_password_draft) < 8) {
                            model.password_feedback =
                                "Use at least 8 characters";
                            return;
                        }
                        if (model.local_password_draft
                            != model.local_password_confirm) {
                            model.password_feedback =
                                "Passwords do not match";
                            return;
                        }
                        std::string error;
                        const bool saved =
                            model.actions->set_local_pairing_password(
                                std::move(model.local_password_draft), error);
                        clear_secret(model.local_password_draft);
                        clear_secret(model.local_password_confirm);
                        model.password_feedback = saved ? "" : error;
                        if (saved) model.last_action_feedback =
                            tr("Local pairing password updated");
                        if (saved) model.password_change_open = false;
                    }).build();
                components::button(ui, "aki.settings.password.cancel")
                    .position(dialog_width - metrics.spacing.section - 140.0f,
                        260.0f)
                    .size(140.0f, metrics.control.field)
                    .text(tr("Cancel")).theme(tokens, false)
                    .onClick([&model] {
                        model.password_change_open = false;
                        model.password_feedback.clear();
                        clear_secret(model.local_password_draft);
                        clear_secret(model.local_password_confirm);
                    }).build();
            })
            .onOpenChange([&model](bool open) {
                if (!open) {
                    model.password_change_open = false;
                    model.password_feedback.clear();
                    clear_secret(model.local_password_draft);
                    clear_secret(model.local_password_confirm);
                }
            }).build();
    }
}

}  // namespace aki::ui
