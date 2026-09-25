#pragma once

#include "playback/decoder/buffered_decoder.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace squeeze2raop2 {

// Native Ogg Opus decoder backed by the vendored libogg + libopus (both
// BSD-3-Clause). Handles the LMS format byte 'u', announced as the 'ops'
// capability. libogg's push-oriented page demuxer fits the pipeline's
// feed/drain shape: bytes go into ogg_sync, complete pages into an ogg_stream,
// and the reassembled packets to libopus.
//
// Output is interleaved s16 at Opus's native 48 kHz. Channel mapping family 0
// (mono/stereo) is supported; other families and > 2 channels are rejected,
// matching the Ogg Vorbis stereo guard. The OpusHead pre-skip is trimmed and
// the stream is end-trimmed to the final granule position (only meaningful for
// finite .opus files); the OpusHead output gain is applied via OPUS_SET_GAIN.
class OggOpusDecoder final : public BufferedDecoder {
public:
    explicit OggOpusDecoder(const PcmFormat& in);
    ~OggOpusDecoder() override;
    OggOpusDecoder(const OggOpusDecoder&) = delete;
    OggOpusDecoder& operator=(const OggOpusDecoder&) = delete;

    void feed(std::span<const std::byte> data) override;
    // Signal end of compressed input. Opus packets are independently
    // decodable, so there is no decoder tail; this only catches a stream that
    // ended before its OpusHead.
    void finish() override;

    uint32_t sampleRate() const override { return sampleRate_; }
    int channels() const override { return channels_; }
    size_t pendingBytes() const override;
    bool valid() const override { return headerSeen_; }
    std::string_view name() const override { return "opus"; }

protected:
    PcmFormat decodedFormat() const override { return s16StereoFormat(); }

private:
    // Ogg/Opus state; defined in the .cpp so the third-party headers stay out
    // of this one.
    struct Impl;

    // Copy every byte into ogg_sync's internal buffer. False on allocation
    // failure.
    bool pushBytes(std::span<const std::byte> data);
    // Pull complete pages off ogg_sync and every packet off the page's stream.
    void decodeMore();
    void handlePacket(const void* data, int bytes);
    // Append `count` decoded frames to pcm_, applying the pre-skip and, once
    // the final granule is known, the end trim.
    void appendFrames(int count);

    std::unique_ptr<Impl> impl_;
    // 0 until the OpusHead is parsed so format() falls back to the input.
    uint32_t sampleRate_ = 0;
    int channels_ = kDefaultChannels;
    bool headerSeen_ = false;
};

}  // namespace squeeze2raop2