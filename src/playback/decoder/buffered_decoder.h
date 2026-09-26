#pragma once

#include "playback/decoder/decoder.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace squeeze2raop2 {

// Base for decoders that accumulate a compressed byte window and a queue of
// decoded samples: the feed/drain plumbing shared by the mp3/aac/ogg/opus
// wrappers. Subclasses own their codec state and implement feed()/finish()
// and the codec-specific hooks; the window, the failure flag and the sample
// queue live here. drain()/hasError()/pendingBytes() are supplied, so a
// subclass only overrides pendingBytes() when its codec buffers elsewhere
// (AAC's MP4 demuxer, Opus's ogg_sync).
class BufferedDecoder : public Decoder {
public:
    size_t drain(std::span<int16_t> out) override { return takeSamples(out, pcm_); }
    bool hasError() const override { return failed_; }
    size_t pendingBytes() const override { return bufferedBytes(); }

protected:
    using Decoder::Decoder;

    void fail(std::string_view why);
    void appendInput(std::span<const std::byte> data) {
        buffer_.insert(buffer_.end(), data.begin(), data.end());
    }
    [[nodiscard]] size_t bufferedBytes() const { return buffer_.size() - consumed_; }

    std::vector<std::byte> buffer_;  // compressed window, prefix consumed_
    size_t consumed_ = 0;            // decoded/skipped prefix of buffer_
    std::vector<int16_t> pcm_;
    bool eof_ = false;
    bool failed_ = false;
};

}  // namespace squeeze2raop2