#pragma once

#include "airplay/airplay_output.h"
#include "lms/lms_stream.h"
#include "lms/slimproto_protocol.h"
#include "playback/decoder/decoder.h"
#include "playback/resampler.h"
#include "playback/ring_telemetry.h"
#include "playback/stream_counters.h"
#include "playback/volume_map.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace squeeze2raop2 {

class DebugWavSink;

// Why a stream's HTTP source ended without a normal decode completion. Mirrors
// squeezelite's disconnect_code so the session can emit the right DSCO.
enum class DisconnectCode : std::uint8_t {
    Ok = 0,           // DISCONNECT_OK: normal end of stream
    Local = 1,        // LOCAL_DISCONNECT: send/header failure
    Remote = 2,       // REMOTE_DISCONNECT: peer closed / read error
    Unreachable = 3,  // connect failed
    Timeout = 4,      // source silence watchdog / header deadline
    None = 0xFF,      // no disconnect (still streaming / silent stop)
};

// One track's playback pipeline: HTTP source read + Decoder drive + optional
// PCM sink, with the pause drain, replay gain, fades, skip-ahead and the
// realtime pacer. Mechanism only — the session owns
// the retry/flush policy and the LMS-facing STATs, and drives run() from its
// stream thread. All methods run on the stream thread except
// pause()/unpause()/skipAhead()/requestFadeOut()/interrupt().
class PlaybackStream {
public:
    // Why a run() pass ended.
    enum class End : std::uint8_t {
        Stopped,      // stop request / shutdown
        FadedOut,     // a requested fade-out completed
        Lost,         // receiver closed the session
        Eof,          // HTTP EOF with the decoder drained
        DecodeError,  // decoder failed (the caller reports STMn)
        SocketError,  // source socket died (also a paused EOF)
    };

    PlaybackStream(AirplayOutput& output, StreamCounters& counters, bool paceRealtime,
                   ResamplerQuality resamplerQuality, std::optional<std::string> sinkPath,
                   uint32_t sourceTimeoutMs = 0, int bufferMs = 0);
    ~PlaybackStream();
    PlaybackStream(const PlaybackStream&) = delete;
    PlaybackStream& operator=(const PlaybackStream&) = delete;

    // Phase 1: open the HTTP source (reader + leftover/ICY state) and return
    // the response headers (for sendResp). No decoder yet — an unknown codec
    // waits for 'codc'. On failure fills `error` and sets disconnectCode().
    std::optional<std::string> openSource(const StrmStart& st, const std::string& host,
                                          uint16_t port, std::string& error);

    // Phase 2: create the decoder (+ optional sink) for a known format. Called
    // from startStream() or, for 'codc' streams, from the codc handler.
    bool attachDecoder(StreamFormat format, const PcmParams& pcm, std::string& error);

    // One pump pass: pause drain, read/decode/push, launch, pacing. Returns
    // when the track ends or a stop is requested, and is re-entered after a
    // receiver-loss retry (the HTTP source stays open). The callback fires once
    // the first decoded audio is queued, so the session can launch the sender
    // and apply the volume.
    End run(std::stop_token st, const std::function<void()>& onOutputReady);

    // Set/clear a pause deadline (session callbacks, reader thread).
    void pause(uint32_t ms);
    // Resume: clears a timed/indefinite pause and schedules the output start.
    // `jiffies` is LMS's `strm u` payload: the player-local ms clock value at
    // which output must begin (0 = immediately).
    void unpause(uint32_t jiffies = 0);
    // Hold output closed until the first `strm u` (strm s autostart 0). The
    // pump decodes/fills but does not launch the receiver until then.
    void holdStart();
    // Drop `ms` worth of decoded output (LMS 'strm a'), counting it as played.
    void skipAhead(uint32_t ms);
    // Start a fade-out (transition type OUT/INOUT); false when no fade applies.
    // The pump keeps emitting ramped audio until the fade completes, then
    // run() returns End::FadedOut so the session can tear down.
    bool requestFadeOut();
    // Unblock a read() parked in poll()/recv() (session teardown).
    void interrupt();
    // Close the sink and the source (idempotent).
    void close();

    // Pipeline output format: the AirPlay clock (44100 Hz s16 stereo) once a
    // decoder is attached; empty before that. The decoder's native rate is
    // internal (the resampler input).
    PcmFormat format() const;

    DisconnectCode disconnectCode() const { return disconnect_; }

    // Raw ICY blocks, forwarded to LMS by the session.
    void setMetaForward(std::function<void(std::string_view)> cb) { metaForward_ = std::move(cb); }
    // First decoded chunk is available (session sends STMl for autostart 0).
    void setDecoderReady(std::function<void()> cb) { decoderReady_ = std::move(cb); }
    // The sender ring ran dry while the HTTP source is still active (STMo).
    void setUnderrun(std::function<void()> cb) { underrun_ = std::move(cb); }
    // Re-push the last ICY title to a recreated receiver session.
    void reapplyNowPlaying();

private:
    void onMeta(std::string_view block);
    // Decode `data` and emit its chunks (sink + ring); false when the decoder
    // failed. `toOutput=false` is the paused drain (decode, discard).
    bool feed(std::stop_token st, std::span<const std::byte> data, PcmFormat& fmt,
              DebugWavSink* sink, bool toOutput = true);
    // Drop queued skip frames from one chunk; returns the played drop count.
    size_t consumeSkip(size_t frames);
    // Scheduled start gate (strm s autostart 0 / strm u). Open when nowMs()
    // reaches the deadline; the pump holds output until then.
    bool gateOpen();
    bool prefillReached() const;
    void waitForGate(std::stop_token st);
    // Ring-level throttle: the target cap on not-yet-sent ring audio, in samples.
    uint64_t ringTargetSamples() const;
    // Apply replay gain + the active fade to a float chunk in scratch.
    std::span<const float> applyGainFade(std::span<const float> pcm, const PcmFormat& fmt);
    // Round/saturate the float output to interleaved s16 in scratch.
    std::span<const int16_t> clampToS16(std::span<const float> pcm);
    // The fixed pipeline output format (AirPlay clock).
    PcmFormat outputFormat() const;
    // Ring-occupancy telemetry + the 10 s source-rate regulation boundary.
    void sampleRingTelemetry();

    AirplayOutput& output_;
    StreamCounters& counters_;
    bool paceRealtime_;
    std::optional<std::string> sinkPath_;

    HttpStreamReader reader_;
    std::unique_ptr<Decoder> decoder_;
    std::unique_ptr<Resampler> resampler_;
    std::unique_ptr<DebugWavSink> sink_;
    // Pipeline clock/quality: decoders emit at their native rate and the
    // resampler converts to outputRate_ (the AirPlay clock).
    ResamplerQuality resamplerQuality_ = ResamplerQuality::Medium;
    uint32_t outputRate_ = kDefaultSampleRate;
    uint32_t sourceRate_ = 0;  // decoder native rate feeding the resampler
    RingTelemetry ringTelemetry_;
    std::atomic<uint64_t> pauseUntilMs_{0};
    std::atomic<uint64_t> skipFrames_{0};
    // Output start gate: 0 = open, UINT64_MAX = closed until strm u, else open
    // once nowMs() reaches the value (a scheduled strm u start).
    std::atomic<uint64_t> outputGateUntilMs_{0};
    // Pause wait: the pump blocks here instead of polling; pause()/unpause()
    // (reader thread) wake it and the stop token cancels it.
    std::mutex pauseMutex_;
    std::condition_variable_any pauseCv_;
    // Fade request from the reader thread; the pump owns the ramp position.
    std::atomic<bool> fadeOutRequested_{false};
    // Pacing clock: active (non-sleeping, non-retry) ms, continuous across a
    // retry pass so it stays in step with the emitted timeline.
    uint64_t activeMs_ = 0;
    std::string lastTitle_;
    std::function<void(std::string_view)> metaForward_;
    std::function<void()> decoderReady_;
    std::function<void()> underrun_;

    // Stream parameters adopted at openSource()/attachDecoder().
    DisconnectCode disconnect_ = DisconnectCode::None;
    int32_t replayGain_ = kFixedOne;
    uint8_t fadeMode_ = 0;  // 0 none, 2 in, 3 out, 4 inout (1 cross -> none)
    uint32_t fadeSecs_ = 0;
    // Fade ramp, stream-thread owned (fadeOutRequested_ crosses threads).
    // Frame-based, like squeezelite's _checkfade: the fade spans fadeSecs of
    // audio, independent of wall-clock pacing.
    bool fadeIn_ = false;
    bool fadeInDone_ = false;
    bool fadeOut_ = false;
    bool fadeDone_ = false;
    uint64_t fadeInFrames_ = 0;
    uint32_t fadeInDur_ = 0;
    uint64_t fadeOutFrames_ = 0;
    uint32_t fadeOutDur_ = 0;
    std::vector<float> gainScratch_;
    std::vector<int16_t> outScratch_;

    bool decoderReadyFired_ = false;
    bool underrunFired_ = false;
    // One-shot logging for the scheduled-start gate (observability).
    bool gateWaitLogged_ = false;
    // Ring-level throttle state (see run()): true while the sender ring is
    // above the high-water mark and we are not reading.
    bool ringThrottled_ = false;
    // When the ring first went empty in the current underrun candidate window;
    // 0 = not empty. A sustained (~1 s) empty ring reports STMo, a brief
    // post-pause/resume refill gap does not.
    uint64_t ringEmptySinceMs_ = 0;
    // End the track when the source delivers nothing for this long (0 = off);
    // lastDataMs_ is the wall-clock of the most recent byte (kept fresh while
    // paused, which is not a stall).
    uint32_t sourceTimeoutMs_ = 0;
    // Cap on the sender ring's not-yet-sent audio (0 = disabled).
    int bufferMs_ = 0;
    uint64_t lastDataMs_ = 0;
};

// --- Pure stream-pump helpers (unit-testable without a pipeline) -----------

// Skip-ahead interval in ms -> source frames at `rate`. 0 ms -> 0 frames.
inline uint64_t skipFramesFor(uint32_t ms, uint32_t rate) {
    return (static_cast<uint64_t>(ms) * rate) / 1000u;
}

// LMS `strm u` payload (a player-local 1 kHz clock value) -> output start-gate
// deadline in the nowMs() timeline. 0 means "open now"; a past value (including
// the u32 wrap) also opens now. Pure so the wrap handling is unit-testable.
inline uint64_t gateDeadlineFromJiffies(uint32_t jiffies, uint64_t now) {
    const int32_t delta = static_cast<int32_t>(jiffies - static_cast<uint32_t>(now));
    return delta > 0 ? now + static_cast<uint64_t>(delta) : 0;
}

// STMo decision: the receiver output is running but its ring is empty while
// the HTTP source is still active (a network underrun, squeezelite parity).
inline bool outputUnderrun(bool running, size_t queued) { return running && queued == 0; }

// --- Pump and stream-exit tuning -------------------------------------------
//
// Policy knobs for one track's pump (playback_stream.cpp) and the exit/drain
// path (stream_coordinator.cpp).

constexpr size_t kReadBufferBytes = 4096;     // source read buffer
constexpr uint32_t kReadPollTimeoutMs = 150;  // HTTP read poll timeout
constexpr uint32_t kPauseSliceMs = 25;        // max pause-wait slice
constexpr uint32_t kUnderrunWindowMs = 1000;  // sustained-empty window -> STMo
constexpr uint32_t kPacerLeadMs = 60;         // pacer sleep headroom
constexpr uint32_t kPacerMaxSleepMs = 120;    // max pacer sleep slice
// While the start gate is closed (autostart 0), stop reading once this much
// audio is buffered and wait for the scheduled `strm u`; keeps the pump from
// decoding a whole track into the ring while the group waits to start.
constexpr uint32_t kStartGatePrefillMs = 1000;
// Ring throttle: how long to service the sender before re-checking the ring
// level; the slice bounds how long a stop request waits to be noticed.
constexpr uint32_t kRingThrottleSliceMs = 50;
constexpr uint32_t kDrainTimeoutMs = 5000;    // bound on the end-of-track drain
constexpr uint32_t kDrainPumpMs = 20;         // sender pump slice while draining
constexpr uint32_t kRetryDelayMs = 2000;      // receiver-loss retry delay

// LMS 'strm s' transition types (Squeezebox.pm). Cross (1) is unsupported and
// mapped to no fade.
constexpr uint8_t kFadeModeNone = 0;
constexpr uint8_t kFadeModeCross = 1;
constexpr uint8_t kFadeModeIn = 2;
constexpr uint8_t kFadeModeOut = 3;
constexpr uint8_t kFadeModeInOut = 4;

}  // namespace squeeze2raop2