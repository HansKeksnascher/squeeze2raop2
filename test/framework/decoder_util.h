#pragma once

// Shared helpers for the decoder unit tests: pull every queued sample out of a
// Decoder and measure it. Header-only; lives beside check.h on the test
// include path.

#include "playback/decoder/decoder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

namespace squeeze2raop2::test {

// Every queued sample via the raw drain() interface.
inline std::vector<int16_t> drainAll(Decoder& dec) {
    std::vector<int16_t> out;
    std::array<int16_t, 4096> buf{};
    for (;;) {
        const size_t n = dec.drain(buf);
        if (n == 0) break;
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
    }
    return out;
}

// Every queued sample via the pipeline's nextChunk() cadence.
inline std::vector<int16_t> chunksAll(Decoder& dec) {
    std::vector<int16_t> out;
    for (;;) {
        const std::span<const int16_t> chunk = dec.nextChunk();
        if (chunk.empty()) break;
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    return out;
}

// Largest absolute sample value (0 for empty/silent).
inline int peak(const std::vector<int16_t>& pcm) {
    int p = 0;
    for (const int16_t s : pcm) p = std::max(p, std::abs(static_cast<int>(s)));
    return p;
}

}  // namespace squeeze2raop2::test
