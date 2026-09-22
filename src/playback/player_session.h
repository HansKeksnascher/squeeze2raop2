#pragma once

#include "airplay/airplay_output.h"
#include "app/config.h"
#include "playback/playback_stream.h"
#include "lms/slimproto.h"
#include "playback/stream_counters.h"
#include "playback/volume_map.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace squeeze2raop2 {

// Why the stream loop ended, and what the exit path should do about it.
enum class ExitAction : std::uint8_t {
    SilentStop,  // stop request / shutdown: no STAT (handlers already told LMS)
    Retry,       // receiver-initiated loss, first time: recreate and resume
    GaveUp,      // receiver loss again after the one retry: report STMd
    EndedEof,    // natural end: STMd, drain the tail, STMu
    EndedError,  // socket/decode error: report STMu
};

struct ExitInputs {
    bool stopping;    // stop_requested() || !g_run
    bool lost;        // receiver closed the session
    bool retryUsed;   // the one transparent retry is already spent
    bool reachedEof;  // HTTP EOF with the decoder drained
};

[[nodiscard]] constexpr ExitAction decideExit(const ExitInputs& in) {
    if (in.stopping) return ExitAction::SilentStop;
    if (in.lost) return in.retryUsed ? ExitAction::GaveUp : ExitAction::Retry;
    if (in.reachedEof) return ExitAction::EndedEof;
    return ExitAction::EndedError;
}

// One LMS player <-> AirPlay receiver pairing: a SlimProtoClient (control
// connection to LMS) plus a lazily-created PlaybackStream per track and an
// AirplayOutput (sender + ring). This class is the player-level policy: the
// LMS protocol, the volume mapping and the retry/flush exit handling. Event
// callbacks run on the SlimProtoClient's reader thread; the track pump runs on
// streamThread_.
class PlayerSession {
public:
    PlayerSession(std::string deviceId, std::string name, std::array<uint8_t, 6> mac,
                  std::optional<std::string> lmsHost, uint16_t lmsPort, bool paceRealtime,
                  std::optional<std::string> sinkPath, std::optional<RaopTarget> raopTarget,
                  CredentialSink credSink, VolumeMode volumeMode, VolumeAnchors anchors,
                  float volPct, int latencyMs);
    ~PlayerSession();
    PlayerSession(const PlayerSession&) = delete;
    PlayerSession& operator=(const PlayerSession&) = delete;

    void start();
    void updateTarget(RaopTarget t);
    void stop();

private:
    StreamStats currentStats();
    void startStream(const StrmStart& st);
    void streamLoop(std::stop_token st);
    void launchAirplaySession();
    // Blocks (bounded) until the sender ring has played out, so the receiver
    // finishes the track tail before we report the end of playback.
    void waitForOutputDrain(std::stop_token st);
    void stopPlayback();

    std::string deviceId_;
    std::string name_;
    std::array<uint8_t, 6> mac_{};
    std::optional<std::string> lmsHost_;
    uint16_t lmsPort_;
    bool paceRealtime_;
    std::optional<std::string> sinkPath_;

    std::unique_ptr<SlimProtoClient> client_;

    // The live AirPlay connection: sender, ring, target/credentials and the
    // volume/metadata application (owns its own cross-thread synchronization).
    std::unique_ptr<AirplayOutput> output_;
    // Byte/frame accounting + the STAT snapshot (own synchronization); owned
    // here so the last track's figures survive until the next stream.
    StreamCounters counters_;

    // The current track's pipeline; replaced per stream, closed by stopPlayback.
    std::unique_ptr<PlaybackStream> track_;
    std::jthread streamThread_;
    std::atomic<bool> autostartPending_{false};

    // Session resilience flags (see streamLoop exit handling + callbacks).
    std::atomic<bool> flushed_{false};    // strm f: keep session for next track
    std::atomic<bool> retryUsed_{false};  // one transparent retry per stream

    // --vol-pct: fixed-mode level, and in lms mode the pre-AUDG fallback
    // applied to every new session before RECORD (so audio never starts at
    // the receiver's hardware default).
    VolumeAnchors anchors_;  // --vol-map dB anchors over the LMS slider
    VolumeMode volumeMode_;
    float fixedVolumePct_;
    // Last LMS slider percent seen via AUDG (lms mode); 0 = none. Re-applied
    // by launchAirplaySession() on session recreation. Mute pushes (0) are not
    // stored, so LMS's end-of-fade zero gain can't mute the next session.
    std::atomic<double> lastLmsPct_{0.0};
};

}  // namespace squeeze2raop2
