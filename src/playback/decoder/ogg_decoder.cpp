// squeeze2raop2 - native Ogg Vorbis decoding via the vendored stb_vorbis
// (public domain / MIT, third_party/stb/stb_vorbis.c). This wrapper is
// squeeze2raop2's own code.

#define STB_VORBIS_HEADER_ONLY

#include "common/third_party_warnings.h"

SQUEEZE2RAOP2_TP_WARNINGS_PUSH
#include "stb_vorbis.c"
SQUEEZE2RAOP2_TP_WARNINGS_POP

#include <algorithm>
#include <cmath>
#include <limits>

#include "common/log.h"
#include "playback/decoder/ogg_decoder.h"

namespace squeeze2raop2 {

namespace {
// stb_vorbis's push-data decode entry points take an int byte count.
constexpr size_t kMaxWindow = static_cast<size_t>(std::numeric_limits<int>::max());
}  // namespace

OggDecoder::OggDecoder(const PcmFormat& in) : Decoder(in) {}

OggDecoder::~OggDecoder() {
    if (vorbis_) stb_vorbis_close(vorbis_);
}

void OggDecoder::fail(std::string_view why) {
    if (failed_) return;
    failed_ = true;
    log::error(log::Area::Dec, "ogg decode failed: {}", why);
}

void OggDecoder::feed(std::span<const std::byte> data) {
    if (failed_ || data.empty()) return;
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    decodeMore();
}

void OggDecoder::finish() {
    if (failed_) return;
    eof_ = true;
    decodeMore();
}

void OggDecoder::decodeMore() {
    if (failed_) return;
    const size_t total = buffer_.size();

    // Open the stream once enough header pages are buffered. Before that the
    // decoder is not valid and format() falls back to the input format.
    if (!opened_) {
        const size_t remaining = total - consumed_;
        if (remaining == 0) {
            if (eof_) fail("empty stream");
            return;
        }
        const uint8_t* data = reinterpret_cast<const uint8_t*>(buffer_.data()) + consumed_;
        int consumedNow = 0;
        int error = VORBIS__no_error;
        stb_vorbis* v = stb_vorbis_open_pushdata(
            data, static_cast<int>(std::min(remaining, kMaxWindow)), &consumedNow, &error, nullptr);
        if (v) {
            vorbis_ = v;
            opened_ = true;
            consumed_ += static_cast<size_t>(consumedNow);
            const stb_vorbis_info info = stb_vorbis_get_info(vorbis_);
            sampleRate_ = info.sample_rate;
            channels_ = info.channels;
            if (channels_ > 2) {
                fail("too many channels");
                return;
            }
            // The channels reported by get_info is the stream's channel count;
            // mono stays mono here and AirplayOutput expands it downstream.
            log::info(log::Area::Dec, "ogg: {} Hz, {} ch", sampleRate_, channels_);
            compactConsumed(buffer_, consumed_);
        } else if (error == VORBIS_need_more_data) {
            // Incomplete header block: wait for more input unless the source
            // already ended (a truncated stream is an error, not an end).
            if (eof_) fail("truncated header");
            return;
        } else {
            fail("header");
            return;
        }
    }

    // Frame loop: pull every complete frame off the window, advancing
    // consumed_ by stb_vorbis's consumed count as it goes.
    for (;;) {
        const size_t remaining = buffer_.size() - consumed_;
        if (remaining == 0) break;
        const uint8_t* data = reinterpret_cast<const uint8_t*>(buffer_.data()) + consumed_;
        int channels = 0;
        float** output = nullptr;
        int samples = 0;
        const int used = stb_vorbis_decode_frame_pushdata(
            vorbis_, data, static_cast<int>(std::min(remaining, kMaxWindow)), &channels, &output,
            &samples);
        if (used == 0 && samples == 0) break;  // need more input
        if (used < 0) {
            fail("frame");
            return;
        }
        consumed_ += static_cast<size_t>(used);
        if (samples > 0 && channels > 0 && channels <= 2) {
            // Interleave the per-channel float buffers into s16 PCM.
            const size_t base = pcm_.size();
            pcm_.resize(base + static_cast<size_t>(samples) * static_cast<size_t>(channels));
            for (int ch = 0; ch < channels; ++ch) {
                const float* src = output[ch];
                int16_t* dst = pcm_.data() + base + ch;
                for (int s = 0; s < samples; ++s) {
                    const float clamped = std::clamp(src[s] * 32768.0f, -32768.0f, 32767.0f);
                    dst[s * channels] = static_cast<int16_t>(std::lround(clamped));
                }
            }
        }
        compactConsumed(buffer_, consumed_);
    }
}

size_t OggDecoder::drain(std::span<int16_t> out) { return takeSamples(out, pcm_); }

}  // namespace squeeze2raop2