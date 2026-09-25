#pragma once

#include "airplay/raop_types.h"
#include "app/config.h"
#include "playback/slimproto_session.h"
#include "playback/stream_coordinator.h"
#include "playback/stream_counters.h"
#include "playback/volume_controller.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace squeeze2raop2 {

// One LMS player <-> AirPlay receiver pairing. This is only the composition
// root: it owns the four collaborators and wires them together, then forwards
// the SlimProtoSession events to the stream coordinator and volume controller.
// The LMS protocol lives in SlimProtoSession, the per-track policy in
// StreamCoordinator and the volume policy in VolumeController.
class PlayerSession final : public SlimProtoSession::Delegate {
public:
    PlayerSession(const ResolvedPlayerConfig& cfg, const GlobalConfig& global,
                  VolumeAnchors anchors, std::optional<std::string> sinkPath,
                  std::optional<RaopTarget> raopTarget, CredentialSink credSink, NameSink nameSink);
    ~PlayerSession();
    PlayerSession(const PlayerSession&) = delete;
    PlayerSession& operator=(const PlayerSession&) = delete;

    void start();
    void updateTarget(RaopTarget t);
    void stop();

    // Transport currently in use (native AirPlay 2 vs classic RAOP), taken from
    // the live target. Used to detect a discovery race that changed it.
    bool airplay2() const { return output_->airplay2(); }

private:
    // SlimProtoSession::Delegate: forward the reader-thread events to the
    // stream coordinator / volume controller.
    void onStreamStart(const StrmStart& st) override;
    void onCont() override;
    void onStop() override;
    void onFlush() override;
    void onPause(uint32_t ms) override;
    void onUnpause(uint32_t ms) override;
    void onSkipAhead(uint32_t ms) override;
    void onCodc(StreamFormat format, const PcmParams& pcm) override;
    void onAude(bool enable) override;
    void onLmsVolume(double l, double r) override;

    // Declaration order is the teardown guarantee: stream_ (and its thread) is
    // destroyed first, then volume_ (its chaser may call slim_), then slim_
    // (its reader thread), then the output/counters the callbacks touch.
    std::unique_ptr<AirplayOutput> output_;
    StreamCounters counters_;
    std::unique_ptr<SlimProtoSession> slim_;
    std::unique_ptr<VolumeController> volume_;
    std::unique_ptr<StreamCoordinator> stream_;
};

}  // namespace squeeze2raop2