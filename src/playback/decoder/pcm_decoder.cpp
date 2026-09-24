#include "playback/decoder/pcm_decoder.h"

#include "common/byte_order.h"
#include "common/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace squeeze2raop2 {

namespace {

// Reference pcm.c MIN_READ: enough buffered bytes before the header
// decision (WAV needs 44, AIFF's chunk walk can need a bit more).
constexpr size_t kHeaderProbeBytes = 512;
// Mirror feedStream's kPcmChunk cadence: drains come in 1152-frame chunks.
constexpr size_t kChunkFrames = 1152;
// Bound the normalized-stage working set (~186 ms at 44.1 kHz stereo),
// mirroring the sender resampler's inBuf_ sizing.
constexpr size_t kStageMaxFrames = 8192;

}  // namespace

PcmDecoder::PcmDecoder(const PcmFormat& in, uint32_t outputRate)
    : Decoder(in), outRate_(outputRate) {
    // Source layouts the reference converts: 16-bit mono/stereo and 24-bit
    // stereo (pcm.c's conversion set). The container header may override
    // rate/channels/size; endian is decided per conversion. Output is
    // always interleaved s16 stereo.
    srcBits_ = in.bitsPerSample;
    srcChannels_ = in.channels;
    srcBigEndian_ = in.bigEndian;
    srcRate_ = in.sampleRate;
    if ((srcBits_ != 16 && srcBits_ != 24) || (srcBits_ == 24 && srcChannels_ != 2) ||
        (srcChannels_ != 1 && srcChannels_ != 2)) {
        log::error(log::Area::Dec, "unsupported {} bit / {} ch stream", srcBits_, srcChannels_);
        failed_ = true;
    }
    bytesPerFrame_ = size_t(srcChannels_) * (srcBits_ / 8);
    fmt_ = PcmFormat{.sampleRate = outRate_ ? outRate_ : srcRate_,
                     .bitsPerSample = 16,
                     .channels = 2,
                     .bigEndian = false};
    rateStep_ = 1.0;
}

std::optional<size_t> PcmDecoder::checkHeader() {
    const std::byte* p = buf_.data();
    const size_t have = buf_.size();
    if (have < kHeaderProbeBytes) return std::nullopt;  // keep buffering

    // RIFF/WAVE: fmt chunk carries the real format; skip RIFF hdr + fmt
    // chunk + the 'data' chunk header (pcm.c's skip arithmetic).
    if (have >= 44 && fourccIs(p, "RIFF") && fourccIs(p + 8, "WAVE") && fourccIs(p + 12, "fmt ")) {
        const uint32_t fmtSize = readInt<Endian::Little, uint32_t>(p + 16);
        srcChannels_ = static_cast<uint8_t>(readInt<Endian::Little, uint16_t>(p + 22));
        srcRate_ = readInt<Endian::Little, uint32_t>(p + 24);
        srcBits_ = static_cast<uint8_t>(readInt<Endian::Little, uint16_t>(p + 34));
        srcBigEndian_ = false;
        log::info(log::Area::Dec, "WAV header, {} bit / {} Hz / {} ch", srcBits_, srcRate_,
                  srcChannels_);
        if ((srcBits_ != 16 && srcBits_ != 24) || (srcBits_ == 24 && srcChannels_ != 2) ||
            (srcChannels_ != 1 && srcChannels_ != 2)) {
            log::error(log::Area::Dec, "unsupported WAV {} bit / {} ch", srcBits_, srcChannels_);
            failed_ = true;
            return size_t{0};
        }
        bytesPerFrame_ = size_t(srcChannels_) * (srcBits_ / 8);
        fmt_.sampleRate = outRate_ ? outRate_ : srcRate_;
        // pcm.c's skip arithmetic: RIFF hdr + fmt chunk + data chunk hdr
        // (28 + fmtSize == 44 for the standard 16-byte fmt chunk).
        return size_t{28} + fmtSize;
    }

    // FORM/AIFF|AIFC: walk chunks, COMM carries the format (big-endian),
    // SSND starts the sound data (its own 8-byte header + data offset).
    if (have >= 64 && fourccIs(p, "FORM") && (fourccIs(p + 8, "AIFF") || fourccIs(p + 8, "AIFC"))) {
        size_t off = 12;
        srcBigEndian_ = true;
        while (off + 8 <= have) {
            // COMM body is read through the rate u32 at +18..+21, so the chunk
            // needs 22 bytes, not 18 (the exponent pair at +16/+17 alone would
            // only need 18).
            if (fourccIs(p + off, "COMM") && off + 22 <= have) {
                srcChannels_ = static_cast<uint8_t>(readInt<Endian::Big, uint16_t>(p + off + 8));
                srcBits_ = static_cast<uint8_t>(readInt<Endian::Big, uint16_t>(p + off + 14));
                // IEEE 80-bit extended rate, same simplification as pcm.c:
                // high 32 bits of the mantissa with the exponent shift.
                int exponent = ((std::to_integer<uint8_t>(p[off + 16]) & 0x7F) << 8 |
                                std::to_integer<uint8_t>(p[off + 17])) -
                               16383 - 31;
                uint32_t rate = readInt<Endian::Big, uint32_t>(p + off + 18);
                while (exponent < 0) {
                    rate >>= 1;
                    ++exponent;
                }
                while (exponent > 0) {
                    rate <<= 1;
                    --exponent;
                }
                srcRate_ = rate;
                log::info(log::Area::Dec, "AIFF header, {} bit / {} Hz / {} ch", srcBits_, srcRate_,
                          srcChannels_);
            }
            if (fourccIs(p + off, "SSND")) {
                // The chunk header (tag + length) fits by the loop guard, but
                // the 4-byte sound-data offset at +8 needs 12 bytes. Reading it
                // with fewer would run past the probe buffer.
                if (off + 12 > have) {
                    log::error(log::Area::Dec, "truncated AIFF SSND header");
                    failed_ = true;
                    return size_t{0};
                }
                const uint32_t sndOffset = readInt<Endian::Big, uint32_t>(p + off + 8);
                const size_t skip = off + 8 + sndOffset;
                if ((srcBits_ != 16 && srcBits_ != 24) || (srcBits_ == 24 && srcChannels_ != 2) ||
                    (srcChannels_ != 1 && srcChannels_ != 2)) {
                    log::error(log::Area::Dec, "unsupported AIFF {} bit / {} ch", srcBits_,
                               srcChannels_);
                    failed_ = true;
                } else {
                    bytesPerFrame_ = size_t(srcChannels_) * (srcBits_ / 8);
                }
                fmt_.sampleRate = outRate_ ? outRate_ : srcRate_;
                return skip;
            }
            const uint32_t len = readInt<Endian::Big, uint32_t>(p + off + 4);
            off += size_t(len) + 8;
        }
        log::error(log::Area::Dec, "AIFF header without SSND chunk");
        failed_ = true;
        return size_t{0};
    }

    // No container: raw samples per the strm params (the common radio case).
    log::info(log::Area::Dec, "raw pcm, {} bit / {} Hz / {} ch / {}, output {} Hz", srcBits_,
              srcRate_, srcChannels_, srcBigEndian_ ? "big-endian" : "little-endian",
              fmt_.sampleRate);
    return size_t{0};
}

void PcmDecoder::feed(std::span<const std::byte> data) {
    if (failed_ || data.empty()) return;
    buf_.insert(buf_.end(), data.begin(), data.end());
}

// Normalize buffered raw bytes into interleaved s16 stereo frames in
// stage_ (at the source rate), pcm.c's conversion set.
size_t PcmDecoder::normalizeMore() {
    const size_t have = buf_.size();
    const size_t inFrames = have / bytesPerFrame_;
    const size_t stageFrames = stage_.size() / 2;
    size_t frames = std::min(inFrames, kStageMaxFrames - stageFrames);
    if (!frames) return 0;

    const std::byte* ip = buf_.data();
    stage_.resize(stage_.size() + frames * 2);
    int16_t* op = stage_.data() + stageFrames * 2;

    if (srcBits_ == 16 && srcChannels_ == 2) {
        if (!srcBigEndian_) {
            std::memcpy(op, ip, frames * 4);
        } else {
            for (size_t n = frames * 2; n--;) {
                *op++ = int16_t((std::to_integer<uint8_t>(ip[0]) << 8) |
                                std::to_integer<uint8_t>(ip[1]));
                ip += 2;
            }
        }
    } else if (srcBits_ == 16 && srcChannels_ == 1) {
        for (size_t n = frames; n--;) {
            int16_t s;
            if (!srcBigEndian_) {
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
    } else if (srcBits_ == 24 && srcChannels_ == 2) {
        // Take the top 16 bits of each 24-bit sample (pcm.c).
        for (size_t n = frames * 2; n--;) {
            if (!srcBigEndian_) {
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
    buf_.erase(buf_.begin(), buf_.begin() + std::ptrdiff_t(frames * bytesPerFrame_));
    return frames;
}

void PcmDecoder::setSourceRate(double framesPerSecond) {
    if (failed_ || outRate_ == 0) return;
    const double lo = 0.9 * outRate_, hi = 1.1 * outRate_;
    if (!(framesPerSecond >= lo && framesPerSecond <= hi)) return;  // hold
    // Defense in depth: never let a measurement glitch push the pitch
    // beyond ±1% (the real-world deficits this regulates are ≲0.3%).
    const double step = std::clamp(framesPerSecond / outRate_, 0.99, 1.01);
    if (std::abs(step - rateStep_) < 1e-6) return;
    const bool wasEngaged = rateEngaged_;
    rateStep_ = step;
    rateEngaged_ = step != 1.0;
    log::info(log::Area::Dec, "source rate {} fps -> step {:.6f} ({})", framesPerSecond, rateStep_,
              rateEngaged_ ? "resampling" : "pass-through");
    if (wasEngaged && !rateEngaged_) resetRateStage();
}

// Emit up to out.size()/2 frames from stage_ at the target rate. Bypass
// (step exactly 1) is a bit-exact move; the engaged stage is the same
// two-point lerp the sender's resampler uses.
size_t PcmDecoder::rateStageEmit(std::span<int16_t> out) {
    const size_t want = std::min(out.size() / 2, kChunkFrames);
    if (!want) return 0;
    const size_t stageFrames = stage_.size() / 2;

    if (!rateEngaged_) {
        const size_t frames = std::min({stageFrames, want, kChunkFrames});
        if (!frames) return 0;
        std::memcpy(out.data(), stage_.data(), frames * 4);
        stage_.erase(stage_.begin(), stage_.begin() + std::ptrdiff_t(frames * 2));
        return frames * 2;
    }

    size_t produced = 0;
    while (produced < want) {
        const size_t i0 = size_t(stagePhase_);
        if (i0 + 1 >= stageFrames) break;  // need i0 and i0+1, starved
        const double frac = stagePhase_ - double(i0);
        const int16_t* a = stage_.data() + i0 * 2;
        const int16_t* b = a + 2;
        out[produced * 2 + 0] = int16_t(a[0] + (b[0] - a[0]) * frac);
        out[produced * 2 + 1] = int16_t(a[1] + (b[1] - a[1]) * frac);
        stagePhase_ += rateStep_;
        ++produced;
    }
    // Compact: drop fully consumed frames, keep the lerp's left neighbour.
    const size_t keepFrom = size_t(stagePhase_);
    if (keepFrom > 0) {
        const size_t drop = std::min(keepFrom, stageFrames);
        stage_.erase(stage_.begin(), stage_.begin() + std::ptrdiff_t(drop * 2));
        stagePhase_ -= double(drop);
    }
    return produced * 2;
}

void PcmDecoder::resetRateStage() {
    // Disengaging: the stage buffer holds source-rate frames; flush them
    // bit-exact (they were produced for the target clock only approximately).
    stagePhase_ = 0.0;
    rateStep_ = 1.0;
    rateEngaged_ = false;
}

size_t PcmDecoder::drain(std::span<int16_t> out) {
    if (failed_ || out.size() < 2) return 0;

    if (!headerDone_) {
        auto skip = checkHeader();
        if (!skip) return 0;  // still probing
        headerBytes_ = *skip;
        if (*skip > buf_.size()) {  // AIFF sound data starts beyond what we
            // have buffered (possible with a large SSND offset); fail closed.
            log::error(log::Area::Dec, "SSND offset {} beyond header buffer", *skip);
            failed_ = true;
            return 0;
        }
        buf_.erase(buf_.begin(), buf_.begin() + std::ptrdiff_t(*skip));
        headerDone_ = true;
    }

    size_t totalSamples = 0;
    for (;;) {
        const size_t got = rateStageEmit(out.subspan(totalSamples));
        totalSamples += got;
        if (totalSamples >= out.size()) break;        // caller buffer full
        if (got == 0 && normalizeMore() == 0) break;  // starved
    }
    return totalSamples;
}

}  // namespace squeeze2raop2