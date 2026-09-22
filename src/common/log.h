#pragma once

#include <cstdint>
#include <exception>
#include <format>
#include <string_view>
#include <utility>

namespace squeeze2raop2::log {

enum class Level : std::uint8_t { Off = 0, Error, Warn, Info, Debug };

void setLevel(Level l);
Level level();
void write(Level l, std::string_view msg);

template <typename... A>
void log(Level l, std::string_view fmt, const A&... a) {
    if (static_cast<int>(l) > static_cast<int>(level())) return;
    if constexpr (sizeof...(a) == 0) {
        write(l, fmt);
    } else {
        // A malformed format string or argument mismatch must never take the
        // process down: fall back to the unformatted pattern.
        try {
            write(l, std::vformat(fmt, std::make_format_args(a...)));
        } catch (const std::exception&) {
            write(l, fmt);
        }
    }
}

inline void error(std::string_view fmt, auto&&... a) {
    log(Level::Error, fmt, std::forward<decltype(a)>(a)...);
}
inline void warn(std::string_view fmt, auto&&... a) {
    log(Level::Warn, fmt, std::forward<decltype(a)>(a)...);
}
inline void info(std::string_view fmt, auto&&... a) {
    log(Level::Info, fmt, std::forward<decltype(a)>(a)...);
}
inline void debug(std::string_view fmt, auto&&... a) {
    log(Level::Debug, fmt, std::forward<decltype(a)>(a)...);
}

}  // namespace squeeze2raop2::log
