#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace squeeze2raop2 {

// Output-ring health sampler for the stream loop: tracks occupancy extremes
// between 10 s summaries and latches one warn/recover pair per starvation
// episode. No I/O beyond the log lines; the caller drives rate regulation on
// the returned window. Stream-thread only.
class RingTelemetry {
public:
    // Record one occupancy sample (`queued` samples). Returns the window
    // length in ms when a 10 s summary boundary is crossed (the caller then
    // regulates the source rate), nullopt otherwise.
    std::optional<uint64_t> observe(size_t queued, uint64_t nowMs);

    // True while the ring is in a latched starvation episode (one warn per
    // episode, cleared by the first non-zero sample).
    bool starved() const { return starvedMs_ != 0; }

private:
    size_t min_ = SIZE_MAX;
    size_t max_ = 0;
    uint64_t markMs_ = 0;
    uint64_t starvedMs_ = 0;
};

}  // namespace squeeze2raop2
