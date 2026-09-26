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
constexpr float kListColumnWidth = 264.0f;
constexpr float kColumnGap = 4.0f;  // §2.4：4px 可调间隙（静态值）
constexpr float kDeviceRowHeight = 64.0f;

struct NavEntry {
    NavPage page;
    const char* rail_label;  // 导航栏窄条短标签
};

constexpr std::array<NavEntry, 4> kNavEntries{{
    {NavPage::Conversations, "Chat"},
    {NavPage::Devices, "Devices"},
    {NavPage::Transfers, "Files"},
    {NavPage::Settings, "Setup"},
}};

// 列表栏/内容栏占位文案（M5-05~07 接入后由视图模型派生替换）。
const char* list_placeholder(NavPage page) {
    switch (page) {
    case NavPage::Conversations:
        return "conversation list placeholder (M5-05)";
    case NavPage::Devices:
        return "device rows in the content pane (M5-04)";
    case NavPage::Transfers:
        return "transfer list placeholder (M5-06)";
    case NavPage::Settings:
        return "settings entries placeholder (M5-07)";
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
    const float list_x = kNavRailWidth + kColumnGap;
    const float content_x = list_x + kListColumnWidth + kColumnGap;
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
        // 导航项：menuItem 高 28（§2.2 覆写档）；选中页以 primary（黑/白）
        // 对比承载（§2.4「Tabs 选中用对比而非品牌填充」同纪律）。
        float nav_y = metrics.spacing.panel;
        for (const NavEntry& entry : kNavEntries) {
            const bool selected = model.page == entry.page;
            components::button(ui, std::string("aki.nav.") + entry.rail_label)
                .position(metrics.spacing.compact, nav_y)
                .size(kNavRailWidth - metrics.spacing.compact * 2.0f,
                    metrics.control.menuItem)
                .text(entry.rail_label)
                .fontSize(metrics.typography.hint)
                .theme(tokens, selected)
                .radius(metrics.radius.small)
                .onClick([&model, page = entry.page] { model.page = page; })
                .build();
            nav_y += metrics.control.menuItem + metrics.spacing.compact;
        }

        // ---- 第二栏：列表栏（fixed）----
        ui.rect("aki.list.bg")
            .position(list_x, 0.0f)
            .size(kListColumnWidth, height)
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
        // M5-03 消费面展示（最小接线）：四域视图模型计数（派生自最近消费的
        // 快照；具体列表展示随各页实体化扩展）。
        const std::string list_counts =
            std::to_string(model.state_view.devices.size()) + " devices · "
            + std::to_string(model.state_view.conversations.size())
            + " conversations · "
            + std::to_string(model.state_view.transfers.size()) + " transfers";
        components::text(ui, "aki.list.counts")
            .text(list_counts)
            .position(list_x + metrics.spacing.content,
                metrics.spacing.content + metrics.typography.title
                    + metrics.spacing.tiny)
            .fontSize(metrics.typography.hint)
            .color(semantic.text_subtle)
            .build();
        ui.rect("aki.list.divider")
            .position(list_x + metrics.spacing.content,
                metrics.spacing.section + metrics.typography.title)
            .size(kListColumnWidth - metrics.spacing.content * 2.0f, 1.0f)
            .color(tokens.border)
            .build();
        components::text(ui, "aki.list.placeholder")
            .text(list_placeholder(model.page))
            .position(list_x + metrics.spacing.content,
                metrics.spacing.section + metrics.typography.title
                    + metrics.spacing.content)
            .fontSize(metrics.typography.body)
            .color(semantic.text_subtlest)
            .build();

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
            for (const models::DeviceView& device : model.state_view.devices) {
                const std::string row_id =
                    "aki.devices.row." + device.id.value;
                const bool odd = (&device - model.state_view.devices.data())
                    % 2 == 1;
                // 行底（奇偶分隔，surface 3% 叠加）。
                ui.rect(row_id + ".bg")
                    .position(content_x + metrics.spacing.section, row_y)
                    .size(row_width, kDeviceRowHeight)
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
                        row_y + kDeviceRowHeight
                            - metrics.spacing.compact
                            - metrics.typography.caption)
                    .fontSize(metrics.typography.caption)
                    .color(trust_color(semantic, device.trust_state))
                    .build();
                // 指纹列（mono；缺公钥显示显式不可用态——不以 id 冒充）。
                components::text(ui, row_id + ".fingerprint")
                    .text(device.fingerprint_available
                            ? device.id.value
                            : "(fingerprint unavailable)")
                    .position(content_x + metrics.spacing.section
                            + metrics.spacing.content * 3.0f + 220.0f,
                        row_y + metrics.spacing.compact + 2.0f)
                    .fontSize(metrics.typography.hint)
                    .fontFamily("Mono")
                    .color(device.fingerprint_available
                            ? semantic.text_subtle
                            : semantic.destructive)
                    .build();

                // 信任操作按钮（可用性按 §4 转移边派生：仅 Pending 可确认/
                // 拒绝、仅 Trusted 可撤销）。确认走弹窗（指纹核对）。
                const float button_x = content_x + metrics.spacing.section
                    + row_width - 2.0f * (132.0f + metrics.spacing.compact);
                const float button_y =
                    row_y + (kDeviceRowHeight - metrics.control.menuItem)
                        * 0.5f;
                if (device.can_confirm() && model.actions) {
                    components::button(ui, row_id + ".confirm")
                        .position(button_x, button_y)
                        .size(132.0f, metrics.control.menuItem)
                        .text("Confirm")
                        .fontSize(metrics.typography.caption)
                        .theme(tokens, true)
                        .radius(metrics.radius.small)
                        .onClick([&model, id = device.id.value] {
                            model.pending_confirm_device = id;
                        })
                        .build();
                    components::button(ui, row_id + ".reject")
                        .position(button_x + 132.0f + metrics.spacing.compact,
                            button_y)
                        .size(132.0f, metrics.control.menuItem)
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
                        .size(132.0f, metrics.control.menuItem)
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
                row_y += kDeviceRowHeight + metrics.spacing.compact;
                if (row_y > height - kDeviceRowHeight) {
                    break;  // 视口有界（滚动归 M5-06+ 通用列表形态）。
                }
            }
            if (model.state_view.devices.empty()) {
                components::text(ui, "aki.devices.empty")
                    .text("no devices yet — start discovery to find peers on"
                          " your LAN")
                    .position(content_x + metrics.spacing.section,
                        metrics.spacing.section + metrics.typography.title
                            + metrics.spacing.content)
                    .fontSize(metrics.typography.body)
                    .color(semantic.text_subtlest)
                    .build();
            }

            // 发现启停（M5-03 出站示范保留在页首）+ 发现来源分期披露
            //（M3-09：当前仅 LAN 来源产生发现；Relay/邀请链接/手动输入为
            // 登记补做条件）。
            const float action_y = height - metrics.control.field
                - metrics.spacing.section * 2.0f;
            if (model.actions) {
                components::button(ui, "aki.content.action.discover")
                    .position(content_x + metrics.spacing.section, action_y)
                    .size(160.0f, metrics.control.field)
                    .text("Start Discovery")
                    .fontSize(metrics.typography.body)
                    .theme(tokens, true)
                    .radius(metrics.radius.small)
                    .onClick([&model] {
                        const bool admitted = model.actions->start_discovery(
                            aki::device::DiscoveryMethod::LanDiscovery);
                        model.last_action_feedback =
                            admitted ? "start_discovery admitted"
                                     : "start_discovery rejected";
                    })
                    .build();
                components::button(ui, "aki.content.action.stop")
                    .position(content_x + metrics.spacing.section + 172.0f,
                        action_y)
                    .size(160.0f, metrics.control.field)
                    .text("Stop Discovery")
                    .fontSize(metrics.typography.body)
                    .theme(tokens, false)
                    .radius(metrics.radius.small)
                    .onClick([&model] {
                        const bool admitted = model.actions->stop_discovery();
                        model.last_action_feedback =
                            admitted ? "stop_discovery admitted"
                                     : "stop_discovery rejected";
                    })
                    .build();
            }
            components::text(ui, "aki.devices.source_note")
                .text("discovery source: LAN only (Relay / invite link /"
                      " manual input are registered follow-ups, M3-09)")
                .position(content_x + metrics.spacing.section + 350.0f,
                    action_y + metrics.spacing.content)
                .fontSize(metrics.typography.caption)
                .color(semantic.text_subtlest)
                .build();
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
                      " M5-05..07")
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
                        - metrics.spacing.section)
                .fontSize(metrics.typography.caption)
                .color(semantic.text_subtle)
                .build();
        }

        // ---- 信任确认弹窗（M5-04；aki_ui_design §3：mono 指纹确认，
        //      无口令输入框；页面持有 open 态）----
        const bool dialog_open = !model.pending_confirm_device.empty();
        if (dialog_open && model.actions) {
            components::dialog(ui, "aki.devices.confirm")
                .open(true)
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
                });
        }
    }).build();
}

}  // namespace aki::ui
