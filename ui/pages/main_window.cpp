// 三栏壳与四页导航实现（M5-02 骨架；布局为 §2.4 workspace 的静态简化形态：
// 导航栏 fixed 64 + 列表栏 fixed 264 + 内容栏 fill，栏间 4px 间隙）。
#include "ui/pages/main_window.hpp"

// 目标式 include（非 components.h 伞头）：第一方 -Werror 门禁下控制 EUI-NEO
// 头文件暴露面（伞头携带 markdown/workshop 等未消费组件的 /W4 告警，
// DEC-005「第三方不继承告警级别」在 MSVC 无 /external:I 落点时的处置）。
#include "components/button.h"
#include "components/text.h"

#include <algorithm>
#include <array>

namespace aki::ui {
namespace {

constexpr float kNavRailWidth = 64.0f;
constexpr float kListColumnWidth = 264.0f;
constexpr float kColumnGap = 4.0f;  // §2.4：4px 可调间隙（M5-02 静态值）

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

// 列表栏/内容栏占位文案（M5-03+ 状态消费面接入后由视图模型派生替换）。
const char* list_placeholder(NavPage page) {
    switch (page) {
    case NavPage::Conversations:
        return "conversation list placeholder (M5-05)";
    case NavPage::Devices:
        return "device list placeholder (M5-04)";
    case NavPage::Transfers:
        return "transfer list placeholder (M5-06)";
    case NavPage::Settings:
        return "settings entries placeholder (M5-07)";
    }
    return "";
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
        // 快照；具体列表展示归 M5-04~07）。
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
                      " M5-04..07")
                .position(content_x + metrics.spacing.section,
                    metrics.spacing.section + metrics.typography.title
                        + metrics.spacing.tiny)
                .fontSize(metrics.typography.hint)
                .color(semantic.text_subtlest)
                .build();
            if (model.page == NavPage::Devices && model.actions) {
                // M5-03 出站面示范（设计 §9.1「操作一律经 Application 出站
                // 面」）：发现启停经注入 UiActions → DeviceManager 泵；
                // admission 结果写页面模型反馈（RULE-09 拒绝可见）。信任
                // 操作面归 M5-04。
                const float action_y = metrics.spacing.section
                    + metrics.typography.title + metrics.spacing.content * 2.0f;
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
                                     : "start_discovery rejected (pump inbox"
                                       " full)";
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
                                     : "stop_discovery rejected (pump inbox"
                                       " full)";
                    })
                    .build();
            }
            if (!model.last_action_feedback.empty()) {
                components::text(ui, "aki.content.action.feedback")
                    .text(model.last_action_feedback)
                    .position(content_x + metrics.spacing.section,
                        metrics.spacing.section + metrics.typography.title
                            + metrics.spacing.content * 2.0f
                            + metrics.control.field + metrics.spacing.content)
                    .fontSize(metrics.typography.caption)
                    .color(semantic.text_subtle)
                    .build();
            }
        }
    }).build();
}

}  // namespace aki::ui
