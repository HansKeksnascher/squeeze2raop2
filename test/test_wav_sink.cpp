#include "wav_sink.h"

#include "check.h"

#include <unistd.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace sq2t;
using squeeze2raop2::PcmFileSink;
using squeeze2raop2::PcmFormat;

namespace {

class ScratchDir {
public:
    ScratchDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("squeeze2raop2_wav_test_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~ScratchDir() { std::filesystem::remove_all(path_); }

    std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
};

std::vector<unsigned char> readFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    expect(f != nullptr, "open written wav for reading");
    std::vector<unsigned char> bytes;
    unsigned char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    return bytes;
}

uint32_t le32(const std::vector<unsigned char>& b, size_t off) {
    expect(b.size() >= off + 4, "file large enough for u32 read");
    return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}

uint16_t le16(const std::vector<unsigned char>& b, size_t off) {
    expect(b.size() >= off + 2, "file large enough for u16 read");
    return static_cast<uint16_t>(static_cast<uint16_t>(b[off]) |
                                 static_cast<uint16_t>(static_cast<unsigned>(b[off + 1]) << 8));
}

void expectTag(const std::vector<unsigned char>& b, size_t off, std::string_view tag) {
    expect(b.size() >= off + tag.size(), "file large enough for tag read");
    for (size_t i = 0; i < tag.size(); ++i)
        expect(b[off + i] == static_cast<unsigned char>(tag[i]), "tag matches at offset");
}

}  // namespace

// Pins the RIFF header layout, including the audit's dataSize+36 fix.
static void testLeHeaderAndPayload(const ScratchDir& dir) {
    const PcmFormat fmt{44100, 16, 2, false};
    const std::array<unsigned char, 8> payload{0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00};

    PcmFileSink sink(dir.file("le.wav"));
    std::string error;
    expect(sink.open(fmt, error), "open LE sink");
    sink.feed(std::as_bytes(std::span{payload}), fmt);
    sink.close();

    const auto bytes = readFile(dir.file("le.wav"));
    expect(bytes.size() == 44 + payload.size(), "44-byte header plus payload");
    expectTag(bytes, 0, "RIFF");
    expect(le32(bytes, 4) == 36 + payload.size(), "RIFF chunk size = dataSize + 36");
    expectTag(bytes, 8, "WAVE");
    expectTag(bytes, 12, "fmt ");
    expect(le32(bytes, 16) == 16, "fmt chunk is 16 bytes");
    expect(le16(bytes, 20) == 1, "PCM format tag");
    expect(le16(bytes, 22) == fmt.channels, "channel count in header");
    expect(le32(bytes, 24) == fmt.sampleRate, "sample rate in header");
    expect(le32(bytes, 28) == fmt.sampleRate * fmt.channels * (fmt.bitsPerSample / 8),
           "byte rate in header");
    expect(le16(bytes, 32) == fmt.channels * (fmt.bitsPerSample / 8), "block align in header");
    expect(le16(bytes, 34) == fmt.bitsPerSample, "bits per sample in header");
    expectTag(bytes, 36, "data");
    expect(le32(bytes, 40) == payload.size(), "data chunk size");
    for (size_t i = 0; i < payload.size(); ++i)
        expect(bytes[44 + i] == payload[i], "LE payload written verbatim");
    expect(sink.bytesTotal() == payload.size(), "bytesTotal tracks payload");
}

// 16-bit big-endian input must be byte-swapped into the LE file.
static void testBeSwap(const ScratchDir& dir) {
    const PcmFormat fmt{44100, 16, 2, true};
    const std::array<unsigned char, 4> input{0x12, 0x34, 0xAB, 0xCD};

    PcmFileSink sink(dir.file("be.wav"));
    std::string error;
    expect(sink.open(fmt, error), "open BE sink");
    sink.feed(std::as_bytes(std::span{input}), fmt);
    sink.close();

    const auto bytes = readFile(dir.file("be.wav"));
    expect(bytes.size() == 44 + input.size(), "BE file size");
    expect(bytes[44] == 0x34 && bytes[45] == 0x12, "first sample swapped");
    expect(bytes[46] == 0xCD && bytes[47] == 0xAB, "second sample swapped");
}

// An incoming stream that already starts with a RIFF header gets its 44-byte
// header stripped on the first feed (total_ == 0 gate).
static void testRiffHeaderStripped(const ScratchDir& dir) {
    const PcmFormat fmt{44100, 16, 2, false};
    std::vector<unsigned char> incoming(44 + 6, 0x00);
    incoming[0] = 'R';
    incoming[1] = 'I';
    incoming[2] = 'F';
    incoming[3] = 'F';
    const std::string tail = "hello!";
    for (size_t i = 0; i < tail.size(); ++i) incoming[44 + i] = static_cast<unsigned char>(tail[i]);

    PcmFileSink sink(dir.file("stripped.wav"));
    std::string error;
    expect(sink.open(fmt, error), "open stripping sink");
    sink.feed(std::as_bytes(std::span{incoming}), fmt);
    sink.close();

    const auto bytes = readFile(dir.file("stripped.wav"));
    expect(bytes.size() == 44 + tail.size(), "incoming RIFF header stripped, payload kept");
    for (size_t i = 0; i < tail.size(); ++i)
        expect(bytes[44 + i] == static_cast<unsigned char>(tail[i]), "payload after strip");
}

int main() {
    ScratchDir dir;
    testLeHeaderAndPayload(dir);
    testBeSwap(dir);
    testRiffHeaderStripped(dir);
    std::printf("ok\n");
    return 0;
}
