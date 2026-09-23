#include "common/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <mutex>

namespace squeeze2raop2::log {

namespace {
std::atomic<Level> g_level{Level::Info};
static_assert(std::atomic<Level>::is_always_lock_free);
std::mutex g_mutex;

// "+0200" / "-0500" -> seconds east of UTC (0 when unknown).
long offsetFromZ(const char* z) {
    const bool neg = z[0] == '-';
    if ((!neg && z[0] != '+') || z[1] < '0' || z[1] > '9') return 0;
    const int hh = (z[1] - '0') * 10 + (z[2] - '0');
    const int mm = (z[3] - '0') * 10 + (z[4] - '0');
    const long secs = static_cast<long>(hh) * 3600 + static_cast<long>(mm) * 60;
    return neg ? -secs : secs;
}
}  // namespace

std::string_view levelTag(Level l) {
    switch (l) {
    case Level::Off: return "OFF";
    case Level::Error: return "ERR";
    case Level::Warn: return "WRN";
    case Level::Info: return "INF";
    case Level::Debug: return "DBG";
    }
    return "???";
}

std::string_view areaTag(Area a) {
    switch (a) {
    case Area::App: return "APP";
    case Area::Lms: return "LMS";
    case Area::Mdns: return "MDNS";
    case Area::Ap: return "AP";
    case Area::Dec: return "DEC";
    case Area::Pb: return "PB";
    case Area::Ses: return "SES";
    }
    return "???";
}

std::string formatStamp(const std::tm& local, int millis, long offsetSeconds) {
    const long abs = offsetSeconds < 0 ? -offsetSeconds : offsetSeconds;
    return std::format("[{:02}:{:02}:{:02}.{:03}{}{:02}:{:02}]", local.tm_hour, local.tm_min,
                       local.tm_sec, millis, offsetSeconds < 0 ? '-' : '+', abs / 3600,
                       (abs % 3600) / 60);
}

std::string formatLine(Level l, Area a, std::string_view stamp, std::string_view msg) {
    // `stamp` already includes its brackets.
    return std::format("[{}]{}[{}]: {}", levelTag(l), stamp, areaTag(a), msg);
}

void setLevel(Level l) { g_level.store(l, std::memory_order_relaxed); }
Level level() { return g_level.load(std::memory_order_relaxed); }

void write(Level l, Area a, std::string_view msg) {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t t = clock::to_time_t(now);
    std::tm local{};
    localtime_r(&t, &local);
    char z[8] = {};
    (void)std::strftime(z, sizeof(z), "%z", &local);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::string line =
        formatLine(l, a, formatStamp(local, static_cast<int>(ms.count()), offsetFromZ(z)), msg);
    std::lock_guard<std::mutex> g(g_mutex);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
}

}  // namespace squeeze2raop2::log