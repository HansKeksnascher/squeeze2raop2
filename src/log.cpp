#include "log.h"

#include <chrono>
#include <format>
#include <cstdio>
#include <mutex>

namespace sq2::log {

namespace {
Level g_level = Level::Info;
std::mutex g_mutex;

const char* tag(Level l) {
    switch (l) {
    case Level::Off:   return "off ";
    case Level::Error: return "err ";
    case Level::Warn:  return "warn";
    case Level::Info:  return "info";
    case Level::Debug: return "dbug";
    }
    return "?";
}

std::string stamp() {
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    std::time_t t = clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    return std::format("[{:02}:{:02}:{:02}.{:03}]", tm.tm_hour, tm.tm_min, tm.tm_sec, ms.count());
}
}

void setLevel(Level l) { g_level = l; }
Level level() { return g_level; }

void write(Level l, std::string_view msg) {
    std::lock_guard<std::mutex> g(g_mutex);
    std::fprintf(stderr, "%s %-4s %.*s\n", stamp().c_str(), tag(l),
                 static_cast<int>(msg.size()), msg.data());
}

}
