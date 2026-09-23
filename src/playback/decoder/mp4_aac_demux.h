#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace squeeze2raop2 {

// Minimal streaming ISOBMFF (MP4/M4A) demuxer for AAC. LMS sends .m4a to an
// `aac`-capable player as the raw MP4 container (pcm_sample_size '5'); this
// class parses moov (the AudioSpecificConfig from esds and the per-sample
// sizes from stsz) and rewrites the mdat payload as a synthesized ADTS stream
// that AacDecoder's normal libxaac ADTS path consumes.
//
// Sequential playback only: samples are read in order, so stco/stsc are not
// needed. Requires moov before mdat (faststart); a trailing moov cannot be
// streamed without buffering the whole file and is reported as a failure.
class Mp4AacDemuxer {
public:
    Mp4AacDemuxer();
    Mp4AacDemuxer(const Mp4AacDemuxer&) = delete;
    Mp4AacDemuxer& operator=(const Mp4AacDemuxer&) = delete;

    void feed(std::span<const std::byte> data);
    // Signal end of input (a trailing box may be flushed).
    void finish();
    // Append any ready ADTS-framed bytes to `out`.
    void drain(std::vector<std::byte>& out);

    bool failed() const { return failed_; }
    size_t pending() const { return buf_.size() - pos_; }

private:
    void pump();
    void compact();
    // Each parse* receives the payload (header stripped) of one container.
    void parseMoov(std::span<const std::byte> boxes);
    void parseTrak(std::span<const std::byte> boxes);
    void parseBoxes(std::span<const std::byte> boxes);
    void parseStsd(std::span<const std::byte> body);
    void parseStsz(std::span<const std::byte> body);
    void parseEsds(std::span<const std::byte> body);
    void synthAdts(std::span<const std::byte> sample, std::vector<std::byte>& out);

    std::vector<std::byte> buf_;     // unconsumed input bytes
    size_t pos_ = 0;                 // parse position within buf_
    std::vector<std::byte> out_;     // synthesized ADTS frames awaiting drain
    std::vector<std::byte> sample_;  // current access unit being assembled
    std::vector<uint32_t> sampleSizes_;
    size_t nextSample_ = 0;
    uint64_t mdatRemaining_ = 0;
    uint64_t skipRemaining_ = 0;
    bool inMdat_ = false;
    bool ready_ = false;
    bool failed_ = false;
    bool done_ = false;
    bool trackAudio_ = false;
    bool haveConfig_ = false;
    std::array<uint8_t, 64> asc_ = {};
    size_t ascLen_ = 0;
    uint8_t profile_ = 1;  // ADTS profile = AOT - 1 (LC core)
    uint8_t sfi_ = 4;      // sampling-frequency index (44100)
    uint32_t sampleRate_ = 44100;
    uint8_t channels_ = 2;
};

}  // namespace squeeze2raop2
