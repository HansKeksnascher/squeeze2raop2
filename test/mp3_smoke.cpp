#include "mp3_decoder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

using sq2::Mp3Decoder;

namespace {

void writeWav(const std::string& path, uint32_t rate, int ch, const std::vector<int16_t>& data) {
    std::ofstream f(path, std::ios::binary);
    uint32_t byteLen = static_cast<uint32_t>(data.size() * 2);
    auto u16 = [](uint32_t v, std::ofstream& o) {
        char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
        o.write(b, 2);
    };
    auto u32 = [](uint32_t v, std::ofstream& o) {
        char b[4] = {static_cast<char>(v), static_cast<char>(v >> 8),
                     static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
        o.write(b, 4);
    };
    f.write("RIFF", 4);
    u32(36 + byteLen, f);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    u32(16, f);
    u16(1, f);
    u16(static_cast<uint16_t>(ch), f);
    u32(rate, f);
    u32(rate * ch * 2, f);
    u16(static_cast<uint16_t>(ch * 2), f);
    u16(16, f);
    f.write("data", 4);
    u32(byteLen, f);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size() * 2));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: mp3_smoke <in.mp3> <out.wav>\n";
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 2;
    }

    Mp3Decoder dec;
    std::vector<int16_t> pcmAll;
    std::vector<uint8_t> buf(4096);
    bool rateWarned = false;

    while (in) {
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        size_t got = static_cast<size_t>(in.gcount());
        if (!got) break;
        dec.feed(buf.data(), got);

        if (dec.valid() && !rateWarned) {
            std::cout << "stream header: " << dec.sampleRate() << " Hz, " << dec.channels()
                      << " ch\n";
            rateWarned = true;
        }

        std::vector<int16_t> chunk(8192);
        for (;;) {
            size_t n = dec.drain(chunk.data(), chunk.size());
            if (!n) break;
            pcmAll.insert(pcmAll.end(), chunk.begin(), chunk.begin() + static_cast<long>(n));
        }
        if (dec.hasError()) {
            std::cerr << "decode error\n";
            return 1;
        }
    }
    // End of input: decode the remaining tail frames, then drain leftovers.
    dec.finish();
    std::vector<int16_t> chunk(8192);
    for (;;) {
        size_t n = dec.drain(chunk.data(), chunk.size());
        if (!n) break;
        pcmAll.insert(pcmAll.end(), chunk.begin(), chunk.begin() + static_cast<long>(n));
    }

    if (!dec.valid()) {
        std::cerr << "never got a valid MP3 header\n";
        return 1;
    }
    const size_t bytes = pcmAll.size() * 2;
    std::cout << "samples=" << pcmAll.size()
              << " bytes=" << bytes
              << " rate=" << dec.sampleRate()
              << " ch=" << dec.channels()
              << " pendingBytes=" << dec.pendingBytes() << "\n";

    int maxAbs = 0;
    for (int16_t s : pcmAll) {
        int v = std::abs(s);
        if (v > maxAbs) maxAbs = v;
    }
    std::cout << "peak=" << maxAbs << "\n";

    writeWav(argv[2], dec.sampleRate(), dec.channels(), pcmAll);
    return 0;
}
