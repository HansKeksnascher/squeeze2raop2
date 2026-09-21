// sqraop2 - bridge between LMS slimproto and AirPlay senders.
// This wrapper is sqraop2's own code; minimp3 itself is public domain / CC0
// (third_party/minimp3/minimp3.h).

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include <minimp3.h>

#include <algorithm>
#include <cstring>

#include "log.h"
#include "mp3_decoder.h"

namespace sq2 {

namespace {
// Only decode while at least this many compressed bytes are pending: a full
// worst-case frame plus header keeps minimp3's frame-chain check from
// spuriously resetting mid-stream. At EOF the threshold drops to the minimum.
constexpr size_t kLiveWindow = 2048 + 8;
constexpr size_t kEofWindow = 8;
// Compact the consumed prefix once it passes this size.
constexpr size_t kCompactThreshold = 1 << 16;
}  // namespace

Mp3Decoder::Mp3Decoder() {
    auto* dec = new mp3dec_t;
    mp3dec_init(dec);
    handle_ = dec;
}

Mp3Decoder::~Mp3Decoder() {
    if (handle_) {
        delete static_cast<mp3dec_t*>(handle_);
        handle_ = nullptr;
    }
}

void Mp3Decoder::feed(const uint8_t* data, size_t len) {
    if (!data || !len) return;
    buffer_.insert(buffer_.end(), data, data + len);
    decodeMore();
}

void Mp3Decoder::finish() {
    eof_ = true;
    decodeMore();
}

void Mp3Decoder::decodeMore() {
    if (failed_ || !handle_) return;
    auto* dec = static_cast<mp3dec_t*>(handle_);
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    const size_t minWindow = eof_ ? kEofWindow : kLiveWindow;

    for (;;) {
        const size_t remaining = buffer_.size() - consumed_;
        if (remaining < minWindow) break;

        mp3dec_frame_info_t info{};
        int frames = mp3dec_decode_frame(dec, buffer_.data() + consumed_,
                                         static_cast<int>(remaining), pcm, &info);
        if (info.frame_bytes <= 0) {
            // Insufficient data for the frame at the window head; wait for more.
            break;
        }
        // frames > 0: successful decode; frames == 0: decoder skipped
        // invalid/ID3 data. Both advance by the consumed byte count.
        consumed_ += static_cast<size_t>(info.frame_bytes);
        if (frames > 0) {
            if (info.channels > 0 && info.hz > 0) {
                channels_ = info.channels;
                sampleRate_ = static_cast<uint32_t>(info.hz);
            }
            const size_t got =
                static_cast<size_t>(frames) * static_cast<size_t>(channels_);
            pcm_.insert(pcm_.end(), pcm, pcm + static_cast<long>(got));
        }

        if (consumed_ >= kCompactThreshold) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(consumed_));
            consumed_ = 0;
        }
    }
}

void Mp3Decoder::reset() {
    buffer_.clear();
    pcm_.clear();
    consumed_ = 0;
    eof_ = false;
    if (handle_) {
        mp3dec_init(static_cast<mp3dec_t*>(handle_));
    }
}

size_t Mp3Decoder::drain(int16_t* out, size_t maxSamples) {
    const size_t n = std::min(pcm_.size(), maxSamples);
    if (n) {
        std::memcpy(out, pcm_.data(), n * sizeof(int16_t));
        pcm_.erase(pcm_.begin(), pcm_.begin() + static_cast<long>(n));
    }
    return n;
}

}  // namespace sq2
