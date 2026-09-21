#pragma once

#include <format>
#include <string_view>

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
        write(l, std::vformat(fmt, std::make_format_args(a...)));
    }
}  // namespace squeeze2raop2

inline void fatal(std::string_view fmt, auto&&... a) {
    log(Level::Error, fmt, std::forward<decltype(a)>(a)...);
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
