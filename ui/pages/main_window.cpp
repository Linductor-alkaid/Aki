// 三栏壳与四页导航（M5-04 Devices 页实体化；其余页 M5-03 消费面接线形态保持，
// 页面具体展示归 M5-05~07）。
//
// 组合纪律（§9.1）：compose 只读派生——页面 UI 态（当前页/主题档/操作反馈/
// 确认弹窗）与消费面状态、注入出站接口（UiActions）存于 MainWindowModel
// （页面模型持有，重组时读入）；点击经回调改写模型后由事件驱动的重组拾取
// （保留模式，M5-01 实测）。compose 内不等待、不轮询、不做 IO；业务状态
// 不在本层持有——Store 经快照只读（RULE-02），操作全经 UiActions（DEC-008）。
// 视觉语义色按 aki_ui_design §3（Online success/Offline subtlest、
// Pending warning、Trusted success、Rejected/Revoked destructive、路径
// caption 中性徽标）。
#include "ui/pages/main_window.hpp"

#include "components/button.h"
#include "components/dialog.h"
#include "components/text.h"

#include <algorithm>
#include <array>
#include <string>

namespace aki::ui {
namespace {

constexpr float kNavRailWidth = 64.0f;
constexpr float kMinListColumnWidth = 168.0f;
constexpr float kMaxListColumnWidth = 264.0f;
constexpr float kColumnGap = 4.0f;  // §2.4：4px 可调间隙（静态值）
constexpr float kDeviceRowHeight = 64.0f;

struct NavEntry {
    NavPage page;
    const char* rail_label;
    unsigned int icon;
};

constexpr std::array<NavEntry, 4> kNavEntries{{
    {NavPage::Conversations, "Chat", 0xF086},
    {NavPage::Devices, "Devices", 0xF6FF},
    {NavPage::Transfers, "Files", 0xF0EC},
    {NavPage::Settings, "Settings", 0xF013},
}};

const char* list_guidance(NavPage page) {
    switch (page) {
    case NavPage::Conversations:
        return "";
    case NavPage::Devices:
        return "Manage trusted peers.";
    case NavPage::Transfers:
        return "File activity.";
    case NavPage::Settings:
        return "Theme and local data.";
    }
    return "";
}

std::string list_summary(NavPage page, const MainWindowModel& model) {
    switch (page) {
    case NavPage::Conversations:
        return std::to_string(model.state_view.conversations.size()) + " chats";
    case NavPage::Devices:
        return std::to_string(model.state_view.devices.size()) + " known devices";
    case NavPage::Transfers:
        return std::to_string(model.state_view.transfers.size()) + " transfers";
    case NavPage::Settings:
        return "Preferences";
    }
    return "";
}

std::string device_class_label(aki::device::DeviceClass device_class) {
    using aki::device::DeviceClass;
    switch (device_class) {
    case DeviceClass::Desktop: return "Desktop";
    case DeviceClass::Tablet: return "Tablet";
    case DeviceClass::Phone: return "Phone";
    case DeviceClass::Server: return "Server";
    case DeviceClass::Robot: return "Robot";
    case DeviceClass::Other: return "Device";
    }
    return "Device";
}

std::string trust_label(aki::device::TrustState trust_state) {
    using aki::device::TrustState;
    switch (trust_state) {
    case TrustState::Unknown: return "Unknown";
    case TrustState::Pending: return "Pending";
    case TrustState::Trusted: return "Trusted";
    case TrustState::Rejected: return "Rejected";
    case TrustState::Revoked: return "Revoked";
    }
    return "Unknown";
}

core::Color trust_color(const AkiSemanticPalette& semantic,
    aki::device::TrustState trust_state) {
    using aki::device::TrustState;
    switch (trust_state) {
    case TrustState::Pending: return semantic.warning;      // §3 warning
    case TrustState::Trusted: return semantic.success;      // §3 success
    case TrustState::Rejected:
    case TrustState::Revoked: return semantic.destructive;  // §3 destructive
    case TrustState::Unknown: break;
    }
    return semantic.text_subtlest;                          // §3 subtlest
}

// 徽标文本（路径 caption 中性：Unknown 显示 "—"，§3）。
std::string path_label(aki::device::ConnectionPath connection_path) {
    const auto text = aki::device::to_string(connection_path);
    if (connection_path == aki::device::ConnectionPath::Unknown) {
        return "--";
    }
    return std::string(text);
}

}  // namespace

const char* navPageTitle(NavPage page) {
    switch (page) {
    case NavPage::Conversations:
        return "Conversations";
    case NavPage::Devices:
        return "Devices";
    case NavPage::Transfers:
        return "Transfers";
    case NavPage::Settings:
        return "Settings";
    }
    return "";
}

void composeMainWindow(eui::Ui& ui, const eui::Screen& screen,
    MainWindowModel& model) {
    const auto tokens = akiTheme(model.theme);
    const auto semantic = akiSemanticColors(model.theme);
    const auto& metrics = tokens.metrics;

    const float width = std::max(screen.width, 1.0f);
    const float height = std::max(screen.height, 1.0f);
    const float list_column_width = std::clamp(width * 0.31f,
        kMinListColumnWidth, kMaxListColumnWidth);
    const float list_x = kNavRailWidth + kColumnGap;
    const float content_x = list_x + list_column_width + kColumnGap;
    const float content_width = std::max(width - content_x, 1.0f);

    ui.stack("aki.shell").size(width, height).content([&] {
        // 窗底：页面背景（§2.3：brand 克制，永不做整面背景）。
        ui.rect("aki.shell.bg").size(width, height).color(tokens.background).build();

        // ---- 第一栏：导航栏（fixed）----
        ui.rect("aki.nav.bg")
            .position(0.0f, 0.0f)
            .size(kNavRailWidth, height)
            .color(tokens.surface)
            .build();
        // 图标与文字纵排，保留窄栏的完整文字标签；选中页以 primary 对比承载。
        float nav_y = metrics.spacing.panel;
        for (const NavEntry& entry : kNavEntries) {
            const bool selected = model.page == entry.page;
            const std::string id = std::string("aki.nav.") + entry.rail_label;
            const float item_width = kNavRailWidth - metrics.spacing.compact * 2.0f;
            const float item_height = metrics.control.menuItem * 2.0f;
            ui.rect(id)
                .position(metrics.spacing.compact, nav_y)
                .size(item_width, item_height)
                .states(selected ? tokens.primary : tokens.surface,
                    selected ? tokens.primary : tokens.surfaceHover,
                    selected ? tokens.primary : tokens.surfaceActive)
                .radius(metrics.radius.small)
                .onClick([&model, page = entry.page] {
                    model.page = page;
                    model.last_action_feedback.clear();
                })
                .build();
            const auto label_color = selected
                ? semantic.primary_foreground : tokens.text;
            components::text(ui, id + ".icon")
                .icon(entry.icon)
                .position(metrics.spacing.compact, nav_y + metrics.spacing.compact)
                .size(item_width, metrics.typography.subtitle)
                .fontSize(metrics.typography.subtitle)
                .horizontalAlign(core::HorizontalAlign::Center)
                .color(label_color)
                .build();
            components::text(ui, id + ".label")
                .text(entry.rail_label)
                .position(metrics.spacing.compact,
                    nav_y + metrics.control.menuItem + metrics.spacing.tiny)
                .size(item_width, metrics.typography.hint)
                .fontSize(metrics.typography.hint)
                .horizontalAlign(core::HorizontalAlign::Center)
                .color(label_color)
                .build();
            nav_y += item_height + metrics.spacing.compact;
        }

        // ---- 第二栏：列表栏（fixed）----
        ui.rect("aki.list.bg")
            .position(list_x, 0.0f)
            .size(list_column_width, height)
            .color(tokens.surface)
            .build();
        components::text(ui, "aki.list.title")
            .text(navPageTitle(model.page))
            .position(list_x + metrics.spacing.content,
                metrics.spacing.content)
            .fontSize(metrics.typography.title)
            .fontWeight(600)
            .color(tokens.text)
            .build();
        const float list_summary_y = metrics.spacing.content
            + metrics.typography.title + metrics.spacing.tiny;
        const float list_divider_y = list_summary_y
            + metrics.typography.hint + metrics.spacing.content;
        const float list_body_y = list_divider_y + metrics.spacing.content;
        components::text(ui, "aki.list.counts")
            .text(list_summary(model.page, model))
            .position(list_x + metrics.spacing.content, list_summary_y)
            .fontSize(metrics.typography.hint)
            .color(semantic.text_subtle)
            .build();
        ui.rect("aki.list.divider")
            .position(list_x + metrics.spacing.content, list_divider_y)
            .size(list_column_width - metrics.spacing.content * 2.0f, 1.0f)
            .color(tokens.border)
            .build();
        if (model.page == NavPage::Conversations) {
            // ---- Conversations 页列表（M5-05，SCOPE-05）----
            composeConversationList(ui, tokens, semantic,
                list_x + metrics.spacing.content,
                list_body_y,
                list_column_width - metrics.spacing.content * 2.0f,
                height - list_body_y,
                width, height, model);
        } else {
            components::text(ui, "aki.list.placeholder")
                .text(list_guidance(model.page))
                .position(list_x + metrics.spacing.content, list_body_y)
                .fontSize(metrics.typography.caption)
                .wrap(true)
                .maxWidth(list_column_width - metrics.spacing.content * 2.0f)
                .color(tokens.text)
                .build();
        }

        // ---- 第三栏：内容栏（fill）----
        ui.rect("aki.content.bg")
            .position(content_x, 0.0f)
            .size(content_width, height)
            .color(tokens.background)
            .build();
        if (!model.startup_error.empty()) {
            // 装配失败降级占位（§9.1：错误占位 UI + 关窗仍经 onShutdown 闭合）。
            components::text(ui, "aki.content.error.title")
                .text("startup failed")
                .position(content_x + metrics.spacing.section,
                    metrics.spacing.section)
                .fontSize(metrics.typography.subtitle)
                .fontWeight(600)
                .color(semantic.destructive)
                .build();
            components::text(ui, "aki.content.error.detail")
                .text(model.startup_error)
                .position(content_x + metrics.spacing.section,
                    metrics.spacing.section + metrics.typography.subtitle
                        + metrics.spacing.compact)
                .fontSize(metrics.typography.body)
                .wrap(true)
                .maxWidth(content_width - metrics.spacing.section * 2.0f)
                .color(tokens.text)
                .build();
        } else if (model.page == NavPage::Devices) {
            // ---- Devices 页（M5-04，SCOPE-04/02/03/10 展示面）----
            components::text(ui, "aki.content.placeholder")
                .text(navPageTitle(model.page))
                .position(content_x + metrics.spacing.section,
                    metrics.spacing.section)
                .fontSize(metrics.typography.title)
                .fontWeight(600)
                .color(tokens.text)
                .build();

            // 设备行（绝对定位循环；设备预算 256，RULE-09——virtualList
            // 固定行高模型在 M5-05 消息列评估，此处行数有界不引入）。
            float row_y = metrics.spacing.section + metrics.typography.title
                + metrics.spacing.content;
            const float row_width =
                content_width - metrics.spacing.section * 2.0f;
            const bool compact_rows = row_width < 520.0f;
            const float action_y = height - metrics.control.field
                - metrics.spacing.section * 2.0f;
            for (const models::DeviceView& device : model.state_view.devices) {
                const std::string row_id =
                    "aki.devices.row." + device.id.value;
                const float row_height = compact_rows
                    ? ((device.can_confirm() || device.can_revoke()) ? 112.0f : 80.0f)
                    : kDeviceRowHeight;
                if (row_y + row_height > action_y
                        - metrics.typography.caption * 3.0f
                        - metrics.spacing.content * 2.0f) {
                    break;
                }
                const bool odd = (&device - model.state_view.devices.data())
                    % 2 == 1;
                // 行底（奇偶分隔，surface 3% 叠加）。
                ui.rect(row_id + ".bg")
                    .position(content_x + metrics.spacing.section, row_y)
                    .size(row_width, row_height)
                    .color(odd ? semantic.surface_overlay
                               : semantic.card)
                    .radius(metrics.radius.small)
                    .build();
                // presence 圆点（§3：Online 实心 success / Offline 空心
                // subtlest）。
                ui.rect(row_id + ".presence")
                    .position(content_x + metrics.spacing.section
                            + metrics.spacing.content,
                        row_y + metrics.spacing.content)
                    .size(10.0f, 10.0f)
                    .radius(metrics.radius.full)
                    .color(device.presence
                            == aki::device::PresenceState::Online
                        ? semantic.success
                        : semantic.text_subtlest)
                    .border(1.0f,
                        device.presence
                                == aki::device::PresenceState::Online
                            ? semantic.success
                            : semantic.text_subtlest)
                    .build();
                // 名称 + 类型/OS 摘要。
                components::text(ui, row_id + ".name")
                    .text(device.display_name)
                    .position(content_x + metrics.spacing.section
                            + metrics.spacing.content * 3.0f,
                        row_y + metrics.spacing.compact)
                    .fontSize(metrics.typography.body)
                    .fontWeight(600)
                    .color(tokens.text)
                    .build();
                components::text(ui, row_id + ".meta")
                    .text(device_class_label(device.device_class) + " · "
                        + device.os_name + " · "
                        + path_label(device.connection_path))
                    .position(content_x + metrics.spacing.section
                            + metrics.spacing.content * 3.0f,
                        row_y + metrics.spacing.compact
                            + metrics.typography.body + 2.0f)
                    .fontSize(metrics.typography.caption)
                    .color(semantic.text_subtle)
                    .build();
                // 信任徽标（§3 语义色）。
                components::text(ui, row_id + ".trust")
                    .text(trust_label(device.trust_state))
                    .position(content_x + metrics.spacing.section
                            + metrics.spacing.content * 3.0f,
                        compact_rows
                            ? row_y + metrics.spacing.compact
                                + metrics.typography.body
                                + metrics.typography.caption + metrics.spacing.tiny
                            : row_y + row_height - metrics.spacing.compact
                                - metrics.typography.caption)
                    .fontSize(metrics.typography.caption)
                    .color(trust_color(semantic, device.trust_state))
                    .build();
                // 指纹列（mono；缺公钥显示显式不可用态——不以 id 冒充）。
                const std::string fingerprint = device.fingerprint_available
                    ? device.id.value : "Fingerprint unavailable";
                const std::string compact_fingerprint =
                    fingerprint.size() > 28
                        ? fingerprint.substr(0, 16) + "..."
                            + fingerprint.substr(fingerprint.size() - 8)
                        : fingerprint;
                components::text(ui, row_id + ".fingerprint")
                    .text(compact_rows ? compact_fingerprint : fingerprint)
                    .position(content_x + metrics.spacing.section
                            + metrics.spacing.content * 3.0f
                            + (compact_rows ? 0.0f : 220.0f),
                        compact_rows ? row_y + row_height
                                - metrics.spacing.compact
                                - metrics.typography.hint
                                - (device.can_confirm() || device.can_revoke()
                                       ? metrics.control.menuItem
                                           + metrics.spacing.compact
                                           + metrics.spacing.tiny
                                       : 0.0f)
                            : row_y + metrics.spacing.compact + 2.0f)
                    .fontSize(metrics.typography.hint)
                    .fontFamily("Mono")
                    .maxWidth(compact_rows ? row_width - 48.0f
                                           : row_width - 268.0f)
                    .color(device.fingerprint_available
                            ? semantic.text_subtle
                            : semantic.destructive)
                    .build();

                // 信任操作按钮（可用性按 §4 转移边派生：仅 Pending 可确认/
                // 拒绝、仅 Trusted 可撤销）。确认走弹窗（指纹核对）。
                const float action_button_width = compact_rows
                    ? (row_width - metrics.spacing.compact * 3.0f) * 0.5f
                    : 132.0f;
                const float button_x = compact_rows
                    ? content_x + metrics.spacing.section
                        + metrics.spacing.compact
                    : content_x + metrics.spacing.section + row_width
                        - 2.0f * (132.0f + metrics.spacing.compact);
                const float button_y = compact_rows
                    ? row_y + row_height - metrics.control.menuItem
                        - metrics.spacing.compact
                    : row_y + (row_height - metrics.control.menuItem) * 0.5f;
                if (device.can_confirm() && model.actions) {
                    components::button(ui, row_id + ".confirm")
                        .position(button_x, button_y)
                        .size(action_button_width, metrics.control.menuItem)
                        .text("Confirm")
                        .fontSize(metrics.typography.caption)
                        .theme(tokens, true)
                        .textColor(semantic.primary_foreground)
                        .radius(metrics.radius.small)
                        .onClick([&model, id = device.id.value] {
                            model.pending_confirm_device = id;
                        })
                        .build();
                    components::button(ui, row_id + ".reject")
                        .position(button_x + action_button_width
                                + metrics.spacing.compact,
                            button_y)
                        .size(action_button_width, metrics.control.menuItem)
                        .text("Reject")
                        .fontSize(metrics.typography.caption)
                        .theme(tokens, false)
                        .radius(metrics.radius.small)
                        .onClick([&model, id = device.id.value] {
                            const bool admitted =
                                model.actions->reject_device(
                                    aki::device::DeviceId{id});
                            model.last_action_feedback =
                                admitted ? "reject " + id + " admitted"
                                         : "reject " + id + " rejected";
                        })
                        .build();
                }
                if (device.can_revoke() && model.actions) {
                    components::button(ui, row_id + ".revoke")
                        .position(button_x, button_y)
                        .size(action_button_width, metrics.control.menuItem)
                        .text("Revoke")
                        .fontSize(metrics.typography.caption)
                        .theme(tokens, false)
                        .radius(metrics.radius.small)
                        .onClick([&model, id = device.id.value] {
                            const bool admitted =
                                model.actions->revoke_device(
                                    aki::device::DeviceId{id});
                            model.last_action_feedback =
                                admitted ? "revoke " + id + " admitted"
                                         : "revoke " + id + " rejected";
                        })
                        .build();
                }
                row_y += row_height + metrics.spacing.compact;
            }
            if (model.state_view.devices.empty()) {
                components::text(ui, "aki.devices.empty")
                    .text("No devices yet. Start discovery to find nearby peers.")
                    .position(content_x + metrics.spacing.section,
                        metrics.spacing.section + metrics.typography.title
                            + metrics.spacing.content)
                    .fontSize(metrics.typography.body)
                    .wrap(true)
                    .maxWidth(row_width)
                    .color(tokens.text)
                    .build();
            }

            // 发现启停（M5-03 出站示范保留在页首）+ 发现来源分期披露
            //（M3-09：当前仅 LAN 来源产生发现；Relay/邀请链接/手动输入为
            // 登记补做条件）。
            const float discovery_button_width =
                (content_width - metrics.spacing.section * 3.0f) * 0.5f;
            if (model.actions) {
                components::button(ui, "aki.content.action.discover")
                    .position(content_x + metrics.spacing.section, action_y)
                    .size(discovery_button_width, metrics.control.field)
                    .text("Start scan")
                    .fontSize(metrics.typography.caption)
                    .theme(tokens, true)
                    .textColor(semantic.primary_foreground)
                    .radius(metrics.radius.small)
                    .onClick([&model] {
                        const bool admitted = model.actions->start_discovery(
                            aki::device::DiscoveryMethod::LanDiscovery);
                        model.last_action_feedback = admitted
                            ? "Discovery request submitted"
                            : "Discovery request rejected";
                    })
                    .build();
                components::button(ui, "aki.content.action.stop")
                    .position(content_x + metrics.spacing.section * 2.0f
                            + discovery_button_width, action_y)
                    .size(discovery_button_width, metrics.control.field)
                    .text("Stop scan")
                    .fontSize(metrics.typography.caption)
                    .theme(tokens, false)
                    .radius(metrics.radius.small)
                    .onClick([&model] {
                        const bool admitted = model.actions->stop_discovery();
                        model.last_action_feedback = admitted
                            ? "Stop request submitted"
                            : "Stop request rejected";
                    })
                    .build();
            }
            components::text(ui, "aki.devices.source_note")
                .text("Find devices on your local network.")
                .position(content_x + metrics.spacing.section,
                    action_y - metrics.typography.caption * 3.0f
                        - metrics.spacing.content)
                .fontSize(metrics.typography.caption)
                .wrap(true)
                .maxWidth(row_width)
                .color(semantic.text_subtle)
                .build();
        } else if (model.page == NavPage::Conversations) {
            // ---- 聊天窗口（M5-05，SCOPE-06/07 UI 面 + SCOPE-08 会话内
            //      文件卡片）----
            composeChatWindow(ui, tokens, semantic, content_x, 0.0f,
                content_width, height, width, height, model);
        } else if (model.page == NavPage::Transfers) {
            // ---- Transfers 页（M5-06，SCOPE-08 集中列表与操作面）----
            composeTransfersPage(ui, tokens, semantic, content_x, 0.0f,
                content_width, height, model);
        } else if (model.page == NavPage::Settings) {
            // ---- Settings 页（M5-07，SCOPE-12 主题三选 + 最小设置项）----
            composeSettingsPage(ui, tokens, semantic, content_x, 0.0f,
                content_width, height, model);
        } else {
            components::text(ui, "aki.content.placeholder")
                .text(navPageTitle(model.page))
                .position(content_x + metrics.spacing.section,
                    metrics.spacing.section)
                .fontSize(metrics.typography.title)
                .fontWeight(600)
                .color(tokens.text)
                .build();
            components::text(ui, "aki.content.placeholder.hint")
                .text("state consumption wired (M5-03); page views land in"
                      " M5-06..07")
                .position(content_x + metrics.spacing.section,
                    metrics.spacing.section + metrics.typography.title
                        + metrics.spacing.tiny)
                .fontSize(metrics.typography.hint)
                .color(semantic.text_subtlest)
                .build();
        }

        // 操作反馈行（页尾；跨页共享页面模型字段）。
        if (!model.last_action_feedback.empty()) {
            components::text(ui, "aki.content.action.feedback")
                .text(model.last_action_feedback)
                .position(content_x + metrics.spacing.section,
                    height - metrics.typography.caption
                        - metrics.spacing.compact)
                .fontSize(metrics.typography.caption)
                .maxWidth(content_width - metrics.spacing.section * 2.0f)
                .color(semantic.text_subtle)
                .build();
        }

        // ---- 信任确认弹窗（M5-04；aki_ui_design §3：mono 指纹确认，
        //      无口令输入框；页面持有 open 态）----
        const bool dialog_open = !model.pending_confirm_device.empty();
        if (dialog_open && model.actions) {
            components::dialog(ui, "aki.devices.confirm")
                .open(true)
                .theme(tokens)
                .screen(width, height)
                .size(560.0f, 260.0f)
                .content([&] {
                    components::text(ui, "aki.devices.confirm.title")
                        .text("Confirm pairing")
                        .position(metrics.spacing.section,
                            metrics.spacing.section)
                        .fontSize(metrics.typography.subtitle)
                        .fontWeight(600)
                        .color(tokens.text)
                        .build();
                    components::text(ui, "aki.devices.confirm.hint")
                        .text("verify the fingerprint matches the one shown"
                              " on the peer device")
                        .position(metrics.spacing.section,
                            metrics.spacing.section + metrics.typography
                                .subtitle
                            + metrics.spacing.compact)
                        .fontSize(metrics.typography.caption)
                        .color(semantic.text_subtle)
                        .build();
                    components::text(ui, "aki.devices.confirm.fingerprint")
                        .text(model.pending_confirm_device)
                        .position(metrics.spacing.section,
                            metrics.spacing.section + metrics.typography
                                .subtitle
                            + metrics.typography.caption
                            + metrics.spacing.section)
                        .fontSize(metrics.typography.caption)
                        .fontFamily("Mono")
                        .color(tokens.text)
                        .build();
                    components::button(ui, "aki.devices.confirm.yes")
                        .position(metrics.spacing.section,
                            260.0f - metrics.control.field
                                - metrics.spacing.section)
                        .size(180.0f, metrics.control.field)
                        .text("Confirm pairing")
                        .fontSize(metrics.typography.caption)
                        .theme(tokens, true)
                        .textColor(semantic.primary_foreground)
                        .radius(metrics.radius.small)
                        .onClick([&model] {
                            const bool admitted =
                                model.actions->confirm_pairing(
                                    aki::device::DeviceId{
                                        model.pending_confirm_device});
                            model.last_action_feedback =
                                admitted ? "pairing submitted for "
                                       + model.pending_confirm_device
                                         : "pairing submit rejected";
                            model.pending_confirm_device.clear();
                        })
                        .build();
                    components::button(ui, "aki.devices.confirm.no")
                        .position(560.0f - metrics.spacing.section - 140.0f,
                            260.0f - metrics.control.field
                                - metrics.spacing.section)
                        .size(140.0f, metrics.control.field)
                        .text("Cancel")
                        .fontSize(metrics.typography.caption)
                        .theme(tokens, false)
                        .radius(metrics.radius.small)
                        .onClick([&model] {
                            model.pending_confirm_device.clear();
                        })
                        .build();
                })
                // BUG-20260927-001 修复：原链缺尾部 .build()（DialogBuilder 仅在 build() 中创建元素——缺失时弹窗根本不进入 UI 树，点击 Confirm 无任何效果）；同批补 .screen/（背板覆盖窗口，原默认 800x600）与 .theme（跟随浅/深档），并补 onOpenChange（背板点击关闭回写页面持有 open 态；无 Escape 路径——pinned dialog 仅背板 onClick 接 requestClose）。
                .onOpenChange([&model](bool open) {
                    if (!open) {
                        model.pending_confirm_device.clear();
                    }
                })
                .build();
        }
    }).build();
}

}  // namespace aki::ui
