#include "decoder/pcm_decoder.h"

#include "log.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace squeeze2raop2 {

namespace {

// Reference pcm.c MIN_READ: enough buffered bytes before the header
// decision (WAV needs 44, AIFF's chunk walk can need a bit more).
constexpr size_t kHeaderProbeBytes = 512;
// Mirror feedMp3's kPcmChunk cadence: drains come in 1152-frame chunks.
constexpr size_t kChunkFrames = 1152;

uint16_t rd16le(const std::byte* p) {
    return uint16_t(std::to_integer<uint8_t>(p[0]) |
                    (std::to_integer<uint8_t>(p[1]) << 8));
}
uint32_t rd32le(const std::byte* p) {
    return uint32_t(rd16le(p)) | (uint32_t(rd16le(p + 2)) << 16);
}
uint16_t rd16be(const std::byte* p) {
    return uint16_t((std::to_integer<uint8_t>(p[0]) << 8) |
                    std::to_integer<uint8_t>(p[1]));
}
uint32_t rd32be(const std::byte* p) {
    return (uint32_t(rd16be(p)) << 16) | rd16be(p + 2);
}
bool tagIs(const std::byte* p, const char (&tag)[5]) {
    return std::memcmp(p, tag, 4) == 0;
}

} // namespace

PcmDecoder::PcmDecoder(const PcmFormat& in) : fmt_(in) {
    // Only layouts the reference converts: 16-bit, 1 or 2 channels. The
    // container header may override rate/channels/size; endian is decided
    // per conversion.
    if (fmt_.bitsPerSample != 16 || (fmt_.channels != 1 && fmt_.channels != 2)) {
        log::error("pcm codec: unsupported {} bit / {} ch stream",
                   fmt_.bitsPerSample, fmt_.channels);
        failed_ = true;
    }
    bytesPerFrame_ = fmt_.channels == 1 ? 2 : 4;
}

std::optional<size_t> PcmDecoder::checkHeader() {
    const auto* p = reinterpret_cast<const std::byte*>(buf_.data());
    const size_t have = buf_.size();
    if (have < kHeaderProbeBytes) return std::nullopt;   // keep buffering

    // RIFF/WAVE: fmt chunk carries the real format; skip RIFF hdr + fmt
    // chunk + the 'data' chunk header (pcm.c's skip arithmetic).
    if (have >= 44 && tagIs(p, "RIFF") && tagIs(p + 8, "WAVE") &&
        tagIs(p + 12, "fmt ")) {
        const uint32_t fmtSize = rd32le(p + 16);
        fmt_.channels = static_cast<uint8_t>(rd16le(p + 22));
        fmt_.sampleRate = rd32le(p + 24);
        fmt_.bitsPerSample = static_cast<uint8_t>(rd16le(p + 34));
        fmt_.bigEndian = false;
        log::info("pcm codec: WAV header, {} bit / {} Hz / {} ch",
                  fmt_.bitsPerSample, fmt_.sampleRate, fmt_.channels);
        if (fmt_.bitsPerSample != 16 || (fmt_.channels != 1 && fmt_.channels != 2)) {
            log::error("pcm codec: unsupported WAV {} bit / {} ch",
                       fmt_.bitsPerSample, fmt_.channels);
            failed_ = true;
            return size_t{0};
        }
        bytesPerFrame_ = fmt_.channels == 1 ? 2 : 4;
        // pcm.c's skip arithmetic: RIFF hdr + fmt chunk + data chunk hdr
        // (28 + fmtSize == 44 for the standard 16-byte fmt chunk).
        return size_t{28} + fmtSize;
    }

    // FORM/AIFF|AIFC: walk chunks, COMM carries the format (big-endian),
    // SSND starts the sound data (its own 8-byte header + data offset).
    if (have >= 64 && tagIs(p, "FORM") &&
        (tagIs(p + 8, "AIFF") || tagIs(p + 8, "AIFC"))) {
        size_t off = 12;
        fmt_.bigEndian = true;
        while (off + 8 <= have) {
            if (tagIs(p + off, "COMM") && off + 18 <= have) {
                fmt_.channels = static_cast<uint8_t>(rd16be(p + off + 8));
                fmt_.bitsPerSample = static_cast<uint8_t>(rd16be(p + off + 14));
                // IEEE 80-bit extended rate, same simplification as pcm.c:
                // high 32 bits of the mantissa with the exponent shift.
                int exponent = ((std::to_integer<uint8_t>(p[off + 16]) & 0x7F) << 8 |
                                std::to_integer<uint8_t>(p[off + 17])) -
                               16383 - 31;
                uint32_t rate = rd32be(p + off + 18);
                while (exponent < 0) { rate >>= 1; ++exponent; }
                while (exponent > 0) { rate <<= 1; --exponent; }
                fmt_.sampleRate = rate;
                log::info("pcm codec: AIFF header, {} bit / {} Hz / {} ch",
                          fmt_.bitsPerSample, fmt_.sampleRate, fmt_.channels);
            }
            if (tagIs(p + off, "SSND")) {
                const uint32_t sndOffset = rd32be(p + off + 8);
                const size_t skip = off + 8 + sndOffset;
                if (fmt_.bitsPerSample != 16 ||
                    (fmt_.channels != 1 && fmt_.channels != 2)) {
                    log::error("pcm codec: unsupported AIFF {} bit / {} ch",
                               fmt_.bitsPerSample, fmt_.channels);
                    failed_ = true;
                } else {
                    bytesPerFrame_ = fmt_.channels == 1 ? 2 : 4;
                }
                return skip;
            }
            const uint32_t len = rd32be(p + off + 4);
            off += size_t(len) + 8;
        }
        log::error("pcm codec: AIFF header without SSND chunk");
        failed_ = true;
        return size_t{0};
    }

    // No container: raw samples per the strm params (the common radio case).
    log::info("pcm codec: raw pcm, {} bit / {} Hz / {} ch / {}",
              fmt_.bitsPerSample, fmt_.sampleRate, fmt_.channels,
              fmt_.bigEndian ? "big-endian" : "little-endian");
    return size_t{0};
}

void PcmDecoder::feed(std::span<const std::byte> data) {
    if (failed_ || data.empty()) return;
    buf_.insert(buf_.end(), data.begin(), data.end());
}

size_t PcmDecoder::convertInto(std::span<int16_t> out) {
    const size_t have = buf_.size();
    const size_t inFrames = have / bytesPerFrame_;
    size_t frames = std::min({inFrames, out.size() / 2, kChunkFrames});
    if (!frames) return 0;

    const auto* ip = reinterpret_cast<const std::byte*>(buf_.data());
    int16_t* op = out.data();
    const size_t outFrames = frames;

    if (fmt_.bitsPerSample == 16 && fmt_.channels == 2) {
        if (!fmt_.bigEndian) {
            std::memcpy(op, ip, frames * 4);
        } else {
            for (size_t n = frames * 2; n--;) {
                *op++ = int16_t((std::to_integer<uint8_t>(ip[0]) << 8) |
                                std::to_integer<uint8_t>(ip[1]));
                ip += 2;
            }
        }
    } else if (fmt_.bitsPerSample == 16 && fmt_.channels == 1) {
        for (size_t n = frames; n--;) {
            int16_t s;
            if (!fmt_.bigEndian) {
                s = int16_t(std::to_integer<uint8_t>(ip[0]) |
                            (std::to_integer<uint8_t>(ip[1]) << 8));
            } else {
                s = int16_t((std::to_integer<uint8_t>(ip[0]) << 8) |
                            std::to_integer<uint8_t>(ip[1]));
            }
            *op++ = s;
            *op++ = s;
            ip += 2;
        }
    } else if (fmt_.bitsPerSample == 24 && fmt_.channels == 2) {
        // Take the top 16 bits of each 24-bit sample (pcm.c).
        for (size_t n = frames * 2; n--;) {
            if (!fmt_.bigEndian) {
                *op++ = int16_t((std::to_integer<uint8_t>(ip[1]) << 8) |
                                std::to_integer<uint8_t>(ip[2]));
                ip += 3;
            } else {
                *op++ = int16_t((std::to_integer<uint8_t>(ip[0]) << 8) |
                                std::to_integer<uint8_t>(ip[1]));
                ip += 3;
            }
        }
    }

    buf_.erase(buf_.begin(),
               buf_.begin() + std::ptrdiff_t(frames * bytesPerFrame_));
    return outFrames * 2;
}

size_t PcmDecoder::drain(std::span<int16_t> out) {
    if (failed_ || out.size() < 2) return 0;

    if (!headerDone_) {
        auto skip = checkHeader();
        if (!skip) return 0;   // still probing
        headerBytes_ = *skip;
        if (*skip > buf_.size()) {   // AIFF sound data starts beyond what we
            // have buffered (possible with a large SSND offset); fail closed.
            log::error("pcm codec: SSND offset {} beyond header buffer",
                       *skip);
            failed_ = true;
            return 0;
        }
        buf_.erase(buf_.begin(), buf_.begin() + std::ptrdiff_t(*skip));
        headerDone_ = true;
    }

    return convertInto(out);
}

} // namespace squeeze2raop2
