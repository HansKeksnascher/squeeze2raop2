#pragma once

#include "decoder/decoder.h"

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
    explicit PcmDecoder(const PcmFormat& in);

    PcmDecoder(const PcmDecoder&) = delete;
    PcmDecoder& operator=(const PcmDecoder&) = delete;

    void feed(std::span<const std::byte> data) override;
    // Emits at most out.size()/2 interleaved s16 stereo frames (native
    // byte order). Returns the number of SAMPLES written.
    size_t drain(std::span<int16_t> out) override;

    uint32_t sampleRate() const override { return fmt_.sampleRate; }
    int channels() const override { return fmt_.channels; }
    size_t pendingBytes() const override { return buf_.size(); }
    bool valid() const override { return !failed_; }
    bool hasError() const override { return failed_; }
    // The effective format (container header may override the strm params).
    PcmFormat format() const override { return fmt_; }
    std::string_view name() const override { return "pcm"; }

private:
    // Returns bytes to skip at the stream head, adopting the container's
    // format; 0 = no container (raw per strm params). Requires enough
    // buffered bytes to decide, otherwise returns nullopt ("not yet").
    std::optional<size_t> checkHeader();
    size_t convertInto(std::span<int16_t> out);

    PcmFormat fmt_;
    unsigned bytesPerFrame_ = 4;
    std::vector<std::byte> buf_;
    size_t headerBytes_ = 0;   // container bytes skipped so far
    bool headerDone_ = false;
    bool failed_ = false;
};

} // namespace squeeze2raop2