#pragma once

#include "airplay/airplay_output.h"
#include "lms/slimproto.h"
#include "playback/playback_stream.h"
#include "playback/slimproto_session.h"
#include "playback/stream_counters.h"
#include "playback/volume_controller.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace squeeze2raop2 {

// One track's lifecycle policy: opens the HTTP source, attaches the decoder,
// starts the pump once the LMS handshake allows it, and applies the exit
// policy (retry / give up / natural end / error / flush). Owns the current
// PlaybackStream and the stream thread plus the per-stream flags. All LMS
// reporting goes through the SlimProtoSession it is given; the receiver is
// driven through the shared AirplayOutput.
class StreamCoordinator {
public:
    StreamCoordinator(AirplayOutput& output, StreamCounters& counters, SlimProtoSession& link,
                      VolumeController& volume, uint32_t sourceTimeoutMs, bool paceRealtime,
                      std::optional<std::string> sinkPath);
    ~StreamCoordinator();
    StreamCoordinator(const StreamCoordinator&) = delete;
    StreamCoordinator& operator=(const StreamCoordinator&) = delete;

    // Event handlers from the SlimProtoSession.
    void onStreamStart(const StrmStart& st);
    void onCont();
    void onCodc(StreamFormat format, const PcmParams& pcm);
    void flush();
    void pause(uint32_t ms);
    void unpause(uint32_t ms);
    void skipAhead(uint32_t ms);
    void requestStopOrFade();
    void audeOff();

    // Join the stream thread and close the current track (idempotent).
    void shutdown();
    bool active() const { return streamActive_.load(); }

    // Played-time snapshot for the LMS STAT reply.
    StreamStats currentStats();

private:
    void maybeStartPump();
    void onDecoderReady();
    void onPrebufferReady();
    void streamLoop(std::stop_token st);
    void waitForOutputDrain(std::stop_token st);

    AirplayOutput& output_;
    StreamCounters& counters_;
    SlimProtoSession& link_;
    VolumeController& volume_;
    const uint32_t sourceTimeoutMs_;
    const bool paceRealtime_;
    const std::optional<std::string> sinkPath_;

    // The current track's pipeline; replaced per stream, closed by shutdown().
    std::unique_ptr<PlaybackStream> track_;
    std::jthread streamThread_;
    // True while streamLoop is on the stack (onStop uses it to decide whether a
    // stop-path fade can be delegated to the pump).
    std::atomic<bool> streamActive_{false};

    // Per-stream protocol state (set in onStreamStart, read by the reader-thread
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
};

}  // namespace squeeze2raop2