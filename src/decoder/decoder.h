#pragma once

#include "slimproto.h"

#include <cstddef>
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
class Decoder {
public:
    Decoder() = default;
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
    virtual PcmFormat format() const = 0;
    virtual std::string_view name() const = 0;

    // Factory for the supported stream formats; nullptr for others (the
    // strm guard already rejects them, so this is a closed-world helper).
    static std::unique_ptr<Decoder> create(StreamFormat format,
                                           const PcmFormat& in);

protected:
    Decoder(Decoder&&) = default;
    Decoder& operator=(Decoder&&) = delete;
};

} // namespace squeeze2raop2