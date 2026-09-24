#include "playback/decoder/buffered_decoder.h"

#include "common/log.h"

namespace squeeze2raop2 {

void BufferedDecoder::fail(std::string_view why) {
    if (failed_) return;
    failed_ = true;
    log::error(log::Area::Dec, "{} decode failed: {}", name(), why);
}

}  // namespace squeeze2raop2