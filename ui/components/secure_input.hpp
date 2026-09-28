#pragma once

#include "ui/theme/aki_theme.hpp"

#include "components/text.h"

#include <eui/dsl.h>

#include <algorithm>
#include <cstddef>
#include <string>

namespace aki::ui {

inline void clear_secret(std::string& secret) {
    std::fill(secret.begin(), secret.end(), '\0');
    secret.clear();
}

inline std::size_t utf8_scalar_count(const std::string& value) {
    return static_cast<std::size_t>(std::count_if(value.begin(), value.end(),
        [](unsigned char byte) { return (byte & 0xC0U) != 0x80U; }));
}

// Aki-owned masked field. EUI-NEO InputBuilder has no password mode, so a
// normal input would put the secret into drawable text and clipboard state.
inline void secureInput(eui::Ui& ui, const std::string& id, std::string& value,
    const components::theme::ThemeColorTokens& tokens, float x, float y, float width,
    float height, const std::string& placeholder) {
    const std::string hit_id = id + ".hit";
    const bool focused = ui.isFocused(hit_id);
    ui.rect(hit_id)
        .position(x, y).size(width, height)
        .color(tokens.surface)
        .border(1.0f, focused ? tokens.primary : tokens.border)
        .radius(tokens.metrics.radius.small)
        .focusable()
        .onKeyEvent([&value](const core::KeyEvent& event) {
            if (!event.isDown()) return false;
            if (event.key == core::InputKey::Tab
                || event.key == core::InputKey::Enter
                || event.key == core::InputKey::Escape) return false;
            if (event.key == core::InputKey::Backspace) {
                if (!value.empty()) {
                    std::size_t pos = value.size() - 1;
                    while (pos > 0
                        && (static_cast<unsigned char>(value[pos]) & 0xC0U)
                            == 0x80U) {
                        --pos;
                    }
                    std::fill(value.begin() + static_cast<std::ptrdiff_t>(pos),
                        value.end(), '\0');
                    value.resize(pos);
                }
                return true;
            }
            // No selection, copy, cut, undo or plaintext echo for secrets.
            return true;
        })
        .onTextInput([&value](const core::TextInputEvent& event) {
            if (event.text.empty() && event.pasteText.empty()) return;
            const std::string& added = event.pasteText.empty()
                ? event.text : event.pasteText;
            if (!added.empty() && added.find_first_of("\r\n")
                    == std::string::npos
                && value.size() + added.size() <= 256) {
                value += added;
            }
        })
        .build();
    const std::string display = value.empty()
        ? placeholder : std::string(utf8_scalar_count(value), '*');
    components::text(ui, id + ".masked")
        .text(display)
        .position(x + tokens.metrics.spacing.content,
            y + (height - tokens.metrics.typography.body) * 0.5f)
        .fontSize(tokens.metrics.typography.body)
        .color(tokens.text)
        .build();
}

}  // namespace aki::ui
