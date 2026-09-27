#pragma once

#include "lms/slimproto_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace squeeze2raop2 {

// Abstract stream decoder: compressed or raw bytes in, interleaved 16-bit
// PCM (native byte order) out, through the same feed/drain shape regardless
// of the stream format. The pipeline's contract:
//   - output frames are `sampleRate() Hz / channels() ch`, described by
//     format() — the stream adopts it whenever it changes;
//   - drain() returns the number of SAMPLES written (0 = idle); a drain
//     returning 0 while hasError() is sticky aborts the stream;
//   - finish() signals end of input so tail frames flush (e.g. MP3); the
//     default is a no-op for formats with no decoder tail.
// The base also carries the 1152-frame chunk buffer behind nextChunk(); the
// rate conversion to the AirPlay output clock lives in playback/resampler.h.
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

    // Factory for the supported stream formats; nullptr for others (the
    // strm guard already rejects them, so this is a closed-world helper).
    // Decoders emit at their native rate; the pipeline resamples to the
    // AirPlay output clock. `containerCode` is the LMS pcm_sample_size byte
    // for AAC transports ('2' ADTS, '5' MP4) and is ignored by every other
    // format.
    static std::unique_ptr<Decoder> create(StreamFormat format, const PcmFormat& in,
                                           uint8_t containerCode = 0);

protected:
    explicit Decoder(const PcmFormat& input);
    Decoder(Decoder&&) = default;
    Decoder& operator=(Decoder&&) = delete;

    // The decoder's own output format (pre-fallback); subclasses implement it.
    virtual PcmFormat decodedFormat() const = 0;

    // Interleaved s16 stereo at the decoder's decoded rate/channels — the
    // output shape every native decoder produces.
    PcmFormat s16StereoFormat() const {
        return PcmFormat{.sampleRate = sampleRate(),
                         .bitsPerSample = kDefaultBitsPerSample,
                         .channels = static_cast<uint8_t>(channels()),
                         .bigEndian = false};
    }

    static size_t takeSamples(std::span<int16_t> out, std::vector<int16_t>& pcm);
    // Drop the consumed prefix once kCompactThreshold bytes accumulate.
    static void compactConsumed(std::vector<std::byte>& buffer, size_t& consumed);
    static constexpr size_t kCompactThreshold = 1 << 16;

    const PcmFormat& inputFormat() const { return input_; }

private:
    std::array<int16_t, 1152 * 2> chunk_{};
    PcmFormat input_;  // fallback format until the decoder is valid
    size_t inputFrameBytes_ = 4;
};

// Codec HELO capability tokens (the LMS caps vocabulary).
constexpr const char* kCodecCapPcm = "pcm";
constexpr const char* kCodecCapMp3 = "mp3";
constexpr const char* kCodecCapAac = "aac";
constexpr const char* kCodecCapOgg = "ogg";
constexpr const char* kCodecCapOpus = "ops";

// One decodable stream format: its LMS HELO capability token and the factory
// that builds its decoder for Decoder::create(). The list in decoder.cpp is
// the single place a codec is enabled/disabled (build-gated): the factory, the
// advertised HELO caps and the strm-format guard all read it, so they cannot
// drift apart. `containerCode` is only meaningful to AAC (see create()).
struct CodecInfo {
    StreamFormat format;
    const char* capToken;
    std::unique_ptr<Decoder> (*create)(const PcmFormat& in, uint8_t containerCode);
};

// Formats this build can decode, in advertised order: pcm, then any enabled
// MP3/AAC/Ogg/Opus. Always contains at least Pcm.
std::span<const CodecInfo> supportedCodecs();

bool supportsFormat(StreamFormat format);

}  // namespace squeeze2raop2
