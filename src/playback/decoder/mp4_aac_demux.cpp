// squeeze2raop2 - minimal streaming MP4/M4A -> ADTS remuxer for AAC.
//
// Deliberately small: only the boxes needed to recover the AudioSpecificConfig
// (moov > trak > mdia > minf > stbl > stsd > mp4a > esds) and the per-sample
// sizes (stsz), then a sequential rewrite of the mdat payload into ADTS frames.
// Seeking, gapless metadata and non-faststart (trailing moov) files are out of
// scope.

#include "playback/decoder/mp4_aac_demux.h"

#include "common/byte_order.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>

namespace squeeze2raop2 {

namespace {

// A parsed ISOBMFF box header. `size` is the total box size including the
// header; a declared size of 0 means "to the end of the available bytes" and
// is resolved by the caller via boxSize().
struct BoxHeader {
    std::byte type[4] = {};
    uint64_t size = 0;
    size_t header = 0;  // 8, or 16 with a 64-bit largesize
};

enum class BoxParse { Ok, NeedMore, Malformed };

// Parse the box header at the front of `buf`. NeedMore means the header (or a
// 64-bit largesize) is not fully buffered yet; Malformed means the declared
// size is smaller than the header.
BoxParse readBoxHeader(std::span<const std::byte> buf, BoxHeader& out) {
    if (buf.size() < 8) return BoxParse::NeedMore;
    out.size = readInt<Endian::Big, uint32_t>(buf.data());
    std::copy_n(buf.data() + 4, 4, out.type);
    out.header = 8;
    if (out.size == 1) {
        if (buf.size() < 16) return BoxParse::NeedMore;
        out.size = readInt<Endian::Big, uint64_t>(buf.data() + 8);
        out.header = 16;
    }
    if (out.size != 0 && out.size < out.header) return BoxParse::Malformed;
    return BoxParse::Ok;
}

// Total box size, resolving a declared 0 ("to EOF") against the bytes at hand.
uint64_t boxSize(const BoxHeader& h, size_t avail) { return h.size == 0 ? avail : h.size; }

// AAC sampling-frequency index for a rate (ISO/IEC 14496-3 table 1.18).
uint8_t rateIndex(uint32_t rate) {
    static constexpr std::array<uint32_t, 13> kRates = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    const auto it = std::ranges::find(kRates, rate);
    return it == kRates.end() ? 4 : static_cast<uint8_t>(std::distance(kRates.begin(), it));
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
            const size_t take =
                static_cast<size_t>(std::min(skipRemaining_, static_cast<uint64_t>(avail)));
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
                const size_t take =
                    static_cast<size_t>(std::min(mdatRemaining_, static_cast<uint64_t>(avail)));
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
                const auto from = buf_.begin() + static_cast<std::ptrdiff_t>(pos_);
                sample_.insert(sample_.end(), from, from + static_cast<std::ptrdiff_t>(take));
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

        const size_t avail = buf_.size() - pos_;
        BoxHeader h;
        const BoxParse status = readBoxHeader(std::span{buf_}.subspan(pos_), h);
        if (status == BoxParse::NeedMore) return;
        if (status == BoxParse::Malformed) {
            failed_ = true;
            return;
        }
        const uint64_t size = boxSize(h, avail);

        if (fourccIs(h.type, "mdat")) {
            if (!ready_) {
                failed_ = true;  // moov must precede mdat
                return;
            }
            pos_ += h.header;
            mdatRemaining_ = h.size == 0 ? std::numeric_limits<uint64_t>::max() : h.size - h.header;
            inMdat_ = true;
            continue;
        }
        if (fourccIs(h.type, "moov")) {
            if (size > avail) return;  // wait for the whole moov
            parseMoov(
                std::span{buf_}.subspan(pos_ + h.header, static_cast<size_t>(size) - h.header));
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
        if (size > avail) {
            pos_ += avail;
            skipRemaining_ = size - avail;
            compact();
            return;
        }
        pos_ += static_cast<size_t>(size);
    }
}

void Mp4AacDemuxer::parseMoov(std::span<const std::byte> boxes) {
    size_t off = 0;
    for (;;) {
        BoxHeader h;
        if (readBoxHeader(boxes.subspan(off), h) != BoxParse::Ok) return;
        const uint64_t size = boxSize(h, boxes.size() - off);
        if (off + size > boxes.size()) return;
        if (fourccIs(h.type, "trak"))
            parseTrak(boxes.subspan(off + h.header, static_cast<size_t>(size) - h.header));
        off += static_cast<size_t>(size);
    }
}

void Mp4AacDemuxer::parseTrak(std::span<const std::byte> boxes) {
    trackAudio_ = false;
    parseBoxes(boxes);
}

void Mp4AacDemuxer::parseBoxes(std::span<const std::byte> boxes) {
    size_t off = 0;
    for (;;) {
        BoxHeader h;
        if (readBoxHeader(boxes.subspan(off), h) != BoxParse::Ok) return;
        const uint64_t size = boxSize(h, boxes.size() - off);
        if (off + size > boxes.size()) return;
        const auto body = boxes.subspan(off + h.header, static_cast<size_t>(size) - h.header);
        if (fourccIs(h.type, "mdia") || fourccIs(h.type, "minf") || fourccIs(h.type, "stbl") ||
            fourccIs(h.type, "edts") || fourccIs(h.type, "udta")) {
            parseBoxes(body);
        } else if (fourccIs(h.type, "stsd")) {
            parseStsd(body);
        } else if (fourccIs(h.type, "stsz")) {
            parseStsz(body);
        } else if (fourccIs(h.type, "esds")) {
            parseEsds(body);
        }
        off += static_cast<size_t>(size);
    }
}

void Mp4AacDemuxer::parseStsd(std::span<const std::byte> body) {
    if (body.size() < 8) return;
    size_t off = 8;  // version/flags + entry_count
    for (;;) {
        BoxHeader h;
        if (readBoxHeader(body.subspan(off), h) != BoxParse::Ok) return;
        const uint64_t size = boxSize(h, body.size() - off);
        if (off + size > body.size()) return;
        if (fourccIs(h.type, "mp4a")) {
            trackAudio_ = true;
            const auto entry = body.subspan(off + h.header, static_cast<size_t>(size) - h.header);
            // AudioSampleEntry fixed fields, then child boxes (esds).
            if (entry.size() >= 28) {
                const uint16_t ch = readInt<Endian::Big, uint16_t>(entry.data() + 16);
                const uint32_t sr = readInt<Endian::Big, uint32_t>(entry.data() + 24) >> 16;
                if (ch >= 1 && ch <= 7 && !haveConfig_) channels_ = static_cast<uint8_t>(ch);
                if (sr && !haveConfig_) sampleRate_ = sr;
            }
            if (entry.size() > 28) parseBoxes(entry.subspan(28));
        }
        off += static_cast<size_t>(size);
    }
}

void Mp4AacDemuxer::parseStsz(std::span<const std::byte> body) {
    if (!trackAudio_ || !sampleSizes_.empty() || body.size() < 12) return;
    const uint32_t uniform = readInt<Endian::Big, uint32_t>(body.data() + 4);
    const uint32_t count = readInt<Endian::Big, uint32_t>(body.data() + 8);
    if (uniform != 0) {
        sampleSizes_.assign(count, uniform);
        return;
    }
    if (body.size() < 12 + static_cast<size_t>(count) * 4) return;
    sampleSizes_.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        sampleSizes_.push_back(readInt<Endian::Big, uint32_t>(body.data() + 12 + i * 4));
}

void Mp4AacDemuxer::parseEsds(std::span<const std::byte> body) {
    if (haveConfig_ || body.size() < 4) return;
    const uint8_t* q = reinterpret_cast<const uint8_t*>(body.data()) + 4;  // version/flags
    const uint8_t* end = reinterpret_cast<const uint8_t*>(body.data()) + body.size();
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
        const size_t remaining = static_cast<size_t>(end - q);
        if (len > remaining) len = static_cast<uint32_t>(remaining);
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
            ascLen_ = std::min<size_t>(len, asc_.size());
            std::copy_n(q, ascLen_, asc_.begin());
            if (ascLen_ >= 2) {
                const uint8_t aot = static_cast<uint8_t>((asc_[0] >> 3) & 0x1Fu);
                uint8_t sfi =
                    static_cast<uint8_t>(((asc_[0] & 0x07u) << 1) | ((asc_[1] >> 7) & 0x01u));
                const uint8_t ch = static_cast<uint8_t>((asc_[1] >> 3) & 0x0Fu);
                if (sfi == 15 && ascLen_ >= 5) {
                    // (asc_[1] & 0x7Fu) is already unsigned int: the u-suffixed literal
                    // promotes the uint8_t operand, so no widening cast is needed.
                    const uint32_t freq = ((asc_[1] & 0x7Fu) << 17) |
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
    push(static_cast<uint8_t>((static_cast<uint32_t>(profile_) << 6) |
                              (static_cast<uint32_t>(sfi_) << 2) | ((ch >> 2) & 0x01u)));
    push(static_cast<uint8_t>(((ch & 0x03u) << 6) | ((frameLen >> 11) & 0x03u)));
    push(static_cast<uint8_t>((frameLen >> 3) & 0xFFu));
    push(static_cast<uint8_t>(((frameLen & 0x07u) << 5) | 0x1Fu));
    push(0xFC);
    out.insert(out.end(), sample.begin(), sample.end());
}

}  // namespace squeeze2raop2
