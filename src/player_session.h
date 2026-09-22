#pragma once

#include "config.h"
#include "decode_stage.h"
#include "lms_stream.h"
#include "raop_player.h"
#include "slimproto.h"
#include "volume_map.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace squeeze2raop2 {

class PcmFileSink;

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
// connection to LMS) plus, per stream, an HTTP reader, an optional local
// PCM sink and a lazily launched RaopPlayer. Event callbacks run on the
// SlimProtoClient's reader thread; audio pumping runs on streamThread_.
class PlayerSession {
public:
    PlayerSession(std::string deviceId, std::string name, std::array<uint8_t, 6> mac,
                  std::optional<std::string> lmsHost, uint16_t lmsPort, bool paceRealtime,
                  std::optional<std::string> sinkPath, std::optional<RaopTarget> raopTarget,
                  RaopPlayer::CredentialSink credSink, VolumeMode volumeMode, VolumeAnchors anchors,
                  float volPct, int latencyMs);
    ~PlayerSession();
    PlayerSession(const PlayerSession&) = delete;
    PlayerSession& operator=(const PlayerSession&) = delete;

    void start();
    bool prepareAirplaySession(uint32_t sampleRate);
    void launchAirplaySession();
    void updateTarget(RaopTarget t);
    void stop();

private:
    StreamStats currentStats();
    void startStream(const StrmStart& st);
    void streamLoop(std::stop_token st);
    void onRaopDeviceClosed();
    void onIcyMeta(std::string_view block);
    // One pipeline for every stream format: bytes go through the stream's
    // Decoder (mp3 decode / pcm normalization), drained in 1152-frame
    // chunks. Returns false when the decoder failed and the stream must
    // abort.
    bool feedStream(std::stop_token st, std::span<const std::byte> data, PcmFormat& fmt,
                    PcmFileSink* sink, bool toOutput = true);
    // Ring-occupancy telemetry while streaming: 10 s min/max/cur summary
    // plus one warn/recover pair per starvation episode (ring sampled on
    // the stream thread; user pauses are excluded — the ring draining
    // there is the expected baseline behavior).
    void sampleRingTelemetry();
    // Blocks (bounded) until the sender ring has played out, so the receiver
    // finishes the track tail before we report the end of playback.
    void waitForOutputDrain(std::stop_token st);
    void feedRing(RaopPlayer& raop, std::stop_token st, std::span<const int16_t> samples);
    // Shared-snapshot access to raop_: any thread may take a reference to the
    // current player; teardown may reset the member while the caller holds the
    // snapshot, and the object stays alive until the caller drops it. This is
    // what lets the stream thread push audio without holding targetMutex_.
    [[nodiscard]] std::shared_ptr<RaopPlayer> raopSnapshot() const;
    void stopPlayback();
    // Silence the receiver immediately and, with fullStop, end and destroy
    // the session. flush() drops the receiver's jitter-buffer tail (a
    // HomePod would keep playing it for ~latency otherwise) and
    // discardAudio() removes the ring residue that would follow the flush.
    // Caller must hold targetMutex_; no-op without a live session.
    void teardownReceiverAudio(bool fullStop);

    std::string deviceId_;
    std::string name_;
    std::array<uint8_t, 6> mac_{};
    std::optional<std::string> lmsHost_;
    uint16_t lmsPort_;
    bool paceRealtime_;
    std::optional<std::string> sinkPath_;

    std::unique_ptr<SlimProtoClient> client_;

    HttpStreamReader reader_;
    std::jthread streamThread_;
    bool autostartPending_ = false;

    // The stream's decode stage (mp3/pcm); null while no stream runs. Owns
    // the decoder plus the stream-thread-only telemetry/rate-regulator state.
    std::unique_ptr<DecodeStage> stage_;
    // Scratch for pushToRaop()'s byte->s16 conversion (stream thread only).
    // Reused across calls so the audio path stops allocating per chunk.
    std::vector<int16_t> pushScratch_;
    // Adopted output rate for the elapsed-time STAT field, updated by the
    // stream thread when the decoder adopts a format (read by currentStats).
    std::atomic<uint32_t> elapsedRate_{44100};

    std::mutex mutex_;
    mutable std::mutex targetMutex_;
    uint64_t pauseUntilMs_ = 0;
    uint64_t receivedBytes_ = 0;
    uint64_t fedBytes_ = 0;
    uint64_t fedSamples_ = 0;

    std::optional<RaopTarget> raopTarget_;
    RaopPlayer::CredentialSink credSink_;
    // Shared so the stream thread can hold a reference across blocking ring
    // pushes while teardown on another thread resets the member (see
    // raopSnapshot()).
    std::shared_ptr<RaopPlayer> raop_;
    std::string raopIdentity_;
    // Session resilience flags (see streamLoop exit handling + callbacks).
    std::atomic<bool> flushed_{false};     // strm f: keep session for next track
    std::atomic<bool> deviceLost_{false};  // receiver ended the session
    std::atomic<bool> retryUsed_{false};   // one transparent retry per stream
    // Interleaved samples still queued in the sender ring, sampled by the
    // stream thread; currentStats() subtracts them to report played time.
    std::atomic<size_t> queuedSamples_{0};
    // Last ICY title: dedupes the repeated meta blocks some stations send,
    // and is re-applied to a recreated receiver session after a retry.
    std::string lastTitle_;
    // --vol-pct: fixed-mode level, and in lms mode the pre-AUDG fallback
    // applied to every new session before RECORD (so audio never starts at
    // the receiver's hardware default).
    VolumeAnchors anchors_;  // --vol-map dB anchors over the LMS slider
    VolumeMode volumeMode_;
    float fixedVolumePct_;
    // Last LMS slider percent seen via AUDG (lms mode); 0 = none. Re-applied
    // by prepareAirplaySession() on session recreation. Mute pushes (0) are
    // not stored
    // so LMS's end-of-fade zero gain can't mute the next session.
    double lastLmsPct_ = 0.0;
    // Scheduled AirPlay latency in ms (--ap-latency-ms).
    int latencyMs_;
};

}  // namespace squeeze2raop2
