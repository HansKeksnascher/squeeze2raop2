#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sq2 {

// Incremental MP3 decoder wrapper around the public-domain minimp3 single header.
// Compressed bytes accumulate in one contiguous window (the consumed prefix is
// compacted away periodically); every decode call sees the full remainder, which
// keeps minimp3's frame-chain validation stable while streaming in chunks.
class Mp3Decoder {
public:
    Mp3Decoder();
    ~Mp3Decoder();
    Mp3Decoder(const Mp3Decoder&) = delete;
    Mp3Decoder& operator=(const Mp3Decoder&) = delete;

    void feed(const uint8_t* data, size_t len);
    // Signal end of compressed input; decodes remaining tail frames.
    void finish();
    size_t drain(int16_t* out, size_t maxSamples);

    uint32_t sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }
    size_t pendingBytes() const { return buffer_.size() - consumed_; }
    bool valid() const { return sampleRate_ != 0; }
    bool hasError() const { return failed_; }

    void reset();

private:
    void decodeMore();

    void* handle_ = nullptr;       // mp3dec_t*
    std::vector<uint8_t> buffer_;  // contiguous compressed window
    size_t consumed_ = 0;          // decoded/skipped prefix of buffer_
    std::vector<int16_t> pcm_;     // decoded samples awaiting drain
    bool eof_ = false;
    uint32_t sampleRate_ = 0;
    int channels_ = 2;
    bool failed_ = false;
};

}  // namespace sq2
