#pragma once

#include "playback/decoder/decoder.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// stb_vorbis is pulled in only by the .cpp (HEADER_ONLY): the library type is
// opaque here to keep the third-party header out of this header.
struct stb_vorbis;

namespace squeeze2raop2 {

// Native Ogg Vorbis decoder backed by the vendored stb_vorbis (public
// domain / MIT). Handles the LMS format byte 'o', announced as the 'ogg'
// capability, through stb_vorbis's push-data API: compressed bytes accumulate
// in one contiguous window (the consumed prefix is compacted away
// periodically), the stream opens once enough header pages are buffered and
// each decode call sees the unconsumed remainder.
//
// Output is interleaved s16 at the stream's decoded rate. Mono (1 channel) is
// accepted — AirplayOutput::push() expands it to stereo — but > 2 channels are
// rejected to match squeezelite's OGG_ERROR_TOO_MANY_CHANNELS semantics.
class OggDecoder final : public Decoder {
public:
    explicit OggDecoder(const PcmFormat& in);
    ~OggDecoder() override;
    OggDecoder(const OggDecoder&) = delete;
    OggDecoder& operator=(const OggDecoder&) = delete;

    void feed(std::span<const std::byte> data) override;
    // Signal end of compressed input; decodes remaining tail frames.
    void finish() override;
    size_t drain(std::span<int16_t> out) override;

    uint32_t sampleRate() const override { return sampleRate_; }
    int channels() const override { return channels_; }
    size_t pendingBytes() const override { return buffer_.size() - consumed_; }
    bool valid() const override { return opened_; }
    bool hasError() const override { return failed_; }
    std::string_view name() const override { return "ogg"; }

protected:
    PcmFormat decodedFormat() const override { return s16StereoFormat(); }

private:
    void decodeMore();
    void fail(std::string_view why);

    stb_vorbis* vorbis_ = nullptr;   // stb_vorbis decoder state (opaque)
    std::vector<std::byte> buffer_;  // contiguous compressed window
    size_t consumed_ = 0;            // consumed prefix of buffer_
    std::vector<int16_t> pcm_;       // decoded samples awaiting drain
    bool opened_ = false;            // header pass succeeded
    bool eof_ = false;
    bool failed_ = false;
    uint32_t sampleRate_ = 0;
    int channels_ = 2;
};

}  // namespace squeeze2raop2