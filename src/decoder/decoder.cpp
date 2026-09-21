#include "decoder/decoder.h"

#include "decoder/mp3_decoder.h"
#include "decoder/pcm_decoder.h"

namespace squeeze2raop2 {

std::unique_ptr<Decoder> Decoder::create(StreamFormat format,
                                         const PcmFormat& in) {
    switch (format) {
    case StreamFormat::Mp3: return std::make_unique<Mp3Decoder>();
    case StreamFormat::Pcm: return std::make_unique<PcmDecoder>(in);
    default: return nullptr;
    }
}

} // namespace squeeze2raop2