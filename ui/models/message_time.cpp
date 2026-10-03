#include "ui/models/message_time.hpp"

#include <array>
#include <cstdio>
#include <ctime>
#include <optional>

namespace aki::ui::models {
namespace {
std::optional<LocalCalendarTime> to_local(std::chrono::system_clock::time_point time) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
#if defined(_MSC_VER)
    if (localtime_s(&local, &raw) != 0)
        return std::nullopt;
#else
    if (localtime_r(&raw, &local) == nullptr)
        return std::nullopt;
#endif
    return LocalCalendarTime{std::chrono::year{local.tm_year + 1900} /
                                 std::chrono::month{static_cast<unsigned>(local.tm_mon + 1)} /
                                 std::chrono::day{static_cast<unsigned>(local.tm_mday)},
                             local.tm_hour, local.tm_min};
}
} // namespace

std::string format_local_message_time(LocalCalendarTime message, LocalCalendarTime now,
                                      Language language) {
    using namespace std::chrono;
    if (!message.date.ok() || !now.date.ok() || message.hour < 0 || message.hour > 23 ||
        message.minute < 0 || message.minute > 59)
        return "--:--";
    char time[8]{};
    std::snprintf(time, sizeof(time), "%02d:%02d", message.hour, message.minute);
    const sys_days message_day{message.date}, today{now.date};
    const auto age = today - message_day;
    if (age == days{0})
        return time;
    const bool chinese = language == Language::Chinese;
    if (age == days{1})
        return std::string(chinese ? "昨天 " : "Yesterday ") + time;
    const auto week_start = today - days{weekday{today}.iso_encoding() - 1};
    const unsigned day_index = weekday{message_day}.iso_encoding() - 1;
    constexpr std::array<const char*, 7> zh{"星期一", "星期二", "星期三", "星期四",
                                            "星期五", "星期六", "星期日"};
    constexpr std::array<const char*, 7> en{"Monday", "Tuesday",  "Wednesday", "Thursday",
                                            "Friday", "Saturday", "Sunday"};
    if (age > days{0} && message_day >= week_start - days{7}) {
        std::string label;
        if (message_day < week_start)
            label = chinese ? "上周" : "Last week ";
        label += chinese ? zh[day_index] : en[day_index];
        return label + " " + time;
    }
    char date[64]{};
    const int year_value = static_cast<int>(message.date.year());
    const auto month_value = static_cast<unsigned>(message.date.month());
    const auto day_value = static_cast<unsigned>(message.date.day());
    std::snprintf(date, sizeof(date), chinese ? "%04d 年 %02u 月 %02u 日 " : "%04d-%02u-%02u ",
                  year_value, month_value, day_value);
    return std::string(date) + time;
}

std::string format_message_time(std::chrono::system_clock::time_point message,
                                std::chrono::system_clock::time_point now, Language language) {
    if (message.time_since_epoch().count() == 0)
        return "--:--";
    const auto local_message = to_local(message), local_now = to_local(now);
    if (!local_message || !local_now)
        return "--:--";
    return format_local_message_time(*local_message, *local_now, language);
}
} // namespace aki::ui::models
