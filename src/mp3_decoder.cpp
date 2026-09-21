// squeeze2raop2 - bridge between LMS slimproto and AirPlay senders.
// This wrapper is squeeze2raop2's own code; minimp3 itself is public domain / CC0
// (third_party/minimp3/minimp3.h).

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif
#include <minimp3.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <type_traits>

#include "log.h"
#include "mp3_decoder.h"

namespace squeeze2raop2 {

namespace {
// Only decode while at least this many compressed bytes are pending: a full
// worst-case frame plus header keeps minimp3's frame-chain check from
// spuriously resetting mid-stream. At EOF the threshold drops to the minimum.
constexpr size_t kLiveWindow = 2048 + 8;
constexpr size_t kEofWindow = 8;
// Compact the consumed prefix once it passes this size.
constexpr size_t kCompactThreshold = 1 << 16;
// minimp3's decode entry point takes an int byte count.
constexpr size_t kMaxWindow = std::numeric_limits<int>::max();

static_assert(std::is_trivially_copyable_v<mp3dec_t>);
}  // namespace

Mp3Decoder::Mp3Decoder() { mp3dec_init(&dec_); }

void Mp3Decoder::feed(std::span<const std::byte> data) {
    if (data.empty()) return;
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    decodeMore();
} // namespace

void Mp3Decoder::finish() {
    eof_ = true;
    decodeMore();
} // namespace squeeze2raop2

void Mp3Decoder::decodeMore() {
    if (failed_) return;
    const size_t minWindow = eof_ ? kEofWindow : kLiveWindow;
    constexpr auto kMaxSamples = static_cast<size_t>(MINIMP3_MAX_SAMPLES_PER_FRAME);
    std::array<mp3d_sample_t, kMaxSamples> pcm{};

    for (;;) {
        const size_t remaining = buffer_.size() - consumed_;
        if (remaining < minWindow) break;

        mp3dec_frame_info_t info{};
        int frames = mp3dec_decode_frame(
            &dec_, reinterpret_cast<const uint8_t*>(buffer_.data()) + consumed_,
            static_cast<int>(std::min(remaining, kMaxWindow)), pcm.data(), &info);
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
            const size_t got = static_cast<size_t>(frames) * static_cast<size_t>(channels_);
            pcm_.insert(pcm_.end(), std::span{pcm}.first(got).begin(),
                        std::span{pcm}.first(got).end());
        }

        if (consumed_ >= kCompactThreshold) {
            buffer_.erase(buffer_.begin(),
                          buffer_.begin() + static_cast<std::ptrdiff_t>(consumed_));
            consumed_ = 0;
        }
    }
}

void Mp3Decoder::reset() {
    buffer_.clear();
    pcm_.clear();
    consumed_ = 0;
    eof_ = false;
    mp3dec_init(&dec_);
}

size_t Mp3Decoder::drain(std::span<int16_t> out) {
    const size_t n = std::min(pcm_.size(), out.size());
    if (n) {
        std::ranges::copy(std::span{pcm_}.first(n), out.begin());
        pcm_.erase(pcm_.begin(), pcm_.begin() + static_cast<std::ptrdiff_t>(n));
    }
    return n;
}

}  // namespace squeeze2raop2
