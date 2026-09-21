#include "wav_sink.h"

#include "log.h"

#include <cstdint>
#include <cstring>

namespace sq2 {

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

void PcmFileSink::feed(const char* data, size_t len, const PcmFormat& format) {
    if (!fp_) return;

    if (!headerWritten_) {
        format_ = format;
        char header[44];
        memcpy(header, "RIFF", 4);
        packLe32(header + 4, 0xFFFFFFFF);
        memcpy(header + 8, "WAVEfmt ", 8);
        packLe32(header + 16, 16);
        packLe16(header + 20, 1);
        packLe16(header + 22, format_.channels);
        packLe32(header + 24, format_.sampleRate);
        uint16_t blockAlign = static_cast<uint16_t>(format_.channels * format_.bitsPerSample / 8);
        uint32_t byteRate = format_.sampleRate * blockAlign;
        packLe32(header + 28, byteRate);
        packLe16(header + 32, blockAlign);
        packLe16(header + 34, format_.bitsPerSample);
        memcpy(header + 36, "data", 4);
        packLe32(header + 40, 0xFFFFFFFF);
        fwrite(header, 1, sizeof(header), fp_);
        headerWritten_ = true;
        log::info("sink opened {} ({} Hz, {} bit, {} ch)", path_, format_.sampleRate,
                  format_.bitsPerSample, format_.channels);
    }

    if (total_ == 0 && len >= 4 && memcmp(data, "RIFF", 4) == 0) {
        if (len >= 44) {
            data += 44;
            len -= 44;
        }
    }

    if (format_.bigEndian && format_.bitsPerSample == 16) {
        std::string swapped;
        swapped.resize(len);
        for (size_t i = 0; i + 1 < len; i += 2) {
            swapped[i] = data[i + 1];
            swapped[i + 1] = data[i];
        }
        fwrite(swapped.data(), 1, swapped.size(), fp_);
    } else {
        fwrite(data, 1, len, fp_);
    }
    total_ += len;
}

void PcmFileSink::close() {
    if (fp_) {
        if (headerWritten_ && total_ > 0) {
            uint32_t dataSize = total_;
            uint32_t riffSize = dataSize + 8 + 36 - 8 + 44 - 44 + 36;
            fseek(fp_, 4, SEEK_SET);
            fwrite(&riffSize, 4, 1, fp_);
            fseek(fp_, 40, SEEK_SET);
            fwrite(&dataSize, 4, 1, fp_);
        }
        fclose(fp_);
        fp_ = nullptr;
    }
}

}
