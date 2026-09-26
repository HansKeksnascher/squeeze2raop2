#include "common/util.h"
#include "lms/slimproto_protocol.h"

#include "check.h"

#include <array>
#include <cstdint>
#include <string>

using namespace squeeze2raop2::test;
using namespace squeeze2raop2;

SQ2_TEST(util, pcm_codes) {
    expect(sampleRateFromCode('3') == 44100, "rate code 3 -> 44100");
    expect(sampleRateFromCode('9') == 96000, "rate code 9 -> 96000");
    expect(bitsPerSampleFromCode('1') == 16, "depth code 1 -> 16");
    expect(channelsFromCode('2') == 2, "channels code 2 -> 2");
    PcmParams p{'1', '3', '2', '0'};
    PcmFormat f = pcmFormat(p, 48000);
    expect(f.bitsPerSample == 16 && f.sampleRate == 44100 && f.channels == 2 && f.bigEndian,
           "known codes resolve");
    PcmParams unknown{'?', '?', '?', '?'};
    PcmFormat d = pcmFormat(unknown, 48000);
    expect(d.sampleRate == 48000 && d.bitsPerSample == 16 && d.channels == 2 && !d.bigEndian,
           "unknown codes fall back to the input format");
}

SQ2_TEST(util, string_helpers) {
    expect(trimView("  a b\t") == "a b", "trim both ends");
    expect(trimView("") == "", "empty stays empty");
    expect(trimView("x") == "x", "no whitespace untouched");
    expect(trimView(" \r\n ") == "", "all-whitespace trims to empty");
    expect(toLower("AbC-12") == "abc-12", "ascii lowercased");
    expect(toLower("") == "", "empty lowercases to empty");
}

SQ2_TEST(util, mac) {
    const auto m = fakeMacFor("kitchen");
    expect(m[0] == 0xaa, "virtual mac has the aa prefix");
    expect(fakeMacFor("kitchen") == m, "virtual mac is stable");
    expect(fakeMacFor("living room") != m, "distinct names yield distinct macs");

    std::array<uint8_t, 6> parsed{};
    expect(macFromString("aa:01:02:03:04:05", parsed), "well-formed mac parses");
    expect(parsed[0] == 0xaa && parsed[5] == 0x05, "mac parsed byte-for-byte");
    expect(!macFromString("zz:01:02:03:04:05", parsed), "non-hex rejected");
    expect(!macFromString("aa:01:02:03:04:05junk", parsed), "trailing junk rejected");
    expect(!macFromString("100:01:02:03:04:05", parsed), "wide octet rejected");
    expect(!macFromString("aa:01", parsed), "short mac rejected");
    expect(!macFromString("aa:01:02:03:04:05:", parsed), "trailing colon rejected");

    std::array<uint8_t, 6> upper{};
    expect(macFromString("AA:0A:BC:DE:F0:12", upper), "uppercase hex accepted");
    expect(upper[0] == 0xAA && upper[5] == 0x12, "uppercase parsed byte-for-byte");
}

SQ2_TEST(util, url_decode) {
    // well-formed percent escapes and '+'-for-space
    expect(urlDecode("a%20b+c") == "a b c", "percent and plus");
    expect(urlDecode("%2B") == "+", "escaped plus");
    expect(urlDecode("%00") == std::string(1, '\0'), "nul escape");
    // malformed escapes pass through literally
    expect(urlDecode("100%") == "100%", "trailing percent");
    expect(urlDecode("%zz") == "%zz", "non-hex escape");
    expect(urlDecode("%2") == "%2", "short escape");
    expect(urlDecode("%2g") == "%2g", "half-hex escape");
    // unchanged strings
    expect(urlDecode("plain") == "plain", "plain passthrough");
    expect(urlDecode("").empty(), "empty stays empty");
}

SQ2_TEST(util, parse_txt_key_values) {
    // wire format: (len byte, len-1 data bytes); first occurrence of a key wins
    const std::string wire = std::string("\x06", 1) + "br=128" + std::string("\x03", 1) + "a=b" +
                             std::string("\x02", 1) + "br" + std::string("\x08", 1) + "name=x=y";
    auto txt = parseTxtKeyValues(wire);
    expect(txt.size() == 3, "three records parsed");
    expect(txt["br"] == "128", "first occurrence wins");
    expect(txt["a"] == "b", "simple pair");
    expect(txt["name"] == "x=y", "only the first '=' splits");

    // key without '=' yields an empty value
    txt = parseTxtKeyValues(std::string("\x03", 1) + "key");
    expect(txt.size() == 1 && txt["key"] == "", "key without '=' is empty");

    // zero-length or truncated records stop the parse cleanly
    txt = parseTxtKeyValues(std::string("\x00", 1) + "ignored");
    expect(txt.empty(), "zero-length record stops");
    txt = parseTxtKeyValues(std::string("\x0A", 1) + "short");
    expect(txt.empty(), "truncated record stops");
    txt = parseTxtKeyValues(std::string("\x05", 1) + "ab=cd" + std::string("\xFF", 1) + "junk");
    expect(txt.size() == 1 && txt["ab"] == "cd", "stops before an oversized record");
}