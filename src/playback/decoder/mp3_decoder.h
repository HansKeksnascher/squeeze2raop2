#pragma once

#include "common/third_party_warnings.h"
#include "playback/decoder/buffered_decoder.h"

#include <cstddef>
#include <cstdint>
#include <span>

SQUEEZE2RAOP2_TP_WARNINGS_PUSH
#include <minimp3.h>
SQUEEZE2RAOP2_TP_WARNINGS_POP

namespace squeeze2raop2 {

// Incremental MP3 decoder wrapper around the public-domain minimp3 single header.
// Compressed bytes accumulate in one contiguous window (the consumed prefix is
// compacted away periodically); every decode call sees the full remainder, which
// keeps minimp3's frame-chain validation stable while streaming in chunks.
class Mp3Decoder final : public BufferedDecoder {
public:
    explicit Mp3Decoder(const PcmFormat& in);
    Mp3Decoder(const Mp3Decoder&) = delete;
    Mp3Decoder& operator=(const Mp3Decoder&) = delete;

    void feed(std::span<const std::byte> data) override;
    // Signal end of compressed input; decodes remaining tail frames.
    void finish() override;

    uint32_t sampleRate() const override { return sampleRate_; }
    int channels() const override { return channels_; }
    bool valid() const override { return sampleRate_ != 0; }
    std::string_view name() const override { return "mp3"; }

protected:
    PcmFormat decodedFormat() const override { return s16StereoFormat(); }

private:
    void decodeMore();

    mp3dec_t dec_{};  // minimp3 decoder state, embedded by value
    uint32_t sampleRate_ = 0;
    int channels_ = 2;
};

}  // namespace squeeze2raop2