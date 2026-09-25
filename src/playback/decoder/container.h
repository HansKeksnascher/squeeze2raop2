#pragma once

// Container/format tags shared by the decoders and the debug sink. Only the
// vocabulary used by more than one translation unit lives here; per-parser
// byte offsets stay local to their parser.

#include <cstddef>
#include <cstdint>

namespace squeeze2raop2 {

// Four-character container tags (RIFF/FORM/...).
constexpr size_t kFourccBytes = 4;

// --- RIFF / WAV -------------------------------------------------------------

constexpr const char* kWavTagRiff = "RIFF";
constexpr const char* kWavTagWave = "WAVE";
constexpr const char* kWavTagFmt = "fmt ";
constexpr const char* kWavTagData = "data";
constexpr size_t kWavHeaderBytes = 44;  // canonical PCM WAV header

// --- AIFF / AIFF-C ----------------------------------------------------------

constexpr const char* kAiffTagForm = "FORM";
constexpr const char* kAiffType = "AIFF";
constexpr const char* kAiffTypeC = "AIFC";
constexpr const char* kAiffTagCommon = "COMM";
constexpr const char* kAiffTagSound = "SSND";

// --- ADTS (AAC) -------------------------------------------------------------

constexpr uint8_t kAdtsSyncByte = 0xFF;   // byte 0
constexpr uint8_t kAdtsSyncMask = 0xF6;   // mask on byte 1 for sync check
constexpr uint8_t kAdtsSyncValue = 0xF0;  // expected (b1 & mask)
constexpr uint8_t kAdtsNoCrcBits = 0xF1;  // MPEG-4, layer 0, no CRC
constexpr size_t kAdtsHeaderBytes = 7;
constexpr uint32_t kAdtsMaxFrameLen = 0x1FFF;  // 13-bit frame length field

}  // namespace squeeze2raop2