#pragma once

#include "lms/slimproto_protocol.h"

#include <cstddef>
#include <cstdio>
#include <span>
#include <string>

namespace squeeze2raop2 {

// Debug-only WAV capture: when a player has a --sink path configured, the
// decoded PCM is tee'd here so the stream can be inspected as a .wav file.
// Not part of the playback path.
class DebugWavSink {
public:
    explicit DebugWavSink(std::string path);
    ~DebugWavSink();

    bool open(const PcmFormat& format, std::string& errorOut);
    void feed(std::span<const std::byte> data, const PcmFormat& format);
    void close();

    uint64_t bytesTotal() const { return total_; }

private:
    std::string path_;
    FILE* fp_ = nullptr;
    PcmFormat format_;
    uint64_t total_ = 0;
    bool headerWritten_ = false;
    bool writeFailed_ = false;
};

}  // namespace squeeze2raop2
