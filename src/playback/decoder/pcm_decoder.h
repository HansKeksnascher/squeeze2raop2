#pragma once

#include "playback/decoder/decoder.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace squeeze2raop2 {

// PCM-to-PCM decoder, mirroring squeezelite's pcm.c: raw LMS 'p' streams are
// decoded like any other codec — container headers (WAV/AIFF) are detected,
// parsed and skipped (their fmt overrides the strm params), and samples are
// normalized to interleaved s16 stereo through the same feed/drain shape the
// MP3 decoder uses, so both paths share one pipeline end to end.
class PcmDecoder final : public Decoder {
public:
    // Decodes raw LMS 'p' streams at their native rate; the pipeline
    // resamples to the AirPlay output clock downstream.
    explicit PcmDecoder(const PcmFormat& in);

    PcmDecoder(const PcmDecoder&) = delete;
    PcmDecoder& operator=(const PcmDecoder&) = delete;

    void feed(std::span<const std::byte> data) override;
    // Emits at most out.size()/2 interleaved s16 stereo frames (native
    // byte order, at the source rate). Returns the number of SAMPLES written.
    size_t drain(std::span<int16_t> out) override;

    uint32_t sampleRate() const override { return fmt_.sampleRate; }
    int channels() const override { return fmt_.channels; }
    // Unemitted input, expressed in raw input bytes: the raw byte buffer plus
    // the normalization stage's frames (one stage frame per input frame).
    size_t pendingBytes() const override {
        return buf_.size() + (stage_.size() / 2) * bytesPerFrame_;
    }
    bool valid() const override { return !failed_; }
    bool hasError() const override { return failed_; }
    std::string_view name() const override { return "pcm"; }

protected:
    // The effective output format (the container header may override the strm
    // params; the rate is the source rate).
    PcmFormat decodedFormat() const override { return fmt_; }

private:
    // Returns bytes to skip at the stream head, adopting the container's
    // format; 0 = no container (raw per strm params). Requires enough
    // buffered bytes to decide, otherwise returns nullopt ("not yet").
    std::optional<size_t> checkHeader();
    // Normalize buffered raw bytes into interleaved s16 stereo frames in
    // `stage_` (at the source rate). Returns frames added.
    size_t normalizeMore();
    // Move up to out.size()/2 normalized frames from `stage_` into out.
    // Returns the number of SAMPLES written.
    size_t stageEmit(std::span<int16_t> out);

    // Source conversion params (the container header may override them).
    uint8_t srcBits_ = kDefaultBitsPerSample;
    uint8_t srcChannels_ = kDefaultChannels;
    bool srcBigEndian_ = false;
    uint32_t srcRate_ = kDefaultSampleRate;
    size_t bytesPerFrame_ = 4;
    PcmFormat fmt_;               // output format (post container adoption)
    std::vector<std::byte> buf_;  // raw input bytes pending normalization
    std::vector<int16_t> stage_;  // normalized stereo frames at source rate
    bool headerDone_ = false;
    bool failed_ = false;
};

}  // namespace squeeze2raop2