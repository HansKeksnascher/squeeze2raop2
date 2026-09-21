#include "wav_sink.h"

#include "log.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace squeeze2raop2 {

namespace {

void packLe32(char* p, uint32_t v) {
    p[0] = static_cast<char>(v & 0xFF);
    p[1] = static_cast<char>((v >> 8) & 0xFF);
    p[2] = static_cast<char>((v >> 16) & 0xFF);
    p[3] = static_cast<char>((v >> 24) & 0xFF);
}

void packLe16(char* p, uint16_t v) {
    p[0] = static_cast<char>(v & 0xFF);
    p[1] = static_cast<char>((v >> 8) & 0xFF);
}

} // namespace

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
    total_ = 0;
    return true;
}

void PcmFileSink::feed(std::span<const std::byte> data, const PcmFormat& format) {
    if (!fp_ || data.empty()) return;

    if (!headerWritten_) {
        format_ = format;
        std::array<char, 44> header{};
        std::ranges::copy(std::string_view{"RIFF"}, header.begin());
        packLe32(header.data() + 4, 0xFFFFFFFF);
        std::ranges::copy(std::string_view{"WAVEfmt "}, header.begin() + 8);
        packLe32(header.data() + 16, 16);
        packLe16(header.data() + 20, 1);
        packLe16(header.data() + 22, format_.channels);
        packLe32(header.data() + 24, format_.sampleRate);
        uint16_t blockAlign =
            static_cast<uint16_t>(format_.channels * format_.bitsPerSample / 8);
        uint32_t byteRate = format_.sampleRate * blockAlign;
        packLe32(header.data() + 28, byteRate);
        packLe16(header.data() + 32, blockAlign);
        packLe16(header.data() + 34, format_.bitsPerSample);
        std::ranges::copy(std::string_view{"data"}, header.begin() + 36);
        packLe32(header.data() + 40, 0xFFFFFFFF);
        fwrite(header.data(), 1, header.size(), fp_);
        headerWritten_ = true;
        log::info("sink opened {} ({} Hz, {} bit, {} ch)", path_, format_.sampleRate,
                  format_.bitsPerSample, format_.channels);
    }

    if (total_ == 0 && data.size() >= 4 &&
        std::memcmp(data.data(), "RIFF", 4) == 0) {
        if (data.size() >= 44) {
            data = data.subspan(44);
        }
    }

    if (format_.bigEndian && format_.bitsPerSample == 16) {
        std::string swapped;
        swapped.resize(data.size());
        for (size_t i = 0; i + 1 < data.size(); i += 2) {
            swapped[i] = std::to_integer<char>(data[i + 1]);
            swapped[i + 1] = std::to_integer<char>(data[i]);
        }
        fwrite(swapped.data(), 1, swapped.size(), fp_);
    } else {
        fwrite(data.data(), 1, data.size(), fp_);
    }
    total_ += data.size();
}

void PcmFileSink::close() {
    if (fp_) {
        if (headerWritten_ && total_ > 0) {
            // RIFF chunk size = file size - 8 = (44-byte header + data) - 8.
            // WAV tops out at 32-bit sizes; clamp like streaming writers do.
            const uint32_t dataSize = static_cast<uint32_t>(std::min<uint64_t>(total_, 0xFFFFFFFFu));
            const uint32_t riffSize =
                static_cast<uint32_t>(std::min<uint64_t>(total_ + 36, 0xFFFFFFFFu));
            fseek(fp_, 4, SEEK_SET);
            char buf[4];
            packLe32(buf, riffSize);
            fwrite(buf, 1, 4, fp_);
            fseek(fp_, 40, SEEK_SET);
            packLe32(buf, dataSize);
            fwrite(buf, 1, 4, fp_);
        }
        fclose(fp_);
        fp_ = nullptr;
    }
}

} // namespace squeeze2raop2
