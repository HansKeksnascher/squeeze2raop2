#pragma once

#include "airplay/airplay_output.h"
#include "airplay/raop_types.h"
#include "app/config.h"
#include "lms/slimproto.h"
#include "playback/playback_stream.h"
#include "playback/stream_counters.h"
#include "playback/volume_map.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
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
    StreamStats currentStats();
    void startStream(const StrmStart& st);
    void maybeStartPump();
    void onDecoderReady();
    void onPrebufferReady();
    void streamLoop(std::stop_token st);
    void launchAirplaySession();
    // Receiver-initiated volume (AP2 event channel, sender thread): convert the
    // receiver's unit volume to the LMS slider it corresponds to and start
    // nudging LMS toward it. No-op in fixed mode or when feedback is disabled.
    void onRemoteVolume(double unit);
    // Paced stepper thread: sends at most one volume-button nudge every
    // kRemoteVolStepMs so LMS sees each as a fresh press (a repeat within its
    // 140 ms IR window would compute an increment of 0), and drives the loop
    // from a timer rather than the AUDG echo (which repeats even when LMS's
    // volume does not move).
    void volumeLoop(std::stop_token st);
    // Send one volume-button nudge toward remoteVolTarget_ if the pacing window
    // has elapsed. Serialized by remoteVolMutex_.
    void pumpRemoteVolume();
    // Blocks (bounded) until the sender ring has played out, so the receiver
    // finishes the track tail before we report the end of playback.
    void waitForOutputDrain(std::stop_token st);
    void stopPlayback();

    std::string name_;
    std::array<uint8_t, 6> mac_{};
    std::optional<std::string> lmsHost_;
    uint16_t lmsPort_;
    bool paceRealtime_;
    std::optional<std::string> sinkPath_;
    uint32_t serverTimeoutMs_;
    uint32_t sourceTimeoutMs_;
    NameSink nameSink_;

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
    // Drives the receiver-volume nudge chain (idle unless a receiver volume
    // event is pending).
    std::jthread volumeThread_;
    // True while streamLoop is on the stack (onStop uses it to decide whether a
    // stop-path fade can be delegated to the pump).
    std::atomic<bool> streamActive_{false};

    // Per-stream protocol state (set in startStream, read by the reader-thread
    // handlers while the pump runs).
    std::atomic<uint8_t> autostart_{1};
    std::atomic<bool> awaitCodc_{false};
    std::atomic<bool> haveCodc_{false};
    std::atomic<bool> haveCont_{false};
    bool sentStml_ = false;
    bool sentStms_ = false;

    // Session resilience flags (see streamLoop exit handling + callbacks).
    std::atomic<bool> flushed_{false};    // strm f: keep session for next track
    std::atomic<bool> retryUsed_{false};  // one transparent retry per stream

    // --vol-pct: fixed-mode level, and in lms mode the pre-AUDG fallback
    // applied to every new session before RECORD (so audio never starts at
    // the receiver's hardware default).
    VolumeAnchors anchors_;  // --vol-map dB anchors over the LMS slider
    VolumeMode volumeMode_;
    float fixedVolumePct_;
    // Last AirPlay sender percent applied from the LMS slider (lms mode); 0 =
    // none. Re-applied by launchAirplaySession() on session recreation, and
    // used to recognise the receiver echoing our own SET_PARAMETER volume back
    // on the event channel. Mute pushes (0) are not stored, so LMS's
    // end-of-fade zero gain can't mute the next session.
    std::atomic<double> lastLmsPct_{0.0};

    // Receiver-initiated volume feedback. `volumeFeedback_` gates it; the
    // rest is the stepper state, all atomic because the receiver callback runs
    // on the sender thread while AUDG arrives on the slimproto reader thread.
    bool volumeFeedback_ = true;
    std::atomic<double> lmsSliderPct_{0.0};  // LMS's own slider view (AUDG)
    std::atomic<bool> remoteVolPending_{false};
    std::atomic<double> remoteVolTarget_{0.0};
    std::atomic<int> remoteVolDir_{0};  // -1 down, +1 up, 0 unset
    std::atomic<int> remoteVolBudget_{0};
    std::atomic<uint64_t> remoteVolLastStepMs_{0};  // pacing
    std::atomic<double> remoteVolLastLms_{0.0};     // stall detection
    std::atomic<int> remoteVolStall_{0};
    std::mutex remoteVolMutex_;  // serializes pumpRemoteVolume()
};

}  // namespace squeeze2raop2