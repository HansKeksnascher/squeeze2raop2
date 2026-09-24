// squeeze2raop2 - native Ogg Opus decoding via the vendored libogg + libopus
// (both BSD-3-Clause, third_party/ogg and third_party/opus). This wrapper is
// squeeze2raop2's own code.

#include "playback/decoder/opus_decoder.h"

#include "common/log.h"
#include "common/third_party_warnings.h"

#include <algorithm>
#include <cstring>

SQUEEZE2RAOP2_TP_WARNINGS_PUSH
#include <ogg/ogg.h>
#include <opus.h>
SQUEEZE2RAOP2_TP_WARNINGS_POP

namespace squeeze2raop2 {

namespace {

// Opus always decodes at 48 kHz; the pipeline adopts the decoder format.
constexpr int kOpusRate = 48000;
// Longest Opus frame is 120 ms = 5760 samples per channel at 48 kHz.
constexpr int kMaxFrame = 5760;
// Give up after this many consecutive undecodable packets (garbage stream).
constexpr int kMaxBadPackets = 64;

SQUEEZE2RAOP2_TP_WARNINGS_PUSH
// OPUS_SET_GAIN expands to old-style casts in the vendored header, so keep the
// expansion under the third-party suppression (it fires at the use site).
void applyOutputGain(::OpusDecoder* dec, int gainQ8) {
    opus_decoder_ctl(dec, OPUS_SET_GAIN(gainQ8));
}
SQUEEZE2RAOP2_TP_WARNINGS_POP

}  // namespace

struct OggOpusDecoder::Impl {
    ogg_sync_state sync{};
    ogg_stream_state stream{};
    bool streamInit = false;
    ::OpusDecoder* dec = nullptr;
    std::vector<opus_int16> decodeBuf;  // one opus_decode() result, interleaved

    uint32_t preSkip = 0;        // frames to drop from the stream head
    uint64_t decodedFrames = 0;  // frames decoded so far, including pre-skip
    uint64_t emittedFrames = 0;  // frames appended to pcm_, after trims
    uint64_t totalFrames = 0;    // final granule - pre-skip; 0 = live/unknown
    bool tagsSeen = false;
    int badPackets = 0;

    ~Impl() {
        if (streamInit) ogg_stream_clear(&stream);
        ogg_sync_clear(&sync);
        if (dec) opus_decoder_destroy(dec);
    }
};

OggOpusDecoder::OggOpusDecoder(const PcmFormat& in) : Decoder(in), impl_(std::make_unique<Impl>()) {
    ogg_sync_init(&impl_->sync);
}

OggOpusDecoder::~OggOpusDecoder() = default;

void OggOpusDecoder::fail(std::string_view why) {
    if (failed_) return;
    failed_ = true;
    log::error(log::Area::Dec, "opus decode failed: {}", why);
}

bool OggOpusDecoder::pushBytes(std::span<const std::byte> data) {
    // ogg_sync takes and owns a copy; request the whole span in one go.
    char* buf = ogg_sync_buffer(&impl_->sync, static_cast<long>(data.size()));
    if (!buf) {
        fail("ogg_sync_buffer");
        return false;
    }
    std::memcpy(buf, data.data(), data.size());
    ogg_sync_wrote(&impl_->sync, static_cast<long>(data.size()));
    return true;
}

void OggOpusDecoder::decodeMore() {
    if (failed_) return;
    for (;;) {
        ogg_page page{};
        const int r = ogg_sync_pageout(&impl_->sync, &page);
        if (r == 0) break;    // need more bytes for the next page
        if (r < 0) continue;  // junk between pages: resync and try again

        if (!impl_->streamInit) {
            if (ogg_stream_init(&impl_->stream, ogg_page_serialno(&page)) != 0) {
                fail("ogg_stream_init");
                return;
            }
            impl_->streamInit = true;
        }
        if (ogg_stream_pagein(&impl_->stream, &page) != 0) {
            // A different logical stream (chained/multiplexed) or corruption;
            // this decoder handles a single stream only.
            fail("ogg page");
            return;
        }

        // Record the end position before decoding the page's packets so the
        // final packets can be trimmed against it.
        if (ogg_page_eos(&page)) {
            const ogg_int64_t granule = ogg_page_granulepos(&page);
            if (granule >= 0) {
                const uint64_t total = static_cast<uint64_t>(granule);
                impl_->totalFrames = total > impl_->preSkip ? total - impl_->preSkip : 0;
            }
        }

        for (;;) {
            ogg_packet packet{};
            const int pr = ogg_stream_packetout(&impl_->stream, &packet);
            if (pr == 0) break;
            if (pr < 0) continue;  // hole in the stream: skip
            handlePacket(packet.packet, static_cast<int>(packet.bytes));
        }
    }
}

void OggOpusDecoder::handlePacket(const void* data, int bytes) {
    if (failed_) return;
    const auto* p = static_cast<const unsigned char*>(data);

    // First packet: OpusHead (RFC 7845 §5.1).
    if (!headerSeen_) {
        if (bytes < 19 || std::memcmp(p, "OpusHead", 8) != 0) {
            fail("not opus");
            return;
        }
        if ((p[8] >> 4) != 0) {  // major version must be 0
            fail("opus version");
            return;
        }
        const int channels = p[9];
        const uint32_t preSkip = static_cast<uint32_t>(p[10]) | (static_cast<uint32_t>(p[11]) << 8);
        const auto gain = static_cast<int16_t>(static_cast<uint16_t>(p[16]) |
                                               (static_cast<uint16_t>(p[17]) << 8));
        const uint8_t family = p[18];
        if (channels < 1 || channels > 2) {
            fail("opus channels");
            return;
        }
        if (family != 0) {
            fail("opus channel mapping");
            return;
        }
        int err = OPUS_OK;
        impl_->dec = opus_decoder_create(kOpusRate, channels, &err);
        if (!impl_->dec || err != OPUS_OK) {
            fail("opus_decoder_create");
            return;
        }
        // OpusHead output gain is signed Q8 dB, the same unit OPUS_SET_GAIN expects.
        applyOutputGain(impl_->dec, static_cast<int>(gain));
        impl_->decodeBuf.resize(static_cast<size_t>(kMaxFrame) * static_cast<size_t>(channels));
        impl_->preSkip = preSkip;
        channels_ = channels;
        sampleRate_ = kOpusRate;
        headerSeen_ = true;
        log::info(log::Area::Dec, "opus: {} Hz, {} ch, pre-skip {} frames", sampleRate_, channels_,
                  preSkip);
        return;
    }

    // Second packet: OpusTags (RFC 7845 §5.2); parsed and ignored.
    if (!impl_->tagsSeen) {
        if (bytes < 8 || std::memcmp(p, "OpusTags", 8) != 0) {
            fail("opus tags");
            return;
        }
        impl_->tagsSeen = true;
        return;
    }

    // Audio packet. opus_decode writes interleaved s16 for the decoder's
    // channel count.
    const int frames = opus_decode(impl_->dec, p, bytes, impl_->decodeBuf.data(), kMaxFrame, 0);
    if (frames < 0) {
        if (++impl_->badPackets > kMaxBadPackets) fail("opus packets");
        return;
    }
    if (frames > 0) appendFrames(frames);
}

void OggOpusDecoder::appendFrames(int count) {
    const size_t ch = static_cast<size_t>(channels_);
    uint64_t skip = 0;
    // Drop the codec delay (pre-skip) from the head of the stream.
    if (impl_->decodedFrames < impl_->preSkip) {
        skip =
            std::min<uint64_t>(static_cast<uint64_t>(count), impl_->preSkip - impl_->decodedFrames);
    }
    uint64_t keep = static_cast<uint64_t>(count) - skip;
    // Trim the encoded padding at the end once the final granule is known.
    if (impl_->totalFrames != 0) {
        if (impl_->emittedFrames >= impl_->totalFrames) {
            keep = 0;
        } else {
            keep = std::min<uint64_t>(keep, impl_->totalFrames - impl_->emittedFrames);
        }
    }
    if (keep > 0) {
        const auto base = static_cast<std::ptrdiff_t>(skip * ch);
        const auto n = static_cast<std::ptrdiff_t>(keep * ch);
        pcm_.insert(pcm_.end(), impl_->decodeBuf.begin() + base,
                    impl_->decodeBuf.begin() + base + n);
        impl_->emittedFrames += keep;
    }
    impl_->decodedFrames += static_cast<uint64_t>(count);
}

void OggOpusDecoder::feed(std::span<const std::byte> data) {
    if (failed_ || data.empty()) return;
    if (!pushBytes(data)) return;
    decodeMore();
}

void OggOpusDecoder::finish() {
    if (failed_) return;
    // A stream that ended before its OpusHead is an error; otherwise every
    // complete packet has already been decoded by feed().
    if (!headerSeen_) fail("truncated header");
}

size_t OggOpusDecoder::drain(std::span<int16_t> out) { return takeSamples(out, pcm_); }

size_t OggOpusDecoder::pendingBytes() const {
    const long pending = impl_->sync.fill - impl_->sync.returned;
    return pending > 0 ? static_cast<size_t>(pending) : 0;
}

}  // namespace squeeze2raop2