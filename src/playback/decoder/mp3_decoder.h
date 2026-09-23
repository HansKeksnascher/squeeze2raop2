#pragma once

#include "common/third_party_warnings.h"
#include "playback/decoder/decoder.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

SQUEEZE2RAOP2_TP_WARNINGS_PUSH
#include <minimp3.h>
SQUEEZE2RAOP2_TP_WARNINGS_POP

namespace squeeze2raop2 {

// Incremental MP3 decoder wrapper around the public-domain minimp3 single header.
// Compressed bytes accumulate in one contiguous window (the consumed prefix is
// compacted away periodically); every decode call sees the full remainder, which
// keeps minimp3's frame-chain validation stable while streaming in chunks.
class Mp3Decoder final : public Decoder {
public:
    explicit Mp3Decoder(const PcmFormat& in);
    Mp3Decoder(const Mp3Decoder&) = delete;
    Mp3Decoder& operator=(const Mp3Decoder&) = delete;

    void feed(std::span<const std::byte> data) override;
    // Signal end of compressed input; decodes remaining tail frames.
    void finish() override;
    size_t drain(std::span<int16_t> out) override;

    uint32_t sampleRate() const override { return sampleRate_; }
    int channels() const override { return channels_; }
    size_t pendingBytes() const override { return buffer_.size() - consumed_; }
    bool valid() const override { return sampleRate_ != 0; }
    bool hasError() const override { return failed_; }
    std::string_view name() const override { return "mp3"; }

protected:
    PcmFormat decodedFormat() const override { return s16StereoFormat(); }

private:
    void decodeMore();

    mp3dec_t dec_{};                 // minimp3 decoder state, embedded by value
    std::vector<std::byte> buffer_;  // contiguous compressed window
    size_t consumed_ = 0;            // decoded/skipped prefix of buffer_
    std::vector<int16_t> pcm_;       // decoded samples awaiting drain
    bool eof_ = false;
    uint32_t sampleRate_ = 0;
    int channels_ = 2;
    bool failed_ = false;
};

}  // namespace squeeze2raop2