#include "playback/player_session.h"

#include "common/util.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace squeeze2raop2 {

PlayerSession::PlayerSession(const ResolvedPlayerConfig& cfg, const GlobalConfig& global,
                             VolumeAnchors anchors, std::optional<std::string> sinkPath,
                             std::optional<RaopTarget> raopTarget, CredentialSink credSink,
                             NameSink nameSink) {
    // Compute the receiver identity before any thread exists (the sender reads
    // it): uppercase hex MAC with the separators removed.
    std::string identity = macToString(cfg.mac);
    std::ranges::transform(identity, identity.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::erase(identity, ':');
    output_ = std::make_unique<AirplayOutput>(cfg.name, std::move(identity), std::move(raopTarget),
                                              std::move(credSink), cfg.latencyMs);
    slim_ = std::make_unique<SlimProtoSession>(cfg, global, *output_, std::move(nameSink), *this);
    // Volume mapping and the receiver-initiated chase; the chaser's button
    // presses go out on the slimproto client (created in start()).
    volume_ = std::make_unique<VolumeController>(
        *output_, std::move(anchors), cfg.volumeMode, cfg.volPct, global.volumeFeedback,
        [this] { return slim_->alive(); }, [this](uint32_t code) { slim_->button(code); });
    stream_ = std::make_unique<StreamCoordinator>(*output_, counters_, *slim_, *volume_,
                                                  global.sourceTimeoutMs, cfg.paceRealtime,
                                                  std::move(sinkPath));
}

PlayerSession::~PlayerSession() { stop(); }

void PlayerSession::start() {
    slim_->setStatsProvider([this] { return stream_->currentStats(); });
    slim_->start();
    // the AirPlay session is prepared lazily when the first audio arrives;
    // connecting eagerly hits receivers that immediately drop idle sessions
    // (HomePod/Sonos)
    volume_->start();
}

void PlayerSession::updateTarget(RaopTarget t) { output_->updateTarget(std::move(t)); }

void PlayerSession::stop() {
    // Join the stream thread first: it reads output_ throughout streamLoop, so
    // the session must outlive it. Then drop any in-flight receiver volume
    // chase (the target is stale once the LMS connection goes away) before the
    // reader thread is joined last.
    stream_->shutdown();
    volume_->stop();
    output_->stop(false);
    slim_->stop();
}

void PlayerSession::onStreamStart(const StrmStart& st) { stream_->onStreamStart(st); }

void PlayerSession::onCont() { stream_->onCont(); }

void PlayerSession::onStop() { stream_->requestStopOrFade(); }

void PlayerSession::onFlush() { stream_->flush(); }

void PlayerSession::onPause(uint32_t ms) { stream_->pause(ms); }

void PlayerSession::onUnpause(uint32_t ms) { stream_->unpause(ms); }

void PlayerSession::onSkipAhead(uint32_t ms) { stream_->skipAhead(ms); }

void PlayerSession::onCodc(StreamFormat format, const PcmParams& pcm) {
    stream_->onCodc(format, pcm);
}

void PlayerSession::onAude(bool enable) {
    // A lazy session recreation covers power-on.
    if (!enable) stream_->audeOff();
}

void PlayerSession::onLmsVolume(double l, double r) { volume_->onLmsVolume(l, r); }

}  // namespace squeeze2raop2