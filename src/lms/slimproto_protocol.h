#pragma once

// LMS slimproto protocol definition: the shared wire payload/enum types, the
// opcode/STAT/command and frame-layout constants, the connection-lifecycle
// tuning, and the audio-format defaults. Kept in a light header so the
// playback pipeline (decoders, sinks, counters) does not pull in the client's
// sockets, threads and mutexes.

#include <cstddef>
#include <cstdint>
#include <string>

namespace squeeze2raop2 {

// --- Stream / PCM types ----------------------------------------------------

enum class StreamFormat : uint8_t {
    Unknown = '?',
    Pcm = 'p',
    Mp3 = 'm',
    Flac = 'f',
    Wma = 'w',
    Ogg = 'o',
    Opus = 'u',
    Aac = 'a',
    Alac = 'l',
};

struct PcmParams {
    uint8_t sampleSizeCode = '?';
    uint8_t sampleRateCode = '?';
    uint8_t channelsCode = '?';
    uint8_t endianCode = '?';

    bool operator==(const PcmParams&) const = default;
};

// Native audio defaults, used wherever a rate/size/channel fallback is needed.
constexpr uint32_t kDefaultSampleRate = 44100;
constexpr uint8_t kDefaultBitsPerSample = 16;
constexpr uint8_t kDefaultChannels = 2;

struct PcmFormat {
    uint32_t sampleRate = kDefaultSampleRate;
    uint8_t bitsPerSample = kDefaultBitsPerSample;
    uint8_t channels = kDefaultChannels;
    bool bigEndian = false;

    bool operator==(const PcmFormat&) const = default;
};

uint32_t sampleRateFromCode(uint8_t code);
uint8_t bitsPerSampleFromCode(uint8_t code);
uint8_t channelsFromCode(uint8_t code);
PcmFormat pcmFormat(const PcmParams& params, uint32_t fallbackRate);

struct StrmStart {
    uint8_t autostart = 1;
    StreamFormat format = StreamFormat::Unknown;
    PcmParams pcm;
    uint8_t thresholdKb = 0;
    uint8_t transitionType = 0;
    uint8_t transitionPeriodS = 0;
    uint8_t flags = 0;
    bool ssl = false;  // flags & kStrmFlagSsl
    uint32_t replayGain = 0;
    uint32_t serverIp = 0;
    uint16_t serverPort = 0;
    std::string request;
};

struct StreamStats {
    uint32_t streamBufferSize = 0;
    uint32_t streamBufferFullness = 0;
    uint64_t bytesReceived = 0;
    uint32_t outputBufferSize = 0;
    uint32_t outputBufferFullness = 0;
    uint32_t elapsedMs = 0;
};

// --- Wire opcodes ----------------------------------------------------------
//
// 4-character slimproto opcodes. Declared as char arrays (5 bytes incl. NUL) so
// they satisfy sendStat()/sendPacket()'s `const char (&)[5]` parameters.

constexpr char kOpHelo[] = "HELO";
constexpr char kOpStat[] = "STAT";
constexpr char kOpResp[] = "RESP";
constexpr char kOpSetd[] = "SETD";
// Server -> player 'setd' (name query/update) is lower-case, distinct from the
// player's upper-case SETD reply.
constexpr char kOpSetdServer[] = "setd";
constexpr char kOpDsco[] = "DSCO";
constexpr char kOpButn[] = "BUTN";
constexpr char kOpMeta[] = "META";
constexpr char kOpBye[] = "BYE!";
constexpr char kOpStrm[] = "strm";
constexpr char kOpCont[] = "cont";
constexpr char kOpCodc[] = "codc";
constexpr char kOpAudg[] = "audg";
constexpr char kOpAude[] = "aude";
constexpr char kOpServ[] = "serv";

// STAT event codes.
constexpr char kStatHeartbeat[] = "STMt";
constexpr char kStatAutostart[] = "STMl";
constexpr char kStatStart[] = "STMs";
constexpr char kStatFlush[] = "STMf";
constexpr char kStatPause[] = "STMp";
constexpr char kStatResume[] = "STMr";
constexpr char kStatDone[] = "STMd";
constexpr char kStatError[] = "STMn";
constexpr char kStatUnderrun[] = "STMo";
constexpr char kStatConnect[] = "STMc";
constexpr char kStatEnd[] = "STMu";

// 'strm' command bytes.
constexpr char kStrmHeartbeat = 't';
constexpr char kStrmStop = 'q';
constexpr char kStrmFlush = 'f';
constexpr char kStrmPause = 'p';
constexpr char kStrmSkip = 'a';
constexpr char kStrmUnpause = 'u';
constexpr char kStrmStart = 's';

// --- Frame layout ----------------------------------------------------------
//
// Client->LMS frames are a 4-byte opcode + a 4-byte big-endian body length;
// server->LMS packets are a 2-byte big-endian length + body.

constexpr size_t kFrameHeaderBytes = 8;
constexpr size_t kOpcodeBytes = 4;
constexpr size_t kLengthFieldOffset = 4;
constexpr size_t kLengthPrefixBytes = 2;
constexpr size_t kMinPacketBytes = 4;
constexpr size_t kMaxPacketBytes = 4096 * 8;

// Fixed body sizes (or minimum accepted lengths) of the packets we build/parse.
constexpr size_t kHeloBodyBytes = 36;
constexpr size_t kStatBodyBytes = 53;
constexpr size_t kSetdBodyBytes = 2;
constexpr size_t kSetdMinBytes = 5;
constexpr size_t kButnBodyBytes = 8;
constexpr size_t kAudgMinBytes = 22;
constexpr size_t kAudeMinBytes = 6;
constexpr size_t kCodcMinBytes = 9;
constexpr size_t kServMinBytes = 8;
constexpr size_t kStrmMinBytes = 5;
constexpr size_t kStrmStartMinBytes = 28;

// 'strm s' flags byte bits we care about (Squeezebox.pm). 0x20 is set on a
// direct stream when the URL is https and the player advertised CanHTTPS=1:
// the transport must use TLS.
constexpr uint8_t kStrmFlagSsl = 0x20;

// HELO flags/fields.
constexpr uint16_t kHeloFlagReconnect = 0x4000;
constexpr uint8_t kHeloDeviceId = 12;  // SqueezePlay player class (squeezelite parity)
constexpr uint8_t kHeloRevision = 1;

// BYE! reason byte for a normal shutdown.
constexpr uint8_t kByeReasonNormal = 0;

// 'strm s' autostart modes: 0 = decoder-ready ack (STMl), >= 2 = wait for a
// 'cont' before starting the pump.
constexpr uint8_t kAutostartDecoderReady = 0;
constexpr uint8_t kAutostartRequireCont = 2;

// Stream server port used when 'strm s' carries no server port (LMS-proxied
// streams always set one; this is the squeezelite default).
constexpr uint16_t kDefaultStreamPort = 9000;

// --- IR button codes -------------------------------------------------------
//
// LMS resolves these against IR/Slim_Devices_Remote.ir and moves its own
// volume mixer one step. The only player->server volume primitive slimproto
// offers, so receiver-initiated changes are chased with button nudges.
constexpr uint32_t kVolUpButton = 0x7689807fu;
constexpr uint32_t kVolDownButton = 0x768900ffu;

// --- Connection lifecycle tuning ------------------------------------------
//
// Defaults for the control connection; config.h may override the watchdog.

constexpr uint16_t kDefaultLmsPort = 3483;
constexpr uint32_t kServerSilenceTimeoutMs = 35000;  // squeezelite parity
constexpr uint32_t kPollTimeoutMs = 100;
constexpr uint32_t kRetryTickMs = 100;
constexpr uint32_t kHeartbeatIntervalMs = 1000;
constexpr int kKeepAliveIdleSec = 30;
constexpr int kKeepAliveIntervalSec = 10;
constexpr int kKeepAliveCount = 6;
constexpr uint32_t kDiscoveryTimeoutMs = 5000;
constexpr uint32_t kDiscoveryPollWaitMs = 2000;
constexpr int kDiscoveryRetryIterations = 50;
constexpr int kReconnectBackoffFactorSec = 2;
constexpr int kReconnectBackoffCapSec = 15;
constexpr size_t kReadDrainBufferBytes = 2048;

}  // namespace squeeze2raop2