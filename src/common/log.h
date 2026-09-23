#pragma once

#include <cstdint>
#include <ctime>
#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace squeeze2raop2::log {

enum class Level : std::uint8_t { Off = 0, Error, Warn, Info, Debug };

// Subsystem that produced a line; rendered as a bracketed tag.
enum class Area : std::uint8_t { App, Lms, Mdns, Ap, Dec, Pb, Ses };

void setLevel(Level l);
Level level();

// Line-format pieces, exposed for the unit test.
std::string_view levelTag(Level l);  // "OFF"/"ERR"/"WRN"/"INF"/"DBG"
std::string_view areaTag(Area a);    // "APP"/"LMS"/"MDNS"/"AP"/"DEC"/"PB"/"SES"
// "[HH:MM:SS.mmm+HH:MM]" from a broken-down local time and its UTC offset.
std::string formatStamp(const std::tm& local, int millis, long offsetSeconds);
// "[LVL][stamp][AREA]: message"
std::string formatLine(Level l, Area a, std::string_view stamp, std::string_view msg);

void write(Level l, Area a, std::string_view msg);

template <typename... A>
void log(Level l, Area a, std::string_view fmt, const A&... args) {
    if (static_cast<int>(l) > static_cast<int>(level())) return;
    if constexpr (sizeof...(args) == 0) {
        write(l, a, fmt);
    } else {
        // A malformed format string or argument mismatch must never take the
        // process down: fall back to the unformatted pattern.
        try {
            write(l, a, std::vformat(fmt, std::make_format_args(args...)));
        } catch (const std::exception&) {
            write(l, a, fmt);
        }
    }
}

inline void error(Area a, std::string_view fmt, auto&&... x) {
    log(Level::Error, a, fmt, std::forward<decltype(x)>(x)...);
}
inline void warn(Area a, std::string_view fmt, auto&&... x) {
    log(Level::Warn, a, fmt, std::forward<decltype(x)>(x)...);
}
inline void info(Area a, std::string_view fmt, auto&&... x) {
    log(Level::Info, a, fmt, std::forward<decltype(x)>(x)...);
}
inline void debug(Area a, std::string_view fmt, auto&&... x) {
    log(Level::Debug, a, fmt, std::forward<decltype(x)>(x)...);
}

}  // namespace squeeze2raop2::log