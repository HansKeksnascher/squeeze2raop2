// squeeze2raop2 - native AAC decoding via the vendored libxaac (Apache-2.0).
// This wrapper is squeeze2raop2's own code; libxaac itself lives under
// third_party/libxaac and keeps its own license.

#include "playback/decoder/aac_decoder.h"

#include "common/log.h"
#include "playback/decoder/mp4_aac_demux.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif
extern "C" {
#include "ixheaac_error_standards.h"
#include "ixheaac_type_def.h"
#include "ixheaacd_aac_config.h"
#include "ixheaacd_apicmd_standards.h"
#include "ixheaacd_memory_standards.h"
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace squeeze2raop2 {

// libxaac state: the API object plus every allocation it requires. All blocks
// are owned here and released in one place. Defined before the init helper so
// the helper can name the type.
struct AacDecoder::Xaac {
    void* api = nullptr;
    uint8_t* in = nullptr;
    size_t inSize = 0;
    uint8_t* out = nullptr;
    size_t outSize = 0;
    std::vector<void*> blocks;

    ~Xaac() {
        for (void* block : blocks) std::free(block);
    }

    // Allocate `size` bytes aligned to `align` (any positive alignment).
    void* alloc(size_t size, size_t align) {
        if (align == 0) align = 1;
        void* raw = std::malloc(size + align);
        if (!raw) return nullptr;
        blocks.push_back(raw);
        const uintptr_t base = reinterpret_cast<uintptr_t>(raw);
        const uintptr_t rem = base % align;
        const uintptr_t aligned = rem ? base + (align - rem) : base;
        return reinterpret_cast<void*>(aligned);
    }
};

namespace {

// libxaac's single decoder entry point (declared by its testbench, not in a
// public header). Plain C types keep the third-party type soup out of here.
extern "C" int32_t ixheaacd_dec_api(void* obj, int32_t cmd, int32_t idx, void* value);

bool isFatal(int32_t err) { return (static_cast<uint32_t>(err) & 0x80000000u) != 0u; }

int32_t callApi(void* api, int32_t cmd, int32_t idx, void* value) {
    return ixheaacd_dec_api(api, cmd, idx, value);
}

// Allocate libxaac's API object and all of its tables/memory, then run the
// two-stage config init. Mirrors test/decoder/ixheaacd_main.c.
bool initXaac(AacDecoder::Xaac& x) {
    uint32_t apiSize = 0;
    if (isFatal(callApi(nullptr, IA_API_CMD_GET_API_SIZE, 0, &apiSize)) || apiSize == 0)
        return false;
    void* apiMem = x.alloc(apiSize, 8);
    if (!apiMem) return false;
    x.api = apiMem;

    if (isFatal(callApi(x.api, IA_API_CMD_INIT, IA_CMD_TYPE_INIT_API_PRE_CONFIG_PARAMS, nullptr)))
        return false;

    // Always decode through the ADTS path: MP4 input is demuxed into
    // synthesized ADTS frames before it reaches here. 16-bit interleaved
    // stereo output, no downmix, no DRC.
    uint32_t pcmWdsz = 16;
    uint32_t maxChannels = 2;
    uint32_t toStereo = 1;
    uint32_t downmix = 0;
    uint32_t mp4Flag = 0;
    callApi(x.api, IA_API_CMD_SET_CONFIG_PARAM, IA_XHEAAC_DEC_CONFIG_PARAM_PCM_WDSZ, &pcmWdsz);
    callApi(x.api, IA_API_CMD_SET_CONFIG_PARAM, IA_XHEAAC_DEC_CONFIG_PARAM_MAX_CHANNEL,
            &maxChannels);
    callApi(x.api, IA_API_CMD_SET_CONFIG_PARAM, IA_XHEAAC_DEC_CONFIG_PARAM_TOSTEREO, &toStereo);
    callApi(x.api, IA_API_CMD_SET_CONFIG_PARAM, IA_XHEAAC_DEC_CONFIG_PARAM_DOWNMIX, &downmix);
    callApi(x.api, IA_API_CMD_SET_CONFIG_PARAM, IA_XHEAAC_DEC_CONFIG_PARAM_MP4FLAG, &mp4Flag);

    uint32_t memTabsSize = 0;
    if (isFatal(callApi(x.api, IA_API_CMD_GET_MEMTABS_SIZE, 0, &memTabsSize)) || memTabsSize == 0)
        return false;
    void* memTabs = x.alloc(memTabsSize, 8);
    if (!memTabs) return false;
    if (isFatal(callApi(x.api, IA_API_CMD_SET_MEMTABS_PTR, 0, memTabs))) return false;
    if (isFatal(callApi(x.api, IA_API_CMD_INIT, IA_CMD_TYPE_INIT_API_POST_CONFIG_PARAMS, nullptr)))
        return false;

    uint32_t count = 0;
    if (isFatal(callApi(x.api, IA_API_CMD_GET_N_MEMTABS, 0, &count))) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t size = 0;
        uint32_t align = 0;
        uint32_t type = 0;
        const int32_t idx = static_cast<int32_t>(i);
        if (isFatal(callApi(x.api, IA_API_CMD_GET_MEM_INFO_SIZE, idx, &size))) return false;
        if (isFatal(callApi(x.api, IA_API_CMD_GET_MEM_INFO_ALIGNMENT, idx, &align))) return false;
        if (isFatal(callApi(x.api, IA_API_CMD_GET_MEM_INFO_TYPE, idx, &type))) return false;
        if (align == 0) align = 8;
        void* ptr = x.alloc(size, align);
        if (!ptr) return false;
        if (isFatal(callApi(x.api, IA_API_CMD_SET_MEM_PTR, idx, ptr))) return false;
        if (type == IA_MEMTYPE_INPUT) {
            x.in = static_cast<uint8_t*>(ptr);
            x.inSize = size;
        } else if (type == IA_MEMTYPE_OUTPUT) {
            x.out = static_cast<uint8_t*>(ptr);
            x.outSize = size;
        }
    }
    return x.in != nullptr && x.out != nullptr && x.inSize != 0 && x.outSize != 0;
}

}  // namespace

AacDecoder::AacDecoder(const PcmFormat& in, uint8_t containerCode)
    : Decoder(in), containerCode_(containerCode) {
    if (containerCode == '5') {
        mp4_ = std::make_unique<Mp4AacDemuxer>();
    } else if (containerCode != '2' && containerCode != 0) {
        // ADIF / LATM-LOAS / raw packet transports are not supported.
        log::error(log::Area::Dec, "aac: unsupported transport code '{}'",
                   static_cast<char>(containerCode));
        failed_ = true;
    }
}

AacDecoder::~AacDecoder() = default;

void AacDecoder::fail(std::string_view why) {
    if (failed_) return;
    failed_ = true;
    log::error(log::Area::Dec, "aac decode failed: {}", why);
}

size_t AacDecoder::fillInput() {
    // libxaac consumes whatever it is handed and drops a trailing partial
    // frame, which desyncs its ADTS parser at arbitrary chunk boundaries. Only
    // ever hand it complete ADTS frames; the remainder stays buffered.
    const size_t limit = std::min(buffer_.size(), consumed_ + xaac_->inSize);
    const size_t end = frameEnd(limit);
    if (end <= consumed_) return 0;
    const size_t n = end - consumed_;
    std::memcpy(xaac_->in, buffer_.data() + consumed_, n);
    return n;
}

// End offset of the last complete ADTS frame in buffer_[consumed_..limit).
// Junk bytes before a sync word are skipped; a partial frame at the limit is
// left for the next call. Returns consumed_ when no complete frame is present.
size_t AacDecoder::frameEnd(size_t limit) const {
    size_t off = consumed_;
    size_t end = consumed_;
    while (off + 7 <= limit) {
        const uint8_t b0 = std::to_integer<uint8_t>(buffer_[off]);
        const uint8_t b1 = std::to_integer<uint8_t>(buffer_[off + 1]);
        if (b0 == 0xFF && (b1 & 0xF6u) == 0xF0u) {
            const size_t len = ((std::to_integer<uint8_t>(buffer_[off + 3]) & 0x03u) << 11) |
                               (std::to_integer<uint8_t>(buffer_[off + 4]) << 3) |
                               (std::to_integer<uint8_t>(buffer_[off + 5]) >> 5);
            if (len >= 7 && off + len <= limit) {
                off += len;
                end = off;
                continue;
            }
            if (len >= 7) break;  // partial frame at the limit
        }
        ++off;  // junk or a bad header: scan for the next sync word
    }
    return end;
}

void AacDecoder::appendPcm(size_t bytes) {
    const size_t samples = bytes / sizeof(int16_t);
    if (!samples) return;
    const size_t base = pcm_.size();
    pcm_.resize(base + samples);
    std::memcpy(pcm_.data() + base, xaac_->out, samples * sizeof(int16_t));
}

void AacDecoder::compact() {
    constexpr size_t kThreshold = 1 << 16;
    if (consumed_ >= kThreshold) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed_));
        consumed_ = 0;
    }
}

bool AacDecoder::ensureInit() {
    if (initDone_) return true;
    if (failed_) return false;
    if (!xaac_) {
        auto state = std::make_unique<Xaac>();
        if (!initXaac(*state)) {
            fail("libxaac init");
            return false;
        }
        xaac_ = std::move(state);
    }

    // Wait until at least one whole ADTS frame is buffered (or EOF) so the
    // header pass never sees a partial frame.
    if (!eof_ && frameEnd(buffer_.size()) == consumed_) return false;
    int retries = 0;

    for (;;) {
        const size_t n = fillInput();
        if (n == 0 && !eof_) return false;  // wait for more input
        uint32_t inBytes = static_cast<uint32_t>(n);
        callApi(xaac_->api, IA_API_CMD_SET_INPUT_BYTES, 0, &inBytes);
        const int32_t err = callApi(xaac_->api, IA_API_CMD_INIT, IA_CMD_TYPE_INIT_PROCESS, nullptr);
        uint32_t done = 0;
        callApi(xaac_->api, IA_API_CMD_INIT, IA_CMD_TYPE_INIT_DONE_QUERY, &done);
        uint32_t consumed = 0;
        callApi(xaac_->api, IA_API_CMD_GET_CURIDX_INPUT_BUF, 0, &consumed);
        if (consumed) {
            consumed_ += consumed;
            compact();
            retries = 0;
        }
        if (isFatal(err)) {
            fail("libxaac header");
            return false;
        }
        if (done) {
            initDone_ = true;
            break;
        }
        if (n == 0) {
            fail("aac header not found");
            return false;
        }
        // The header/init pass is multi-step; retry a few times while it makes
        // no progress before deciding we need more input.
        if (++retries > 8) {
            if (!eof_) return false;
            fail("aac header not found");
            return false;
        }
    }

    uint32_t rate = 0;
    uint32_t channels = 0;
    callApi(xaac_->api, IA_API_CMD_GET_CONFIG_PARAM, IA_XHEAAC_DEC_CONFIG_PARAM_SAMP_FREQ, &rate);
    callApi(xaac_->api, IA_API_CMD_GET_CONFIG_PARAM, IA_XHEAAC_DEC_CONFIG_PARAM_NUM_CHANNELS,
            &channels);
    sampleRate_ = rate ? rate : inputFormat().sampleRate;
    channels_ = channels ? static_cast<int>(channels) : 2;
    log::info(log::Area::Dec, "aac: {} Hz, {} ch", sampleRate_, channels_);
    return true;
}

void AacDecoder::decodeMore() {
    if (failed_) return;
    if (!initDone_ && !ensureInit()) return;

    for (;;) {
        const size_t n = fillInput();
        if (n == 0 && !eof_) break;  // need more input
        const bool flush = (n == 0);
        if (flush) {
            if (inputOver_) break;  // tail already emitted
            inputOver_ = true;
            callApi(xaac_->api, IA_API_CMD_INPUT_OVER, 0, nullptr);
        }
        uint32_t inBytes = static_cast<uint32_t>(n);
        callApi(xaac_->api, IA_API_CMD_SET_INPUT_BYTES, 0, &inBytes);
        const int32_t err =
            callApi(xaac_->api, IA_API_CMD_EXECUTE, IA_CMD_TYPE_DO_EXECUTE, nullptr);
        uint32_t consumed = 0;
        uint32_t outBytes = 0;
        callApi(xaac_->api, IA_API_CMD_GET_CURIDX_INPUT_BUF, 0, &consumed);
        callApi(xaac_->api, IA_API_CMD_GET_OUTPUT_BYTES, 0, &outBytes);
        if (consumed) {
            consumed_ += consumed;
            compact();
        }
        if (outBytes) appendPcm(outBytes);
        if (isFatal(err)) {
            fail("libxaac frame");
            break;
        }
        // At EOF libxaac keeps re-emitting the tail for zero input: one flush
        // execute is enough.
        if (flush) break;
        if (consumed == 0 && outBytes == 0) break;  // need more input
    }
}

void AacDecoder::feed(std::span<const std::byte> data) {
    if (failed_ || data.empty()) return;
    if (mp4_) {
        mp4_->feed(data);
        if (mp4_->failed()) {
            fail("mp4 demux");
            return;
        }
        mp4_->drain(buffer_);
    } else {
        buffer_.insert(buffer_.end(), data.begin(), data.end());
    }
    decodeMore();
}

void AacDecoder::finish() {
    if (failed_) return;
    eof_ = true;
    if (mp4_) {
        mp4_->finish();
        if (mp4_->failed()) {
            fail("mp4 demux");
            return;
        }
        mp4_->drain(buffer_);
    }
    decodeMore();
}

size_t AacDecoder::drain(std::span<int16_t> out) {
    const size_t n = std::min(pcm_.size(), out.size());
    if (n) {
        std::memcpy(out.data(), pcm_.data(), n * sizeof(int16_t));
        pcm_.erase(pcm_.begin(), pcm_.begin() + static_cast<std::ptrdiff_t>(n));
    }
    return n;
}

size_t AacDecoder::pendingBytes() const {
    const size_t buffered = buffer_.size() - consumed_;
    return buffered + (mp4_ ? mp4_->pending() : 0);
}

PcmFormat AacDecoder::decodedFormat() const {
    return PcmFormat{.sampleRate = sampleRate_,
                     .bitsPerSample = 16,
                     .channels = static_cast<uint8_t>(channels_),
                     .bigEndian = false};
}

}  // namespace squeeze2raop2
