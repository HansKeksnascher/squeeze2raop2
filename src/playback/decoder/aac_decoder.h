#pragma once

#include "playback/decoder/buffered_decoder.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace squeeze2raop2 {

class Mp4AacDemuxer;

// Native AAC decoder backed by the vendored libxaac (Apache-2.0). Handles the
// two transports LMS sends under the 'aac' capability:
//   - ADTS ('2'): the elementary stream is fed to libxaac's ADTS parser.
//   - MP4  ('5'): the MP4/M4A container is demuxed (Mp4AacDemuxer) into
//     synthesized ADTS frames, then decoded through the same path.
// Output is always interleaved s16 stereo at the stream's decoded rate.
class AacDecoder final : public BufferedDecoder {
public:
    // `containerCode` is the LMS pcm_sample_size byte: '2' = ADTS, '5' = MP4.
    AacDecoder(const PcmFormat& in, uint8_t containerCode);
    ~AacDecoder() override;
    AacDecoder(const AacDecoder&) = delete;
    AacDecoder& operator=(const AacDecoder&) = delete;

    void feed(std::span<const std::byte> data) override;
    // Signal end of input so tail frames flush.
    void finish() override;

    uint32_t sampleRate() const override { return sampleRate_; }
    int channels() const override { return channels_; }
    size_t pendingBytes() const override;
    bool valid() const override { return sampleRate_ != 0; }
    std::string_view name() const override { return "aac"; }

protected:
    PcmFormat decodedFormat() const override;

private:
    // Opaque libxaac state (API object + memory/table allocations); defined in
    // the .cpp so the third-party headers stay out of this header.
    struct Xaac;
    // Allocate and initialise libxaac's API object, tables and memory.
    static bool initXaac(Xaac& x);

    // Lazily initialise the libxaac decoder. Returns true once initialised;
    // false when more input is needed or on failure.
    bool ensureInit();
    // Decode as many frames as the buffered input allows into pcm_.
    void decodeMore();
    // Pull the MP4 demuxer's output into buffer_, failing on demux errors.
    void drainDemuxed();
    // Copy pending compressed bytes into the library's input buffer; returns
    // the number copied.
    size_t fillInput();
    // End of the last complete ADTS frame in buffer_[consumed_..limit).
    size_t frameEnd(size_t limit) const;
    // Append `bytes` of s16 from the library output buffer to pcm_.
    void appendPcm(size_t bytes);

    std::unique_ptr<Xaac> xaac_;
    std::unique_ptr<Mp4AacDemuxer> mp4_;
    uint32_t sampleRate_ = 0;
    int channels_ = 2;
    bool initDone_ = false;
    bool inputOver_ = false;
};

}  // namespace squeeze2raop2
