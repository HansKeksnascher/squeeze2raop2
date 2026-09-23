#pragma once

#include "airplay/airplay_output.h"
#include "lms/lms_stream.h"
#include "lms/wire_types.h"
#include "playback/decoder/decoder.h"
#include "playback/gain.h"
#include "playback/ring_telemetry.h"
#include "playback/stream_counters.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace squeeze2raop2 {

class PcmFileSink;

// Why a stream's HTTP source ended without a normal decode completion. Mirrors
// squeezelite's disconnect_code so the session can emit the right DSCO.
enum class DisconnectCode : std::uint8_t {
    Ok = 0,           // DISCONNECT_OK: normal end of stream
    Local = 1,        // LOCAL_DISCONNECT: send/header failure
    Remote = 2,       // REMOTE_DISCONNECT: peer closed / read error
    Unreachable = 3,  // connect failed
    Timeout = 4,      // header phase deadline
    None = 0xFF,      // no disconnect (still streaming / silent stop)
};

// One track's playback pipeline: HTTP source read + Decoder drive + optional
// PCM sink, with the startup prebuffer gate, the pause drain, replay gain,
// fades, skip-ahead and the realtime pacer. Mechanism only — the session owns
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
                   std::optional<std::string> sinkPath);
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

    // One pump pass: prebuffer gate, pause drain, read/decode/push, pacing.
    // Returns when the track ends or a stop is requested, and is re-entered
    // after a receiver-loss retry (the HTTP source stays open). The callback
    // fires once the ring reaches half capacity, so the session can launch the
    // sender and apply the volume.
    End run(std::stop_token st, const std::function<void()>& onPrebufferReady);

    // Set/clear a pause deadline (session callbacks, reader thread).
    void pause(uint32_t ms);
    void unpause();
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

    // Decoder output format (defaults until the decoder is attached).
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
              PcmFileSink* sink, bool toOutput = true);
    // Drop queued skip frames from one chunk; returns the played drop count.
    size_t consumeSkip(size_t frames);
    // Apply replay gain + the active fade to an s16 chunk in scratch.
    std::span<const int16_t> applyGainFade(std::span<const int16_t> chunk, const PcmFormat& fmt);
    // Ring-occupancy telemetry + the 10 s source-rate regulation boundary.
    void sampleRingTelemetry();

    AirplayOutput& output_;
    StreamCounters& counters_;
    bool paceRealtime_;
    std::optional<std::string> sinkPath_;

    HttpStreamReader reader_;
    std::unique_ptr<Decoder> decoder_;
    std::unique_ptr<PcmFileSink> sink_;
    RingTelemetry ringTelemetry_;
    std::atomic<uint64_t> pauseUntilMs_{0};
    std::atomic<uint64_t> skipFrames_{0};
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
    std::vector<int16_t> gainScratch_;

    bool decoderReadyFired_ = false;
    bool underrunFired_ = false;
    // When the ring first went empty in the current underrun candidate window;
    // 0 = not empty. A sustained (~1 s) empty ring reports STMo, a brief
    // post-pause/resume refill gap does not.
    uint64_t ringEmptySinceMs_ = 0;
};

}  // namespace squeeze2raop2