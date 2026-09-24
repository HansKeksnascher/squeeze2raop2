#pragma once

// LMS wire payload/enum types shared by the slimproto client and the playback
// pipeline (decoders, sinks, counters). Kept in a light header so those users
// do not pull in the client's sockets, threads and mutexes.

#include <cstdint>
#include <string>

namespace squeeze2raop2 {

enum class StreamFormat : uint8_t {
    Unknown = '?',
    Pcm = 'p',
    Mp3 = 'm',
    Flac = 'f',
    Wma = 'w',
    Ogg = 'o',
    Opus = 'u',
    Aac = 'a',
    Alac = 'l',
};

struct PcmParams {
    uint8_t sampleSizeCode = '?';
    uint8_t sampleRateCode = '?';
    uint8_t channelsCode = '?';
    uint8_t endianCode = '?';

    bool operator==(const PcmParams&) const = default;
};

struct PcmFormat {
    uint32_t sampleRate = 44100;
    uint8_t bitsPerSample = 16;
    uint8_t channels = 2;
    bool bigEndian = false;

    bool operator==(const PcmFormat&) const = default;
};

uint32_t sampleRateFromCode(uint8_t code);
uint8_t bitsPerSampleFromCode(uint8_t code);
uint8_t channelsFromCode(uint8_t code);
PcmFormat pcmFormat(const PcmParams& params, uint32_t fallbackRate);

// 'strm s' flags byte bits we care about (Squeezebox.pm). 0x20 is set on a
// direct stream when the URL is https and the player advertised CanHTTPS=1:
// the transport must use TLS.
constexpr uint8_t kStrmFlagSsl = 0x20;

struct StrmStart {
    uint8_t autostart = 1;
    StreamFormat format = StreamFormat::Unknown;
    PcmParams pcm;
    uint8_t thresholdKb = 0;
    uint8_t transitionType = 0;
    uint8_t transitionPeriodS = 0;
    uint8_t flags = 0;
    bool ssl = false;  // flags & kStrmFlagSsl
    uint32_t replayGain = 0;
    uint32_t serverIp = 0;
    uint16_t serverPort = 0;
    std::string request;
};

struct StreamStats {
    uint32_t streamBufferSize = 0;
    uint32_t streamBufferFullness = 0;
    uint64_t bytesReceived = 0;
    uint32_t outputBufferSize = 0;
    uint32_t outputBufferFullness = 0;
    uint32_t elapsedMs = 0;
};

}  // namespace squeeze2raop2