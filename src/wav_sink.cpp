#include "wav_sink.h"

#include "byte_order.h"
#include "log.h"
#include "util.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <span>

namespace squeeze2raop2 {

PcmFileSink::PcmFileSink(std::string path) : path_(std::move(path)) {}

PcmFileSink::~PcmFileSink() { close(); }

bool PcmFileSink::open(const PcmFormat& format, std::string& errorOut) {
    format_ = format;
    fp_ = fopen(path_.c_str(), "wb");
    if (!fp_) {
        errorOut = std::string("cannot open ") + path_;
        return false;
    }
    headerWritten_ = false;
    writeFailed_ = false;
    total_ = 0;
    return true;
}

void PcmFileSink::feed(std::span<const std::byte> data, const PcmFormat& format) {
    if (!fp_ || writeFailed_ || data.empty()) return;

    if (!headerWritten_) {
        format_ = format;
        std::array<std::byte, 44> header{};
        auto putTag = [&header](size_t off, std::string_view tag) {
            std::ranges::copy(std::as_bytes(std::span{tag}),
                              header.begin() + static_cast<std::ptrdiff_t>(off));
        };
        putTag(0, "RIFF");
        writeInt<Endian::Little>(header.data() + 4, uint32_t{0xFFFFFFFF});
        putTag(8, "WAVEfmt ");
        writeInt<Endian::Little>(header.data() + 16, uint32_t{16});
        writeInt<Endian::Little>(header.data() + 20, uint16_t{1});
        writeInt<Endian::Little>(header.data() + 22, static_cast<uint16_t>(format_.channels));
        writeInt<Endian::Little>(header.data() + 24, format_.sampleRate);
        const auto blockAlign = static_cast<uint16_t>(format_.channels * format_.bitsPerSample / 8);
        const auto byteRate = format_.sampleRate * blockAlign;
        writeInt<Endian::Little>(header.data() + 28, byteRate);
        writeInt<Endian::Little>(header.data() + 32, blockAlign);
        writeInt<Endian::Little>(header.data() + 34, static_cast<uint16_t>(format_.bitsPerSample));
        putTag(36, "data");
        writeInt<Endian::Little>(header.data() + 40, uint32_t{0xFFFFFFFF});
        if (fwrite(header.data(), 1, header.size(), fp_) != header.size()) {
            writeFailed_ = true;
            log::error("sink write failed {}: {}", path_, errnoMessage(errno));
            return;
        }
        headerWritten_ = true;
        log::info("sink opened {} ({} Hz, {} bit, {} ch)", path_, format_.sampleRate,
                  format_.bitsPerSample, format_.channels);
    }

    if (total_ == 0 && data.size() >= 4 && std::memcmp(data.data(), "RIFF", 4) == 0) {
        if (data.size() >= 44) {
            data = data.subspan(44);
        }
    }

    std::string swapped;
    const char* out = reinterpret_cast<const char*>(data.data());
    size_t outLen = data.size();
    if (format_.bigEndian && format_.bitsPerSample == 16) {
        swapped.resize(data.size());
        for (size_t i = 0; i + 1 < data.size(); i += 2) {
            swapped[i] = std::to_integer<char>(data[i + 1]);
            swapped[i + 1] = std::to_integer<char>(data[i]);
        }
        out = swapped.data();
        outLen = swapped.size();
    }

    if (fwrite(out, 1, outLen, fp_) != outLen) {
        writeFailed_ = true;
        log::error("sink write failed {}: {}", path_, errnoMessage(errno));
        return;
    }
    total_ += outLen;
}

void PcmFileSink::close() {
    if (fp_) {
        if (headerWritten_ && total_ > 0 && !writeFailed_) {
            // RIFF chunk size = file size - 8 = (44-byte header + data) - 8.
            // WAV tops out at 32-bit sizes; clamp like streaming writers do.
            const uint32_t dataSize =
                static_cast<uint32_t>(std::min<uint64_t>(total_, 0xFFFFFFFFu));
            const uint32_t riffSize =
                static_cast<uint32_t>(std::min<uint64_t>(total_ + 36, 0xFFFFFFFFu));
            fseek(fp_, 4, SEEK_SET);
            std::array<std::byte, 4> buf{};
            writeInt<Endian::Little>(buf.data(), riffSize);
            fwrite(buf.data(), 1, buf.size(), fp_);
            fseek(fp_, 40, SEEK_SET);
            writeInt<Endian::Little>(buf.data(), dataSize);
            fwrite(buf.data(), 1, buf.size(), fp_);
        }
        fclose(fp_);
        fp_ = nullptr;
    }
}

}  // namespace squeeze2raop2
