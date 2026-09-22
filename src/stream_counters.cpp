#include "stream_counters.h"

#include <algorithm>

namespace squeeze2raop2 {

void StreamCounters::reset(uint32_t inputRate) {
    std::lock_guard<std::mutex> lock(mutex_);
    receivedBytes_ = 0;
    fedBytes_ = 0;
    fedSamples_ = 0;
    queuedSamples_ = 0;
    outputRate_ = inputRate ? inputRate : 44100;
}

void StreamCounters::onReceived(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    receivedBytes_ += bytes;
}

void StreamCounters::onFed(size_t samples, size_t channels, uint64_t pendingBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    fedSamples_ += samples / (channels ? channels : 2);
    fedBytes_ = receivedBytes_ > pendingBytes ? receivedBytes_ - pendingBytes : 0;
}

void StreamCounters::setQueued(size_t samples) {
    std::lock_guard<std::mutex> lock(mutex_);
    queuedSamples_ = samples;
}

void StreamCounters::setOutputRate(uint32_t rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    outputRate_ = rate ? rate : 44100;
}

uint64_t StreamCounters::fedSamples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fedSamples_;
}

uint64_t StreamCounters::bytesReceived() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return receivedBytes_;
}

StreamStats StreamCounters::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    StreamStats st;
    st.streamBufferSize = 1 << 20;
    st.streamBufferFullness =
        static_cast<uint32_t>(std::max<int64_t>(0, static_cast<int64_t>(receivedBytes_ - fedBytes_)));
    st.bytesReceived = receivedBytes_;
    st.outputBufferSize = 0;
    st.outputBufferFullness = 0;
    // Report played time, not decoded time: the pipeline decodes ahead into
    // the sender ring, so fedSamples_ leads the receiver by up to the ring
    // occupancy (~1.5 s). LMS derives the progress display from this value,
    // and a decoded-ahead figure makes it run ahead and jump at track end
    // (squeezelite reports frames_played the same way). Subtract the frames
    // still queued to approximate the played position.
    const uint64_t queuedFrames = queuedSamples_ / 2;
    const uint64_t playedFrames = fedSamples_ > queuedFrames ? fedSamples_ - queuedFrames : 0;
    st.elapsedMs = static_cast<uint32_t>(playedFrames * 1000ULL / outputRate_);
    return st;
}

}  // namespace squeeze2raop2
