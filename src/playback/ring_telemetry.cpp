#include "playback/ring_telemetry.h"

#include "common/log.h"

namespace squeeze2raop2 {

std::optional<uint64_t> RingTelemetry::observe(size_t queued, uint64_t nowMs) {
    if (queued < min_) min_ = queued;
    if (queued > max_) max_ = queued;

    std::optional<uint64_t> window;
    if (markMs_ == 0) {
        markMs_ = nowMs;
    } else if (nowMs - markMs_ >= 10000) {
        log::info("[ap] ring 10s: cur={} min={} max={} samples", queued, min_, max_);
        window = nowMs - markMs_;
        min_ = SIZE_MAX;
        max_ = 0;
        markMs_ = nowMs;
    }

    if (queued == 0) {
        if (!starvedMs_) {
            starvedMs_ = nowMs;
            log::warn("[ap] ring starved (0 samples)");
        }
    } else if (starvedMs_) {
        log::info("[ap] ring recovered after {} ms", nowMs - starvedMs_);
        starvedMs_ = 0;
    }
    return window;
}

}  // namespace squeeze2raop2
