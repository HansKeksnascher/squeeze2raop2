#pragma once

#include "slimproto.h"

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace squeeze2raop2 {

// The stream loop's byte/frame accounting. Written on the stream thread,
// read by the STAT provider on the SlimProto reader thread; one mutex gives
// currentStats() a consistent snapshot. Owned by PlayerSession so the last
// track's figures survive until the next stream resets them.
class StreamCounters {
public:
    // Fresh stream: zero the byte/frame counters and adopt the input rate.
    void reset(uint32_t inputRate);

    void onReceived(uint64_t bytes);  // bytes read from the source
    // Consumed prefix: `samples` interleaved s16 emitted, `pendingBytes` the
    // decoder's unemitted backlog (clamped so it cannot underflow).
    void onFed(size_t samples, size_t channels, uint64_t pendingBytes);
    void setQueued(size_t samples);      // sender-ring occupancy
    void setOutputRate(uint32_t rate);   // decoder format adoption

    // Played-time snapshot for the LMS STAT reply.
    StreamStats stats() const;
    // Frames emitted so far (the realtime pacer's timeline).
    uint64_t fedSamples() const;
    // Source bytes received so far (rate regulator + end-of-stream log).
    uint64_t bytesReceived() const;

private:
    mutable std::mutex mutex_;
    uint64_t receivedBytes_ = 0;
    uint64_t fedBytes_ = 0;
    uint64_t fedSamples_ = 0;
    size_t queuedSamples_ = 0;
    uint32_t outputRate_ = 44100;
};

}  // namespace squeeze2raop2
