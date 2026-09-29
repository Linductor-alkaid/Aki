// 传输卡片共享形态实现（语义见 transfer_card.hpp；元素排布自 M5-05 ③
// 会话内文件卡片原样迁移——形状与语义色不变，aki_ui_design §4 复用契约）。
#include "ui/components/transfer_card.hpp"
#include "ui/i18n.hpp"

#include "components/progress.h"
#include "components/text.h"

#include <cstdio>
#include <string>

namespace aki::ui::widgets {
namespace {

constexpr float kLineHeightFactor = 1.35f;  // 与会话页同档（引擎同量级）。

}  // namespace

std::string format_bytes(std::uint64_t bytes) {
    char buffer[32] = {};
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%.1f GB",
            static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB",
            static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB",
            static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu B",
            static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

core::Color transfer_state_color(const AkiSemanticPalette& semantic,
    aki::transfer::TransferState state) {
    using aki::transfer::TransferState;
    switch (state) {
    case TransferState::Transferring: return semantic.brand;
    case TransferState::Paused: return semantic.warning;
    case TransferState::Failed: return semantic.destructive;
    case TransferState::Completed: return semantic.success;
    default: break;
    }
    return semantic.text_subtlest;
}

void compose_transfer_card_body(eui::Ui& ui,
    const components::theme::ThemeColorTokens& tokens,
    const AkiSemanticPalette& semantic, const std::string& id,
    const TransferCardData& data, float width) {
    const auto& metrics = tokens.metrics;
    const float caption_line = metrics.typography.caption * kLineHeightFactor;
    const float icon_advance =
        metrics.typography.caption + metrics.spacing.compact;

    ui.stack(id)
        .width(width)
        .wrapContent()
        .content([&] {
            // 方向 + 文件名（§2.6：收/发方向箭头）。
            components::text(ui, id + ".direction")
                .icon(data.outbound ? eui::utf8(0xF176) : eui::utf8(0xF175))
                .position(0.0f, 0.0f)
                .fontSize(metrics.typography.caption)
                .color(semantic.text_subtle)
                .build();
            components::text(ui, id + ".name")
                .text(data.file_name)
                .position(icon_advance, 0.0f)
                .fontSize(metrics.typography.caption)
                .fontWeight(600)
                .maxWidth(width - icon_advance)
                .color(tokens.text)
                .build();
            const float size_y = caption_line;
            components::text(ui, id + ".size")
                .text(format_bytes(data.size_bytes) + " · "
                    + (data.mime_type.empty() ? "application/octet-stream"
                                              : data.mime_type))
                .position(0.0f, size_y)
                .fontSize(metrics.typography.micro)
                .color(semantic.text_subtlest)
                .build();

            // 进度行（untracked = §6.1 已登记单侧到达边角的兜底态，不猜测
            // 进度——DEC-010/DEC-013）。
            const float progress_y =
                size_y + metrics.typography.micro + metrics.spacing.tiny;
            float state_y = progress_y;
            if (data.tracked) {
                ui.stack(id + ".progress.wrap")
                    .position(0.0f, progress_y)
                    .size(width, 6.0f)
                    .content([&] {
                        components::ProgressStyle progress_style(tokens);
                        progress_style.fill =
                            transfer_state_color(semantic, data.state);
                        components::progress(ui, id + ".progress")
                            .size(width, 6.0f)
                            .value(static_cast<float>(data.progress))
                            .style(progress_style)
                            .build();
                    })
                    .build();
                state_y = progress_y + 6.0f + metrics.spacing.tiny;
            }
            const std::string state_text = data.tracked
                ? tr(aki::transfer::to_string(data.state)) + " · "
                    + std::to_string(
                        static_cast<int>(data.progress * 100.0))
                    + "%"
                : tr("no transfer row (single-side arrival)");
            components::text(ui, id + ".state")
                .text(state_text)
                .position(0.0f, state_y)
                .fontSize(metrics.typography.micro)
                .color(data.tracked
                        ? transfer_state_color(semantic, data.state)
                        : semantic.warning)
                .build();
        })
        .build();
}

}  // namespace aki::ui::widgets
