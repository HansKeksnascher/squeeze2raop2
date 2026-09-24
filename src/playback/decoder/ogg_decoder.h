#pragma once

#include "playback/decoder/buffered_decoder.h"

#include <cstddef>
#include <cstdint>
#include <span>

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
class OggDecoder final : public BufferedDecoder {
public:
    explicit OggDecoder(const PcmFormat& in);
    ~OggDecoder() override;
    OggDecoder(const OggDecoder&) = delete;
    OggDecoder& operator=(const OggDecoder&) = delete;

    void feed(std::span<const std::byte> data) override;
    // Signal end of compressed input; decodes remaining tail frames.
    void finish() override;

    uint32_t sampleRate() const override { return sampleRate_; }
    int channels() const override { return channels_; }
    bool valid() const override { return opened_; }
    std::string_view name() const override { return "ogg"; }

protected:
    PcmFormat decodedFormat() const override { return s16StereoFormat(); }

private:
    void decodeMore();

    stb_vorbis* vorbis_ = nullptr;  // stb_vorbis decoder state (opaque)
    bool opened_ = false;           // header pass succeeded
    uint32_t sampleRate_ = 0;
    int channels_ = 2;
};

}  // namespace squeeze2raop2