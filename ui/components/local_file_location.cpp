#include "ui/components/local_file_location.hpp"
#include "ui/models/local_file_path.hpp"
#include "ui/i18n.hpp"
#include "components/button.h"
#include "components/text.h"
#include <algorithm>

namespace aki::ui::widgets {
void compose_local_file_location(eui::Ui& ui,
    const components::theme::ThemeColorTokens& tokens,
    const AkiSemanticPalette&, const std::string& id,
    const std::string& root, const std::string& relative, bool available,
    const std::string& error,
    float width, bool compact, models::UiActions* actions,
    std::function<void(std::string)> feedback) {
    const auto file = models::local_file_path(root, relative);
    const auto& metrics = tokens.metrics;
    if (file.empty()) {
        components::text(ui, id + ".saving").text(tr(error.empty() ? "Saving local file…" : "Could not save local file."))
            .maxWidth(width).wrap(true).fontSize(metrics.typography.hint)
            .color(tokens.text).build();
        return;
    }
    const auto path = file.string();
    auto displayed = path;
    if (compact) {
        const auto budget = static_cast<std::size_t>(std::max(16.0f,
            width / metrics.typography.hint));
        if (displayed.size() > budget) {
            auto start = displayed.size() - budget;
            while (start < displayed.size()
                && (static_cast<unsigned char>(displayed[start]) & 0xc0) == 0x80) ++start;
            displayed = "…" + displayed.substr(start);
        }
    }
    ui.column(id).width(width).wrapContent().gap(metrics.spacing.tiny)
        .content([&] {
            components::text(ui, id + ".label")
                .text(tr(available ? "Saved to" : "Local file is missing"))
                .maxWidth(width).fontSize(metrics.typography.hint)
                .color(tokens.text).build();
            components::text(ui, id + ".path").text(displayed)
                .maxWidth(width).wrap(!compact).fontFamily("Mono")
                .fontSize(metrics.typography.hint).color(tokens.text).build();
            const auto button_width = std::min(
                metrics.control.field * 3.0f, (width - metrics.spacing.compact) * 0.5f);
            ui.row(id + ".actions").width(width).wrapContent()
                .gap(metrics.spacing.compact).content([&] {
                    if (actions && actions->copy_local_path) {
                        components::button(ui, id + ".copy")
                            .size(button_width, metrics.control.menuItem)
                            .text(tr("Copy path")).fontSize(metrics.typography.hint)
                            .theme(tokens, false)
                            .onClick([actions, path, feedback] {
                                actions->copy_local_path(path);
                                feedback(tr("Path copied"));
                            }).build();
                    }
                    if (actions && actions->open_file_folder) {
                        components::button(ui, id + ".open")
                            .size(button_width, metrics.control.menuItem)
                            .text(tr("Open folder")).fontSize(metrics.typography.hint)
                            .theme(tokens, false)
                            .onClick([actions, file, feedback] {
                                std::string error;
                                if (!actions->open_file_folder(file, error))
                                    feedback(tr("Cannot open local folder.") + " " + error);
                            }).build();
                    }
                }).build();
        }).build();
}
}
