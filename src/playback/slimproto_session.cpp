#include "playback/slimproto_session.h"

#include "app/version.h"
#include "common/transport.h"
#include "playback/decoder/decoder.h"

#include <utility>

namespace squeeze2raop2 {

namespace {

// The highest input sample rate the decoders accept: PCM and AAC can carry up
// to 96 kHz, and the sender resamples the decoded PCM down to the receiver's
// rate. Advertised so LMS picks a stream rate <= this instead of handing over
// a source it cannot decode.
constexpr uint32_t kMaxSampleRate = 96000;

}  // namespace

SlimProtoSession::SlimProtoSession(const ResolvedPlayerConfig& cfg, const GlobalConfig& global,
                                   AirplayOutput& output, NameSink nameSink, Delegate& delegate)
    : mac_(cfg.mac),
      lmsHost_(global.lmsHost),
      lmsPort_(global.lmsPort),
      serverTimeoutMs_(global.serverTimeoutMs),
      name_(cfg.name),
      nameSink_(std::move(nameSink)),
      output_(output),
      delegate_(delegate) {}

SlimProtoSession::~SlimProtoSession() { stop(); }

void SlimProtoSession::setStatsProvider(std::function<StreamStats()> provider) {
    statsProvider_ = std::move(provider);
    if (client_) client_->setStatsProvider(statsProvider_);
}

// squeezelite-style caps: Model/ModelName drive the LMS web UI (player lists,
// settings); the player's display name is sent separately via SETD name.
// Firmware= is the free-text version LMS shows in player settings. ModelName
// carries the AirPlay transport in use so the LMS UI distinguishes the classic
// RAOP/AP1 path from the native AirPlay 2 path. CanHTTPS=1 makes LMS hand over
// direct https radio URLs (strm s with the 0x20 flag) instead of proxying them;
// only advertised when TLS is usable.
std::string SlimProtoSession::buildCaps() const {
    // A player can register with LMS before discovery has pinned a target (a
    // static player without one); it cannot play until a target arrives, so
    // advertise no transport suffix until then. Once the target is known the
    // suffix tells the LMS UI which path is in use.
    const std::string modelName =
        output_.hasTarget() ? (output_.airplay2() ? "squeeze2raop2@ap2" : "squeeze2raop2@raop")
                            : "squeeze2raop2";
    // Advertise exactly the codecs this build decodes, from the same
    // supportedCodecs() list the factory and the strm guard use (so the caps
    // cannot drift from what is decodable). Never wav/aif/flc/alc.
    std::string caps;
    if (tlsUsable()) caps = "CanHTTPS=1,";
    caps +=
        "Model=squeezelite,ModelName=" + modelName +
        ",AccuratePlayPoints=1,HasDigitalOut=1,MaxSampleRate=" + std::to_string(kMaxSampleRate) +
        ",Firmware=squeeze2raop2 " SQUEEZE2RAOP2_VERSION;
    for (const CodecInfo& codec : supportedCodecs()) caps += std::string(",") + codec.capToken;
    return caps;
}

void SlimProtoSession::start() {
    SlimProtoClient::Events events;
    // The stream events are policy-free: the Delegate decides. 'stop' and
    // 'flush' also answer LMS with STMf here, at the same points the session
    // used to (stop: before the fade decision; flush: after dropping audio).
    events.onStart = [this](const StrmStart& st) { delegate_.onStreamStart(st); };
    events.onCont = [this](uint32_t) { delegate_.onCont(); };
    events.onStop = [this]() {
        stat("STMf");
        delegate_.onStop();
    };
    events.onFlush = [this](bool) {
        delegate_.onFlush();
        stat("STMf");
    };
    events.onPause = [this](uint32_t ms) { delegate_.onPause(ms); };
    events.onUnpause = [this](uint32_t ms) { delegate_.onUnpause(ms); };
    events.onSkipAhead = [this](uint32_t ms) { delegate_.onSkipAhead(ms); };
    events.onCodc = [this](StreamFormat format, const PcmParams& pcm) {
        delegate_.onCodc(format, pcm);
    };
    events.onAude = [this](bool enable) { delegate_.onAude(enable); };
    events.onSetName = [this](const std::string& n) {
        name_ = n;
        client_->setPlayerName(n);
        if (nameSink_) nameSink_(n);
    };
    events.onVolume = [this](double l, double r) { delegate_.onLmsVolume(l, r); };

    client_ = std::make_unique<SlimProtoClient>(mac_, buildCaps(), std::move(events));
    client_->setPlayerName(name_);
    client_->setServerTimeout(serverTimeoutMs_);
    if (statsProvider_) client_->setStatsProvider(statsProvider_);
    client_->start(lmsHost_.value_or(""), lmsPort_);
}

void SlimProtoSession::stop() {
    if (client_) client_->stop();
}

void SlimProtoSession::stat(const char (&event)[5]) {
    if (!client_) return;
    client_->sendStat(event, statsProvider_ ? statsProvider_() : StreamStats{});
}

void SlimProtoSession::resp(std::string_view header) {
    if (client_) client_->sendResp(std::string(header));
}

void SlimProtoSession::disco(DisconnectCode code) {
    if (client_) client_->sendDisco(static_cast<uint8_t>(code));
}

void SlimProtoSession::meta(std::string_view block) {
    if (client_) client_->sendMeta(block);
}

void SlimProtoSession::button(uint32_t code) {
    if (client_) client_->sendButton(code);
}

const std::string& SlimProtoSession::serverHost() const { return client_->serverHost(); }

}  // namespace squeeze2raop2