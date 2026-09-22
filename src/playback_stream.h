#pragma once

#include "airplay_output.h"
#include "decoder/decoder.h"
#include "lms_stream.h"
#include "ring_telemetry.h"
#include "slimproto.h"
#include "stream_counters.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace squeeze2raop2 {

class PcmFileSink;

// One track's playback pipeline: HTTP source read + Decoder drive + optional
// PCM sink, with the startup prebuffer gate, the pause drain and the realtime
// pacer. Mechanism only — the session owns the retry/flush policy and the
// LMS-facing STATs, and drives run() from its stream thread. All methods run
// on the stream thread except pause()/unpause()/interrupt().
class PlaybackStream {
public:
    // Why a run() pass ended.
    enum class End : std::uint8_t {
        Stopped,      // stop request / shutdown
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

    // Open the source and create the decoder + optional sink. On success
    // returns the response headers (for sendResp); on failure fills `error`.
    std::optional<std::string> open(const StrmStart& st, const std::string& host, uint16_t port,
                                    std::string& error);

    // One pump pass: prebuffer gate, pause drain, read/decode/push, pacing.
    // Returns when the track ends or a stop is requested, and is re-entered
    // after a receiver-loss retry (the HTTP source stays open). The callback
    // fires once the ring reaches half capacity, so the session can launch the
    // sender and apply the volume.
    End run(std::stop_token st, const std::function<void()>& onPrebufferReady);

    // Set/clear a pause deadline (session callbacks, reader thread).
    void pause(uint32_t ms);
    void unpause();
    // Unblock a read() parked in poll()/recv() (session teardown).
    void interrupt();
    // Close the sink and the source (idempotent).
    void close();

    // Decoder output format (input fallback until the first frame).
    PcmFormat format() const;

    // Raw ICY blocks, forwarded to LMS by the session.
    void setMetaForward(std::function<void(std::string_view)> cb) {
        metaForward_ = std::move(cb);
    }
    // Re-push the last ICY title to a recreated receiver session.
    void reapplyNowPlaying();

private:
    void onMeta(std::string_view block);
    // Decode `data` and emit its chunks (sink + ring); false when the decoder
    // failed. `toOutput=false` is the paused drain (decode, discard).
    bool feed(std::stop_token st, std::span<const std::byte> data, PcmFormat& fmt,
              PcmFileSink* sink, bool toOutput = true);
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
    // Pacing clock: active (non-sleeping, non-retry) ms, continuous across a
    // retry pass so it stays in step with the emitted timeline.
    uint64_t activeMs_ = 0;
    std::string lastTitle_;
    std::function<void(std::string_view)> metaForward_;
};

}  // namespace squeeze2raop2
