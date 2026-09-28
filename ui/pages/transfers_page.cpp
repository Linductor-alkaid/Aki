// Transfers 页实现（语义见 transfers_page.hpp；aki_ui_design §3 传输态
// 视觉语义 / §4 组件映射：文件卡片 card+progress+button 共享形态
// （ui/components/transfer_card）+ 操作按钮；§9.1 compose 三不纪律——
// 只读派生、无等待/轮询/IO）。
#include "ui/pages/transfers_page.hpp"

#include "ui/components/transfer_card.hpp"
#include "ui/pages/main_window.hpp"

#include "components/button.h"
#include "components/scrollview.h"
#include "components/text.h"

#include <algorithm>
#include <string>
#include <utility>

namespace aki::ui {
namespace {

using components::theme::ThemeColorTokens;

constexpr float kTransferRowHeight = 88.0f;
constexpr float kActionButtonWidth = 96.0f;

// 快照只读 join（设备预算 256，线性查找在预算内，RULE-09）。
std::string peer_label(const MainWindowModel& model,
    const models::TransferView& transfer) {
    for (const models::DeviceView& device : model.state_view.devices) {
        if (device.id == transfer.peer) {
            return device.display_name;
        }
    }
    return transfer.peer.value;
}

void set_feedback(MainWindowModel& model, std::string text) {
    model.last_action_feedback = std::move(text);
}

// 单行（固定高；文件卡片本体居左、操作按钮居右——按钮集按状态门控，
// RULE-09 admission 拒绝经共享反馈行可见）。
void compose_transfer_row(eui::Ui& ui, const ThemeColorTokens& tokens,
    const AkiSemanticPalette& semantic, const std::string& id,
    const models::TransferView& transfer, float width, float row_height,
    MainWindowModel& model) {
    const auto& metrics = tokens.metrics;
    const float pad = metrics.spacing.content;
    const float button_column = kActionButtonWidth + metrics.spacing.content;
    const float body_width = std::max(
        width - pad * 2.0f - button_column - 170.0f, 160.0f);

    ui.stack(id)
        .size(width, row_height)
        .content([&] {
            // 行底（卡片面：§4 一级容器语义——中性 card + 边框，无选中态）。
            ui.rect(id + ".bg")
                .size(width, row_height)
                .color(semantic.card)
                .radius(metrics.radius.small)
                .border(1.0f, tokens.border)
                .build();

            // 文件卡片本体（共享形态，§4 复用契约；wrap 高由子元素外延）。
            ui.stack(id + ".card.wrap")
                .position(pad, pad)
                .width(body_width)
                .wrapContent()
                .content([&] {
                    widgets::TransferCardData data;
                    data.outbound = transfer.outbound;
                    data.file_name = transfer.file_name;
                    data.mime_type = transfer.mime_type;
                    data.size_bytes = transfer.total;
                    data.tracked = true;  // 行本体即传输行。
                    data.state = transfer.state;
                    data.progress = transfer.progress;
                    widgets::compose_transfer_card_body(ui, tokens, semantic,
                        id + ".card", data, body_width);
                })
                .build();

            // 方向 + 对端（行底 caption 中性）。
            components::text(ui, id + ".peer")
                .text(std::string(transfer.outbound ? "to " : "from ")
                    + peer_label(model, transfer))
                .position(pad,
                    row_height - metrics.spacing.compact
                        - metrics.typography.micro)
                .fontSize(metrics.typography.micro)
                .color(semantic.text_subtlest)
                .build();

            // 操作面（状态门控 = TransferView 派生；返回值即 admission，
            // 拒绝经反馈行可见；Paused 行 Cancel = DEC-013⑥ 无会话行
            // 直接终态入口的 UI 触达）。纵排右上角，随可用集收缩。
            const float button_x = width - pad - kActionButtonWidth;
            float button_y = pad;
            if (transfer.can_pause()) {
                components::button(ui, id + ".action.pause")
                    .position(button_x, button_y)
                    .size(kActionButtonWidth, metrics.control.menuItem)
                    .text("Pause")
                    .fontSize(metrics.typography.caption)
                    .theme(tokens, false)
                    .radius(metrics.radius.small)
                    .onClick([&model, tid = transfer.id] {
                        const bool admitted =
                            model.actions->pause_transfer(tid);
                        set_feedback(model,
                            admitted ? "pause admitted (" + tid.value + ")"
                                     : "pause rejected (inbox admission)");
                    })
                    .build();
                button_y += metrics.control.menuItem + metrics.spacing.tiny;
            }
            if (transfer.can_resume()) {
                components::button(ui, id + ".action.resume")
                    .position(button_x, button_y)
                    .size(kActionButtonWidth, metrics.control.menuItem)
                    .text("Resume")
                    .fontSize(metrics.typography.caption)
                    .theme(tokens, false)
                    .radius(metrics.radius.small)
                    .onClick([&model, tid = transfer.id] {
                        const bool admitted =
                            model.actions->resume_transfer(tid);
                        set_feedback(model,
                            admitted ? "resume admitted (" + tid.value + ")"
                                     : "resume rejected (inbox admission)");
                    })
                    .build();
                button_y += metrics.control.menuItem + metrics.spacing.tiny;
            }
            if (transfer.can_cancel()) {
                components::button(ui, id + ".action.cancel")
                    .position(button_x, button_y)
                    .size(kActionButtonWidth, metrics.control.menuItem)
                    .text("Cancel")
                    .fontSize(metrics.typography.caption)
                    .theme(tokens, true)
                    .textColor(semantic.primary_foreground)
                    .radius(metrics.radius.small)
                    .onClick([&model, tid = transfer.id] {
                        const bool admitted =
                            model.actions->cancel_transfer(tid);
                        set_feedback(model,
                            admitted ? "cancel admitted (" + tid.value + ")"
                                     : "cancel rejected (inbox admission)");
                    })
                    .build();
            }
        })
        .build();
}

}  // namespace

void composeTransfersPage(eui::Ui& ui, const ThemeColorTokens& tokens,
    const AkiSemanticPalette& semantic, float x, float y, float width,
    float height, MainWindowModel& model) {
    const auto& metrics = tokens.metrics;

    components::text(ui, "aki.transfers.title")
        .text("Transfers")
        .position(x + metrics.spacing.section, y + metrics.spacing.section)
        .fontSize(metrics.typography.title)
        .fontWeight(600)
        .color(tokens.text)
        .build();
    const std::string page_summary =
        std::to_string(model.state_view.transfers.size()) + " transfers";
    components::text(ui, "aki.transfers.summary")
        .text(page_summary)
        .position(x + metrics.spacing.section,
            y + metrics.spacing.section + metrics.typography.title
                + metrics.spacing.tiny)
        .fontSize(metrics.typography.caption)
        .color(semantic.text_subtle)
        .build();

    if (model.state_view.transfers.empty()) {
        components::text(ui, "aki.transfers.empty")
            .text("No transfers yet. Send a file from a conversation to track it here.")
            .position(x + metrics.spacing.section,
                y + metrics.spacing.section + metrics.typography.title
                    + metrics.typography.caption + metrics.spacing.content)
            .fontSize(metrics.typography.body)
            .wrap(true)
            .maxWidth(width - metrics.spacing.section * 2.0f)
            .color(tokens.text)
            .build();
    } else {
        const float list_y = y + metrics.spacing.section
            + metrics.typography.title + metrics.typography.caption
            + metrics.spacing.content;
        // 底部预留多行披露与单行操作反馈，窄窗口时不覆盖传输列表。
        const float list_height = std::max(
            height - list_y - metrics.typography.caption * 4.0f
                - metrics.spacing.content - metrics.spacing.section * 2.0f,
            metrics.control.menuItem);
        components::scrollView(ui, "aki.transfers.list")
            .position(x + metrics.spacing.section, list_y)
            .size(width - metrics.spacing.section * 2.0f, list_height)
            .theme(tokens)
            .gap(metrics.spacing.tiny)
            .step(kTransferRowHeight)
            .contentKey("aki.transfers.rows:"
                + std::to_string(model.state_view.transfers.size()))
            .content([&](eui::Ui& list_ui, float row_width, float) {
                for (const models::TransferView& transfer :
                    model.state_view.transfers) {
                    compose_transfer_row(list_ui, tokens, semantic,
                        "aki.transfers.row." + transfer.id.value, transfer,
                        row_width, kTransferRowHeight, model);
                }
            })
            .build();
    }

    // 传输恢复限制用用户可操作的说明披露；为窄窗口保留换行空间。
    components::text(ui, "aki.transfers.disclosure")
        .text("Interrupted incoming files may need resending.\nPartial files may remain in local storage.")
        .position(x + metrics.spacing.section,
            y + height - metrics.typography.caption * 4.0f
                - metrics.spacing.content - metrics.spacing.section)
        .fontSize(metrics.typography.caption)
        .wrap(true)
        .maxWidth(width - metrics.spacing.section * 2.0f)
        .color(tokens.text)
        .build();
}

}  // namespace aki::ui
