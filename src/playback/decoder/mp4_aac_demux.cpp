// squeeze2raop2 - minimal streaming MP4/M4A -> ADTS remuxer for AAC.
//
// Deliberately small: only the boxes needed to recover the AudioSpecificConfig
// (moov > trak > mdia > minf > stbl > stsd > mp4a > esds) and the per-sample
// sizes (stsz), then a sequential rewrite of the mdat payload into ADTS frames.
// Seeking, gapless metadata and non-faststart (trailing moov) files are out of
// scope.

#include "playback/decoder/mp4_aac_demux.h"

#include "common/log.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace squeeze2raop2 {

namespace {

uint8_t byteAt(const std::byte* p) { return std::to_integer<uint8_t>(p[0]); }

uint16_t rd16(const std::byte* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(byteAt(p)) << 8) | byteAt(p + 1));
}

uint32_t rd32(const std::byte* p) {
    return (static_cast<uint32_t>(byteAt(p)) << 24) | (static_cast<uint32_t>(byteAt(p + 1)) << 16) |
           (static_cast<uint32_t>(byteAt(p + 2)) << 8) | static_cast<uint32_t>(byteAt(p + 3));
}

uint64_t rd64(const std::byte* p) { return (static_cast<uint64_t>(rd32(p)) << 32) | rd32(p + 4); }

bool isType(const std::byte* p, const char (&tag)[5]) { return std::memcmp(p, tag, 4) == 0; }

// AAC sampling-frequency index for a rate (ISO/IEC 14496-3 table 1.18).
uint8_t rateIndex(uint32_t rate) {
    static constexpr uint32_t kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                          22050, 16000, 12000, 11025, 8000,  7350};
    for (uint8_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); ++i) {
        if (kRates[i] == rate) return i;
    }
    return 4;  // 44100
}

}  // namespace

Mp4AacDemuxer::Mp4AacDemuxer() = default;

void Mp4AacDemuxer::feed(std::span<const std::byte> data) {
    if (failed_ || done_) return;
    buf_.insert(buf_.end(), data.begin(), data.end());
    pump();
    compact();
}

void Mp4AacDemuxer::finish() {
    finished_ = true;
    if (failed_ || done_) return;
    pump();
    compact();
}

void Mp4AacDemuxer::drain(std::vector<std::byte>& out) {
    out.insert(out.end(), out_.begin(), out_.end());
    out_.clear();
}

void Mp4AacDemuxer::compact() {
    if (pos_ == 0) return;
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
    pos_ = 0;
}

void Mp4AacDemuxer::pump() {
    for (;;) {
        if (failed_ || done_) return;

        if (skipRemaining_) {
            const size_t avail = buf_.size() - pos_;
            const size_t take = static_cast<size_t>(std::min<uint64_t>(skipRemaining_, avail));
            pos_ += take;
            skipRemaining_ -= take;
            if (skipRemaining_) return;
            continue;
        }

        if (inMdat_) {
            if (mdatRemaining_ == 0) {
                inMdat_ = false;
                continue;
            }
            if (nextSample_ >= sampleSizes_.size()) {
                // No more samples: drain the rest of mdat.
                const size_t avail = buf_.size() - pos_;
                const size_t take = static_cast<size_t>(std::min<uint64_t>(mdatRemaining_, avail));
                pos_ += take;
                mdatRemaining_ -= take;
                if (mdatRemaining_ == 0) {
                    inMdat_ = false;
                    done_ = true;
                }
                if (take == 0) return;
                continue;
            }
            const uint32_t want = sampleSizes_[nextSample_];
            if (want == 0 || want > (1u << 20)) {
                failed_ = true;
                return;
            }
            const size_t need = want - sample_.size();
            const size_t avail = buf_.size() - pos_;
            const size_t take = std::min(need, avail);
            if (take) {
                sample_.insert(sample_.end(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_),
                               buf_.begin() + static_cast<std::ptrdiff_t>(pos_ + take));
                pos_ += take;
                mdatRemaining_ -= take;
            }
            if (sample_.size() == want) {
                synthAdts(sample_, out_);
                sample_.clear();
                ++nextSample_;
            }
            if (mdatRemaining_ == 0) inMdat_ = false;
            if (take == 0) return;  // wait for more input
            continue;
        }

        if (buf_.size() - pos_ < 8) return;
        const std::byte* hdr = buf_.data() + pos_;
        uint64_t size = rd32(hdr);
        const std::byte* type = hdr + 4;
        size_t headerLen = 8;
        if (size == 1) {
            if (buf_.size() - pos_ < 16) return;
            size = rd64(hdr + 8);
            headerLen = 16;
        } else if (size == 0) {
            size = buf_.size() - pos_;  // extends to the end of what we have
        }
        if (size < headerLen) {
            failed_ = true;
            return;
        }

        if (isType(type, "mdat")) {
            if (!ready_) {
                failed_ = true;  // moov must precede mdat
                return;
            }
            pos_ += headerLen;
            mdatRemaining_ = (size == 0) ? std::numeric_limits<uint64_t>::max() : size - headerLen;
            inMdat_ = true;
            continue;
        }
        if (isType(type, "moov")) {
            const size_t avail = buf_.size() - pos_;
            if (size > avail) return;  // wait for the whole moov
            parseMoov(hdr + headerLen, static_cast<size_t>(size) - headerLen);
            if (failed_) return;
            pos_ += static_cast<size_t>(size);
            ready_ = haveConfig_ && !sampleSizes_.empty();
            if (!ready_) {
                failed_ = true;
                return;
            }
            continue;
        }

        // Skip ftyp/free/wide/uuid/... (consume what we have and wait).
        const size_t avail = buf_.size() - pos_;
        if (size > avail) {
            pos_ += avail;
            skipRemaining_ = size - avail;
            compact();
            return;
        }
        pos_ += static_cast<size_t>(size);
    }
}

void Mp4AacDemuxer::parseMoov(const std::byte* p, size_t n) {
    size_t off = 0;
    while (off + 8 <= n) {
        uint64_t size = rd32(p + off);
        const std::byte* t = p + off + 4;
        size_t hdr = 8;
        if (size == 1) {
            if (off + 16 > n) return;
            size = rd64(p + off + 8);
            hdr = 16;
        } else if (size == 0) {
            size = n - off;
        }
        if (size < hdr || off + size > n) return;
        if (isType(t, "trak")) parseTrak(p + off + hdr, static_cast<size_t>(size) - hdr);
        off += static_cast<size_t>(size);
    }
}

void Mp4AacDemuxer::parseTrak(const std::byte* p, size_t n) {
    trackAudio_ = false;
    parseBoxes(p, n);
}

void Mp4AacDemuxer::parseBoxes(const std::byte* p, size_t n) {
    size_t off = 0;
    while (off + 8 <= n) {
        uint64_t size = rd32(p + off);
        const std::byte* t = p + off + 4;
        size_t hdr = 8;
        if (size == 1) {
            if (off + 16 > n) return;
            size = rd64(p + off + 8);
            hdr = 16;
        } else if (size == 0) {
            size = n - off;
        }
        if (size < hdr || off + size > n) return;
        const std::byte* body = p + off + hdr;
        const size_t blen = static_cast<size_t>(size) - hdr;
        if (isType(t, "mdia") || isType(t, "minf") || isType(t, "stbl") || isType(t, "edts") ||
            isType(t, "udta")) {
            parseBoxes(body, blen);
        } else if (isType(t, "stsd")) {
            parseStsd(body, blen);
        } else if (isType(t, "stsz")) {
            parseStsz(body, blen);
        } else if (isType(t, "esds")) {
            parseEsds(body, blen);
        }
        off += static_cast<size_t>(size);
    }
}

void Mp4AacDemuxer::parseStsd(const std::byte* p, size_t n) {
    if (n < 8) return;
    size_t off = 8;  // version/flags + entry_count
    while (off + 8 <= n) {
        uint64_t size = rd32(p + off);
        const std::byte* fmt = p + off + 4;
        size_t hdr = 8;
        if (size == 1) {
            if (off + 16 > n) return;
            size = rd64(p + off + 8);
            hdr = 16;
        }
        if (size < hdr || off + size > n) return;
        if (isType(fmt, "mp4a")) {
            trackAudio_ = true;
            const std::byte* entry = p + off + hdr;
            const size_t elen = static_cast<size_t>(size) - hdr;
            // AudioSampleEntry fixed fields, then child boxes (esds).
            if (elen >= 28) {
                const uint16_t ch = rd16(entry + 16);
                const uint32_t sr = rd32(entry + 24) >> 16;
                if (ch >= 1 && ch <= 7 && !haveConfig_) channels_ = static_cast<uint8_t>(ch);
                if (sr && !haveConfig_) sampleRate_ = sr;
            }
            if (elen > 28) parseBoxes(entry + 28, elen - 28);
        }
        off += static_cast<size_t>(size);
    }
}

void Mp4AacDemuxer::parseStsz(const std::byte* p, size_t n) {
    if (!trackAudio_ || !sampleSizes_.empty() || n < 12) return;
    const uint32_t uniform = rd32(p + 4);
    const uint32_t count = rd32(p + 8);
    if (uniform != 0) {
        sampleSizes_.assign(count, uniform);
        return;
    }
    if (static_cast<uint64_t>(n) < 12ull + static_cast<uint64_t>(count) * 4ull) return;
    sampleSizes_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) sampleSizes_.push_back(rd32(p + 12 + i * 4));
}

void Mp4AacDemuxer::parseEsds(const std::byte* p, size_t n) {
    if (haveConfig_ || n < 4) return;
    const uint8_t* q = reinterpret_cast<const uint8_t*>(p) + 4;  // version/flags
    const uint8_t* end = reinterpret_cast<const uint8_t*>(p) + n;
    while (q < end) {
        const uint8_t tag = *q++;
        uint32_t len = 0;
        int cnt = 0;
        while (q < end && cnt < 4) {
            const uint8_t b = *q++;
            len = (len << 7) | (b & 0x7Fu);
            ++cnt;
            if (!(b & 0x80u)) break;
        }
        if (static_cast<uint64_t>(q - reinterpret_cast<const uint8_t*>(p)) + len > n) {
            len = static_cast<uint32_t>(end - q);
        }
        if (tag == 0x03) {  // ES_Descriptor: ES_ID(2) + flags(1) [+ optional]
            if (q + 3 > end) return;
            const uint8_t flags = q[2];
            q += 3;
            if (flags & 0x80u) {
                if (q + 2 > end) return;
                q += 2;
            }
            if (flags & 0x40u) {
                if (q >= end) return;
                const uint8_t urlLen = *q++;
                if (q + urlLen > end) return;
                q += urlLen;
            }
            if (flags & 0x20u) {
                if (q + 2 > end) return;
                q += 2;
            }
            continue;  // descend into the following descriptors
        }
        if (tag == 0x04) {  // DecoderConfigDescriptor: 13 bytes, then DSI
            if (q + 13 > end) return;
            q += 13;
            continue;
        }
        if (tag == 0x05) {  // DecoderSpecificInfo: AudioSpecificConfig
            ascLen_ = std::min<size_t>(len, sizeof(asc_));
            std::memcpy(asc_, q, ascLen_);
            if (ascLen_ >= 2) {
                const uint8_t aot = static_cast<uint8_t>((asc_[0] >> 3) & 0x1Fu);
                uint8_t sfi =
                    static_cast<uint8_t>(((asc_[0] & 0x07u) << 1) | ((asc_[1] >> 7) & 0x01u));
                const uint8_t ch = static_cast<uint8_t>((asc_[1] >> 3) & 0x0Fu);
                if (sfi == 15 && ascLen_ >= 5) {
                    const uint32_t freq = (static_cast<uint32_t>(asc_[1] & 0x7Fu) << 17) |
                                          (static_cast<uint32_t>(asc_[2]) << 9) |
                                          (static_cast<uint32_t>(asc_[3]) << 1) |
                                          (static_cast<uint32_t>(asc_[4]) >> 7);
                    sampleRate_ = freq;
                    sfi = rateIndex(freq);
                }
                profile_ = (aot >= 1 && aot <= 4) ? static_cast<uint8_t>(aot - 1) : 1;
                sfi_ = sfi;
                if (ch >= 1 && ch <= 7) channels_ = ch;
                haveConfig_ = true;
            }
            return;
        }
        q += len;  // unknown descriptor: skip its body
    }
}

void Mp4AacDemuxer::synthAdts(std::span<const std::byte> sample, std::vector<std::byte>& out) {
    const size_t frameLen = sample.size() + 7;
    if (frameLen > 0x1FFFu) return;  // ADTS frame length is 13 bits
    const uint8_t ch = (channels_ >= 1 && channels_ <= 7) ? channels_ : 2;
    auto push = [&out](uint8_t b) { out.push_back(static_cast<std::byte>(b)); };
    push(0xFF);
    push(0xF1);  // MPEG-4, layer 0, no CRC
    push(static_cast<uint8_t>((profile_ << 6) | (sfi_ << 2) | ((ch >> 2) & 0x01u)));
    push(static_cast<uint8_t>(((ch & 0x03u) << 6) | ((frameLen >> 11) & 0x03u)));
    push(static_cast<uint8_t>((frameLen >> 3) & 0xFFu));
    push(static_cast<uint8_t>(((frameLen & 0x07u) << 5) | 0x1Fu));
    push(0xFC);
    out.insert(out.end(), sample.begin(), sample.end());
}

}  // namespace squeeze2raop2
