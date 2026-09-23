#pragma once

#include "lms/wire_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace squeeze2raop2 {

// Abstract stream decoder: compressed or raw bytes in, interleaved 16-bit
// PCM (native byte order) out, through the same feed/drain shape regardless
// of the stream format. The pipeline's contract:
//   - output frames are `sampleRate() Hz / channels() ch`, described by
//     format() — the stream adopts it whenever it changes;
//   - drain() returns the number of SAMPLES written (0 = idle); a drain
//     returning 0 while hasError() is sticky aborts the stream;
//   - finish() signals end of input so tail frames flush (MP3); the
//     default is a no-op for formats with no decoder tail.
// The base also carries the two pipeline-cadence pieces every decoder shares
// and neither is format-specific: the 1152-frame chunk buffer behind
// nextChunk() and the PCM source-rate regulator (regulateRate()).
class Decoder {
public:
    virtual ~Decoder() = default;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    virtual void feed(std::span<const std::byte> data) = 0;
    virtual size_t drain(std::span<int16_t> out) = 0;
    virtual void finish() {}
    virtual uint32_t sampleRate() const = 0;
    virtual int channels() const = 0;
    virtual size_t pendingBytes() const = 0;
    virtual bool valid() const = 0;
    virtual bool hasError() const = 0;
    virtual std::string_view name() const = 0;

    // The decoder's output format, falling back to the input format until the
    // decoder is valid (an MP3 has no rate before its first frame).
    PcmFormat format() const;

    // Next decoded chunk in native s16, drawn through an internal 1152-frame
    // buffer; empty when the decoder is drained. The span is valid until the
    // next nextChunk()/feed() call. Callers that need raw drain() control can
    // still use drain() directly.
    std::span<const int16_t> nextChunk();

    // PCM source-rate regulation, driven once per telemetry window with the
    // source bytes received over it and the current output-ring occupancy.
    // Returns the applied source rate (0 = pass-through). No-op for decoders
    // without an active rate stage.
    double regulateRate(uint64_t receivedBytes, size_t queued, uint64_t windowMs);
    // True while the source-rate stage is stretching/shrinking (the emitted
    // timeline then runs ahead of the source; the pacer stands down).
    bool regulating() const { return pcmAppliedRate_ != 0.0; }

    // Tell the decoder the measured real-time input rate (frames/s) so it can
    // regulate its output to the pipeline's target clock. No-op for decoders
    // without a rate stage.
    virtual void setSourceRate(double framesPerSecond) { (void)framesPerSecond; }

    // Factory for the supported stream formats; nullptr for others (the
    // strm guard already rejects them, so this is a closed-world helper).
    // `outputRate` is the pipeline's target output clock (0 = no rate
    // regulation; the AirPlay pipeline passes 44100).
    static std::unique_ptr<Decoder> create(StreamFormat format, const PcmFormat& in,
                                           uint32_t outputRate);

protected:
    explicit Decoder(const PcmFormat& input);
    Decoder(Decoder&&) = default;
    Decoder& operator=(Decoder&&) = delete;

    // The decoder's own output format (pre-fallback); subclasses implement it.
    virtual PcmFormat decodedFormat() const = 0;
    // True for decoders with an active source-rate stage (PCM with a target).
    virtual bool regulatesRate() const { return false; }

    const PcmFormat& inputFormat() const { return input_; }

private:
    std::array<int16_t, 1152 * 2> chunk_{};
    PcmFormat input_;             // fallback format until the decoder is valid
    size_t inputFrameBytes_ = 4;  // raw input frame size (rate regulator)
    // PCM source-rate regulation.
    uint64_t pcmWindowReceivedBytes_ = 0;
    double pcmAppliedRate_ = 0.0;
};

}  // namespace squeeze2raop2
