#pragma once

#include "ui/i18n.hpp"
#include <chrono>
#include <string>

namespace aki::ui::models {

// Calendar values are already converted into the viewer's local time zone.
struct LocalCalendarTime {
    std::chrono::year_month_day date;
    int hour = 0;
    int minute = 0;
};

[[nodiscard]] std::string format_local_message_time(LocalCalendarTime message,
                                                    LocalCalendarTime now, Language language);
[[nodiscard]] std::string format_message_time(std::chrono::system_clock::time_point message,
                                              std::chrono::system_clock::time_point now,
                                              Language language);

} // namespace aki::ui::models
