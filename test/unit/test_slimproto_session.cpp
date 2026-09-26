// Pins the HELO capability string SlimProtoSession advertises to LMS
// (slimproto_session.cpp::buildCaps): the Model/ModelName transport suffix, the
// optional CanHTTPS=1, and — most importantly — that the codec cap tokens and
// their order cannot drift from Decoder::supportedCodecs() (the same list the
// factory and the strm-format guard use).

#include "playback/slimproto_session.h"

#include "airplay/airplay_output.h"
#include "common/transport.h"
#include "playback/decoder/decoder.h"

#include "check.h"

#include <string>

using namespace squeeze2raop2;
using namespace squeeze2raop2::test;

namespace squeeze2raop2 {

// Grants access to the private buildCaps() (test-only friend).
struct SlimProtoSessionTestAccess {
    static std::string caps(const SlimProtoSession& s) { return s.buildCaps(); }
};

}  // namespace squeeze2raop2

namespace {

struct StubDelegate : SlimProtoSession::Delegate {
    void onStreamStart(const StrmStart&) override {}
    void onCont() override {}
    void onStop() override {}
    void onFlush() override {}
    void onPause(uint32_t) override {}
    void onUnpause(uint32_t) override {}
    void onSkipAhead(uint32_t) override {}
    void onCodc(StreamFormat, const PcmParams&) override {}
    void onAude(bool) override {}
    void onLmsVolume(double, double) override {}
};

// A session with no receiver target: ModelName has no transport suffix.
std::string capsWithoutTarget(AirplayOutput& output, StubDelegate& delegate) {
    ResolvedPlayerConfig cfg;
    cfg.name = "Kitchen";
    cfg.mac = {0xaa, 0, 0, 0, 0, 0x01};
    GlobalConfig global;
    SlimProtoSession session(cfg, global, output, NameSink{}, delegate);
    return SlimProtoSessionTestAccess::caps(session);
}

}  // namespace

SQ2_TEST(slimproto_session, caps_codec_tokens_and_order) {
    AirplayOutput output("test", "AABBCCDDEEFF", std::nullopt, CredentialSink{}, 50);
    StubDelegate delegate;
    const std::string caps = capsWithoutTarget(output, delegate);

    // The advertised codec tokens must equal supportedCodecs() exactly, in
    // order. This is the list LMS uses to pick a format for the stream.
    std::string expected = ",pcm";
#if defined(SQUEEZE2RAOP2_WITH_MP3)
    expected += ",mp3";
#endif
#if defined(SQUEEZE2RAOP2_WITH_AAC)
    expected += ",aac";
#endif
#if defined(SQUEEZE2RAOP2_WITH_OGG)
    expected += ",ogg";
#endif
#if defined(SQUEEZE2RAOP2_WITH_OPUS)
    expected += ",ops";
#endif

    std::string actual;
    for (const CodecInfo& codec : supportedCodecs()) {
        actual += ",";
        actual += codec.capToken;
    }
    expect(actual == expected, "codec cap tokens and order match the build");

    // No container/codec LMS can hand us that we do not decode.
    for (const char* bad : {"wav", "aif", "flc", "alc", "wma"})
        expect(caps.find(bad) == std::string::npos, "caps omit an undecodable token");

    // The caps string ends with exactly those tokens.
    expect(caps.size() >= actual.size() &&
               caps.compare(caps.size() - actual.size(), actual.size(), actual) == 0,
           "caps end with the codec tokens in order");
}

SQ2_TEST(slimproto_session, caps_model_and_https) {
    AirplayOutput output("test", "AABBCCDDEEFF", std::nullopt, CredentialSink{}, 50);
    StubDelegate delegate;
    const std::string caps = capsWithoutTarget(output, delegate);

    expect(caps.find("Model=squeezelite") != std::string::npos, "advertises the player model");
    expect(caps.find("MaxSampleRate=96000") != std::string::npos, "advertises the max input rate");
    expect(caps.find("Firmware=squeeze2raop2 ") != std::string::npos, "advertises the firmware");
    // No target yet: no transport suffix, so the LMS UI cannot claim a path.
    expect(caps.find("ModelName=squeeze2raop2,") != std::string::npos,
           "target-less session has no transport suffix");

    // CanHTTPS=1 is advertised only when the build can actually do TLS.
    const bool https = caps.rfind("CanHTTPS=1,", 0) == 0;
    expect(https == tlsUsable(), "CanHTTPS gating matches tlsUsable()");
}

SQ2_TEST(slimproto_session, caps_model_name_by_transport) {
    AirplayOutput output("test", "AABBCCDDEEFF", std::nullopt, CredentialSink{}, 50);
    StubDelegate delegate;

    RaopTarget target;
    target.host = "127.0.0.1";
    target.port = 5001;
    target.airplay2 = true;
    output.updateTarget(target);
    expect(output.hasTarget(), "target applied");
    expect(capsWithoutTarget(output, delegate).find("ModelName=squeeze2raop2@ap2,") !=
               std::string::npos,
           "ap2 target advertises the ap2 suffix");

    target.airplay2 = false;
    output.updateTarget(target);
    expect(capsWithoutTarget(output, delegate).find("ModelName=squeeze2raop2@raop,") !=
               std::string::npos,
           "raop target advertises the raop suffix");
}
