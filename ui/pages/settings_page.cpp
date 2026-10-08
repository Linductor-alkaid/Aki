#include "ui/i18n.hpp"
// Settings 页实现（语义见 settings_page.hpp；aki_ui_design §4 Settings 页
// 组件映射：segmented 三选 + 只读设置项；§9.1 compose 三不纪律——
// FollowSystem 的系统主题查询仅在点击回调上下文（有界注册表读取）执行，
// compose 只读派生）。
#include "ui/pages/settings_page.hpp"
#include "ui/components/fold.hpp"
#include "ui/components/secure_input.hpp"

#include "components/dialog.h"

#include "ui/pages/main_window.hpp"

#include "app/lifecycle/system_theme.hpp"
#include "components/input.h"
#include "components/segmented.h"
#include "components/text.h"

#include <algorithm>
#include <stdexcept>
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

// relay 连接态语义名 → 展示文案（state_name 为上游 RelayNodeState 语义名，
// 数值不跨层解释——aki_design §8.1 同款纪律；M7/DEC-028 决策 8）。
std::string relay_connection_label(const std::string& state_name) {
    if (state_name == "ready") return tr("connected");
    if (state_name == "starting") return tr("connecting");
    if (state_name == "degraded") return tr("reconnecting");
    if (state_name == "failed") return tr("connection failed");
    if (state_name == "stopped") return tr("stopped");
    return tr("not connected");
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

    // ---- 本机名改名（M5-16）：未编辑时回显状态中的当前名；用户一旦编辑
    // （含清空）即显示草稿，可清空重输。保存成功清空草稿回显新名；空名
    // 提示命名规则。改名经 HostRuntime 字段级更新并热更新名称广播。
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
            .value(model.settings_name_dirty
                ? model.settings_device_name_draft : current_name)
            .placeholder(tr("Device name"))
            .onChange([&model](const std::string& value) {
                model.settings_device_name_draft = value;
                model.settings_name_dirty = true;
            }).build();
        components::button(ui, "aki.settings.name.save")
            .position(pad_x + name_width + metrics.spacing.compact, row_y)
            .size(96.0f, metrics.control.field)
            .text(tr("Save"))
            .theme(tokens, false)
            .onClick([&model] {
                if (!model.settings_name_dirty) return;
                if (model.settings_device_name_draft.empty()) {
                    set_feedback(model,
                        tr("Device name must be 1-64 bytes without controls."));
                    return;
                }
                if (model.actions->set_device_name(
                        model.settings_device_name_draft)) {
                    model.settings_device_name_draft.clear();
                    model.settings_name_dirty = false;
                    set_feedback(model, tr("Device name updated"));
                } else {
                    set_feedback(model, tr("Device name save failed"));
                }
            }).build();
        // 输入行占位 + 用途说明（与密码区保持间距，不重叠）。
        row_y += metrics.control.field + metrics.spacing.tiny;
        components::text(ui, "aki.settings.name.hint")
            .text(tr("Rename this device. Other devices will see"
                     " the new name."))
            .position(pad_x, row_y)
            .fontSize(metrics.typography.caption)
            .wrap(true)
            .maxWidth(content_width)
            .color(semantic.text_subtle)
            .build();
        row_y += metrics.typography.caption + metrics.spacing.section;
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

    // ---- 中继服务器（M7/DEC-028；M8-01 决策 11 阶段 1 收拢）：注册/移除
    // 与运行态。未注册主视图 = 地址 + 注册按钮，租户/令牌/证书收拢进高级
    // 折叠区（默认收起，必填校验失败自动展开）。enrollment 变更重启生效
    // （HEY-20261006-001——文案如实披露）；连接事实经 RelayStatus 状态行
    // 展示（AppState 易失字段 → 快照派生）。URL/CA 为技术内容用 mono
    // （§2.1）；token 走 secureInput（DEC-018 凭据纪律）。----
    if (model.actions && model.actions->enroll_relay
        && model.actions->remove_relay) {
        row_y += metrics.control.field + metrics.spacing.section;
        components::text(ui, "aki.settings.relay.title")
            .text(tr("Relay server"))
            .position(pad_x, row_y)
            .fontSize(metrics.typography.body)
            .fontWeight(600)
            .color(tokens.text)
            .build();
        row_y += metrics.typography.body + metrics.spacing.compact;
        // 状态行（快照只读派生——compose 三不纪律）。
        const auto& relay = model.state_view.state.relay;
        std::string status_text;
        if (!relay.has_value() || !relay->enrolled) {
            status_text = tr("Relay: not enrolled");
        } else if (relay->connection_state_name == "ready") {
            status_text = tr("connected") + std::string(": ")
                + relay->relay_url;
        } else {
            status_text = tr("Enrolled. Restart Aki to connect.")
                + std::string(" (")
                + relay_connection_label(relay->connection_state_name)
                + std::string(")");
        }
        components::text(ui, "aki.settings.relay.status")
            .text(status_text)
            .position(pad_x, row_y)
            .fontSize(metrics.typography.caption)
            .fontFamily("Mono")
            .wrap(true)
            .maxWidth(content_width)
            .color(semantic.text_subtle)
            .build();
        if (relay.has_value() && !relay->last_error.empty()) {
            row_y += metrics.typography.caption + metrics.spacing.tiny;
            components::text(ui, "aki.settings.relay.error")
                .text(relay->last_error)
                .position(pad_x, row_y)
                .fontSize(metrics.typography.caption)
                .wrap(true)
                .maxWidth(content_width)
                .color(semantic.destructive)
                .build();
        }
        row_y += metrics.typography.caption + metrics.spacing.compact;

        if (!relay.has_value() || !relay->enrolled) {
            // 注册向导（M8-04/DEC-028 决策 11 阶段 2）：主视图 = 地址 +
            // 注册密码 + 注册按钮；租户/令牌/证书收拢进「高级中继设置」
            // 折叠区（默认收起）。令牌草稿非空时走 token 高级路径（折叠区
            // 必填校验失败自动展开）；否则走密码主路径（TOFU 首连，relay
            // 回传指纹自动锚定——文案如实披露首连窗口）。
            const float relay_url_width =
                std::min(300.0f, content_width);
            components::input(ui, "aki.settings.relay.url")
                .position(pad_x, row_y)
                .size(relay_url_width, metrics.control.field)
                .theme(tokens)
                .value(model.settings_relay_url_draft)
                .placeholder(tr("Relay address (wss://…)"))
                .onChange([&model](const std::string& value) {
                    model.settings_relay_url_draft = value;
                }).build();
            row_y += metrics.control.field + metrics.spacing.compact;
            const bool password_path_available =
                model.actions->enroll_relay_password.operator bool();
            if (password_path_available) {
                secureInput(ui, "aki.settings.relay.password",
                    model.settings_relay_password_draft, tokens, pad_x,
                    row_y, std::min(300.0f, content_width),
                    metrics.control.field, tr("Enrollment password"));
            }
            components::button(ui, "aki.settings.relay.enroll")
                .position(pad_x + std::min(300.0f, content_width)
                    + metrics.spacing.compact, row_y)
                .size(96.0f, metrics.control.field)
                .text(tr("Enroll"))
                .theme(tokens, true)
                .textColor(semantic.primary_foreground)
                .onClick([&model, password_path_available] {
                    const bool password_path = password_path_available
                        && model.settings_relay_token_draft.empty();
                    if (password_path) {
                        if (model.settings_relay_url_draft.empty()
                            || model.settings_relay_password_draft.empty()) {
                            set_feedback(model,
                                tr("Relay address and enrollment password"
                                   " are required."));
                            return;
                        }
                        std::string error;
                        const bool submitted =
                            model.actions->enroll_relay_password(
                                model.settings_relay_url_draft,
                                model.settings_relay_password_draft,
                                model.settings_relay_ca_draft, error);
                        aki::ui::clear_secret(
                            model.settings_relay_password_draft);
                        if (!submitted) {
                            set_feedback(model,
                                error.empty()
                                    ? tr("Relay enrollment request"
                                         " rejected")
                                    : std::move(error));
                            return;
                        }
                        set_feedback(model,
                            tr("Relay enrollment submitted; see status"
                               " below."));
                        return;
                    }
                    if (model.settings_relay_url_draft.empty()
                        || model.settings_relay_tenant_draft.empty()
                        || model.settings_relay_token_draft.empty()) {
                        // 必填项在折叠区内：先展开再反馈缺失，用户直接
                        // 落在待填字段上（§2.5 状态不仅靠颜色）。
                        model.settings_relay_advanced_open = true;
                        set_feedback(model,
                            tr("Relay address, tenant, and token are"
                               " required."));
                        return;
                    }
                    std::string error;
                    const bool submitted =
                        model.actions->enroll_relay(
                            model.settings_relay_url_draft,
                            model.settings_relay_tenant_draft,
                            model.settings_relay_token_draft,
                            model.settings_relay_ca_draft, error);
                    aki::ui::clear_secret(
                        model.settings_relay_token_draft);
                    if (!submitted) {
                        set_feedback(model,
                            error.empty()
                                ? tr("Relay enrollment request rejected")
                                : std::move(error));
                        return;
                    }
                    set_feedback(model,
                        tr("Relay enrollment submitted; see status"
                           " below."));
                }).build();
            row_y += metrics.control.field + metrics.spacing.tiny;
            if (password_path_available) {
                // TOFU 首连窗口披露（决策 11 阶段 2 条款，如实呈现）。
                components::text(ui, "aki.settings.relay.tofu")
                    .text(tr("Password enrollment trusts the relay"
                             " certificate on first connection (TOFU);"
                             " make sure the address and password come from"
                             " a trusted source."))
                    .position(pad_x, row_y)
                    .fontSize(metrics.typography.caption)
                    .wrap(true)
                    .maxWidth(content_width)
                    .color(semantic.text_subtle)
                    .build();
                row_y += metrics.typography.caption * 3.0f
                    + metrics.spacing.compact;
            }
            // §2.5 容忍翻译变长：英文标签较长，宽度给足（420 上限）。
            foldToggle(ui, "aki.settings.relay.advanced",
                model.settings_relay_advanced_open, tokens, pad_x, row_y,
                std::min(420.0f, content_width), metrics.control.menuItem,
                tr("Advanced relay settings (tenant / token /"
                   " certificate)"));
            row_y += metrics.control.menuItem + metrics.spacing.tiny;
            if (model.settings_relay_advanced_open) {
                // 高级区：租户 + 准入令牌同行（token 制部署路径），证书
                // 路径独占一行（token 链校验 / 密码模式严格部署共用）。
                const float tenant_width = std::min(120.0f, content_width);
                components::input(ui, "aki.settings.relay.tenant")
                    .position(pad_x, row_y)
                    .size(tenant_width, metrics.control.field)
                    .theme(tokens)
                    .value(model.settings_relay_tenant_draft)
                    .placeholder(tr("Tenant"))
                    .onChange([&model](const std::string& value) {
                        model.settings_relay_tenant_draft = value;
                    }).build();
                secureInput(ui, "aki.settings.relay.token",
                    model.settings_relay_token_draft, tokens,
                    pad_x + tenant_width + metrics.spacing.compact, row_y,
                    std::min(300.0f, std::max(60.0f, content_width
                        - tenant_width - metrics.spacing.compact)),
                    metrics.control.field, tr("Bootstrap token"));
                row_y += metrics.control.field + metrics.spacing.compact;
                components::input(ui, "aki.settings.relay.ca")
                    .position(pad_x, row_y)
                    .size(std::min(440.0f, content_width),
                        metrics.control.field)
                    .theme(tokens)
                    .value(model.settings_relay_ca_draft)
                    .placeholder(tr("Relay certificate file (optional)"))
                    .onChange([&model](const std::string& value) {
                        model.settings_relay_ca_draft = value;
                    }).build();
                row_y += metrics.control.field + metrics.spacing.tiny;
            }
            components::text(ui, "aki.settings.relay.hint")
                .text(tr("Devices discover each other through the relay"
                         " across networks.\nEnrollment changes take effect"
                         " after restart."))
                .position(pad_x, row_y)
                .fontSize(metrics.typography.caption)
                .wrap(true)
                .maxWidth(content_width)
                .color(semantic.text_subtle)
                .build();
            row_y += metrics.typography.caption * 3.0f
                + metrics.spacing.section;
        } else if (model.actions->remove_relay) {
            components::button(ui, "aki.settings.relay.remove")
                .position(pad_x, row_y)
                .size(std::min(220.0f, content_width),
                    metrics.control.field)
                .text(tr("Remove enrollment"))
                .theme(tokens, false)
                .onClick([&model] {
                    std::string error;
                    if (model.actions->remove_relay(error)) {
                        set_feedback(model,
                            tr("Relay enrollment removed. Restart Aki to"
                               " disconnect."));
                    } else {
                        set_feedback(model,
                            error.empty()
                                ? tr("Relay removal rejected")
                                : std::move(error));
                    }
                }).build();
            row_y += metrics.control.field + metrics.spacing.tiny;
            components::text(ui, "aki.settings.relay.removed.hint")
                .text(tr("Devices discover each other through the relay"
                         " across networks.\nEnrollment changes take effect"
                         " after restart."))
                .position(pad_x, row_y)
                .fontSize(metrics.typography.caption)
                .wrap(true)
                .maxWidth(content_width)
                .color(semantic.text_subtle)
                .build();
            row_y += metrics.typography.caption * 3.0f
                + metrics.spacing.section;
        }
    }

    // ---- TURN 服务器（高级，M7/DEC-028 决策 7）：跨网段直连失败时的
    // 数据面兜底（relay 不转发数据）；静态长期凭据；重启生效。----
    if (model.actions && model.actions->set_turn_server) {
        components::text(ui, "aki.settings.turn.title")
            .text(tr("TURN server (advanced)"))
            .position(pad_x, row_y)
            .fontSize(metrics.typography.body)
            .fontWeight(600)
            .color(tokens.text)
            .build();
        row_y += metrics.typography.body + metrics.spacing.compact;
        components::text(ui, "aki.settings.turn.hint")
            .text(tr("TURN carries data when direct connection fails"
                     " across networks.\nChanges take effect after"
                     " restart."))
            .position(pad_x, row_y)
            .fontSize(metrics.typography.caption)
            .wrap(true)
            .maxWidth(content_width)
            .color(semantic.text_subtle)
            .build();
        row_y += metrics.typography.caption * 3.0f + metrics.spacing.tiny;
        const float turn_host_width = std::min(220.0f, content_width);
        components::input(ui, "aki.settings.turn.host")
            .position(pad_x, row_y)
            .size(turn_host_width, metrics.control.field)
            .theme(tokens)
            .value(model.settings_turn_host_draft)
            .placeholder(tr("Host"))
            .onChange([&model](const std::string& value) {
                model.settings_turn_host_draft = value;
            }).build();
        const float turn_port_x =
            pad_x + turn_host_width + metrics.spacing.compact;
        const float turn_port_width =
            std::min(96.0f, std::max(60.0f, content_width
                - turn_host_width - metrics.spacing.compact));
        components::input(ui, "aki.settings.turn.port")
            .position(turn_port_x, row_y)
            .size(turn_port_width, metrics.control.field)
            .theme(tokens)
            .value(model.settings_turn_port_draft)
            .placeholder(tr("Port"))
            .onChange([&model](const std::string& value) {
                model.settings_turn_port_draft = value;
            }).build();
        row_y += metrics.control.field + metrics.spacing.compact;
        const float turn_user_width = std::min(160.0f, content_width);
        components::input(ui, "aki.settings.turn.user")
            .position(pad_x, row_y)
            .size(turn_user_width, metrics.control.field)
            .theme(tokens)
            .value(model.settings_turn_username_draft)
            .placeholder(tr("Username"))
            .onChange([&model](const std::string& value) {
                model.settings_turn_username_draft = value;
            }).build();
        secureInput(ui, "aki.settings.turn.credential",
            model.settings_turn_credential_draft, tokens,
            turn_user_width + metrics.spacing.compact, row_y,
            std::min(200.0f, std::max(60.0f, content_width
                - turn_user_width - metrics.spacing.compact)),
            metrics.control.field, tr("Credential"));
        components::button(ui, "aki.settings.turn.save")
            .position(pad_x + std::min(360.0f, content_width)
                + metrics.spacing.compact, row_y)
            .size(96.0f, metrics.control.field)
            .text(tr("Save"))
            .theme(tokens, false)
            .onClick([&model] {
                unsigned port = 0;
                try {
                    port = static_cast<unsigned>(
                        std::stoul(model.settings_turn_port_draft));
                } catch (const std::exception&) {
                    port = 0;
                }
                if (port == 0 || port > 65535) {
                    set_feedback(model, tr("Port must be 1-65535."));
                    return;
                }
                std::string error;
                const bool saved = model.actions->set_turn_server(
                    model.settings_turn_host_draft, port,
                    model.settings_turn_username_draft,
                    model.settings_turn_credential_draft, error);
                aki::ui::clear_secret(
                    model.settings_turn_credential_draft);
                if (!saved) {
                    set_feedback(model,
                        error.empty()
                            ? tr("TURN configuration rejected")
                            : std::move(error));
                    return;
                }
                set_feedback(model,
                    tr("TURN configuration saved. Restart Aki to"
                       " apply."));
            }).build();
        row_y += metrics.control.field + metrics.spacing.section;
    }

}

void composeSettingsPasswordDialog(eui::Ui& ui,
    const ThemeColorTokens& tokens, const AkiSemanticPalette& semantic,
    float screen_width, float screen_height, MainWindowModel& model) {
    if (model.page == NavPage::Settings && model.password_change_open
        && model.actions) {
        const auto& metrics = tokens.metrics;
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
