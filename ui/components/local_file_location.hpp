#pragma once
#include "ui/models/ui_actions.hpp"
#include "ui/theme/aki_theme.hpp"
#include <eui/dsl.h>
#include <functional>
#include <string>
namespace aki::ui::widgets {
void compose_local_file_location(eui::Ui& ui,
    const components::theme::ThemeColorTokens& tokens,
    const AkiSemanticPalette& semantic, const std::string& id,
    const std::string& root, const std::string& relative, bool available,
    const std::string& error,
    float width, bool compact, models::UiActions* actions,
    std::function<void(std::string)> feedback);
}
