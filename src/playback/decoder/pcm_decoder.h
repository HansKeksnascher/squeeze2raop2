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
    // `outputRate`: the pipeline's target output clock (0 = emit at the
    // source rate, no regulation). With a target, setSourceRate() engages
    // a linear-interpolation rate stage that stretches/shrinks the source
    // to exactly the target — a real-time source that under-delivers by a
    // fraction of a percent no longer drains the pipeline's buffer.
    PcmDecoder(const PcmFormat& in, uint32_t outputRate);

    PcmDecoder(const PcmDecoder&) = delete;
    PcmDecoder& operator=(const PcmDecoder&) = delete;

    void feed(std::span<const std::byte> data) override;
    // Emits at most out.size()/2 interleaved s16 stereo frames (native
    // byte order, at the target output rate when regulated). Returns the
    // number of SAMPLES written.
    size_t drain(std::span<int16_t> out) override;

    uint32_t sampleRate() const override { return fmt_.sampleRate; }
    int channels() const override { return fmt_.channels; }
    // Unemitted input, expressed in raw input bytes: the normalization
    // buffer plus the stage's frames (one stage frame per input frame).
    // The bridge's source-rate measurement needs the input-side backlog,
    // not just the raw byte buffer — a backed-up rate stage would
    // otherwise masquerade as a slowing source.
    size_t pendingBytes() const override {
        return buf_.size() + (stage_.size() / 2) * bytesPerFrame_;
    }
    bool valid() const override { return !failed_; }
    bool hasError() const override { return failed_; }
    std::string_view name() const override { return "pcm"; }

    void setSourceRate(double framesPerSecond) override;

protected:
    // The effective output format (container header may override the strm
    // params; with rate regulation the output clock is the target rate).
    PcmFormat decodedFormat() const override { return fmt_; }
    bool regulatesRate() const override { return outRate_ != 0; }

private:
    // Returns bytes to skip at the stream head, adopting the container's
    // format; 0 = no container (raw per strm params). Requires enough
    // buffered bytes to decide, otherwise returns nullopt ("not yet").
    std::optional<size_t> checkHeader();
    // Normalize buffered raw bytes into interleaved s16 stereo frames in
    // `stage_` (at the source rate). Returns frames added.
    size_t normalizeMore();
    // Rate stage: emit up to want frames from `stage_` into out at the
    // target rate (lerp; bypass = bit-exact move when step is exactly 1).
    size_t rateStageEmit(std::span<int16_t> out);
    void resetRateStage();

    // Source conversion params (the container header may override them).
    uint8_t srcBits_ = 16;
    uint8_t srcChannels_ = 2;
    bool srcBigEndian_ = false;
    uint32_t srcRate_ = 44100;
    size_t bytesPerFrame_ = 4;    // source input frame size
    PcmFormat fmt_;               // OUTPUT format (post-adoption, post-rate)
    uint32_t outRate_ = 0;        // target output clock (0 = unregulated)
    std::vector<std::byte> buf_;  // raw input bytes pending normalization
    std::vector<int16_t> stage_;  // normalized stereo frames at source rate
    double stagePhase_ = 0.0;     // fractional read position into stage_
    double rateStep_ = 1.0;       // source frames consumed per output frame
    bool rateEngaged_ = false;
    size_t headerBytes_ = 0;  // container bytes skipped so far
    bool headerDone_ = false;
    bool failed_ = false;
};

}  // namespace squeeze2raop2