#pragma once

#include "config.h"
#include "decoder/decoder.h"
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

// One LMS player <-> AirPlay receiver pairing: a SlimProtoClient (control
// connection to LMS) plus, per stream, an HTTP reader, an optional local
// PCM sink and a lazily launched RaopPlayer. Event callbacks run on the
// SlimProtoClient's reader thread; audio pumping runs on streamThread_.
class PlayerSession {
public:
    PlayerSession(std::string deviceId, std::string name, std::array<uint8_t, 6> mac,
                  std::optional<std::string> lmsHost, uint16_t lmsPort,
                  bool paceRealtime, std::optional<std::string> sinkPath,
                  std::optional<RaopTarget> raopTarget,
                  RaopPlayer::CredentialSink credSink, VolumeMode volumeMode,
                  VolumeAnchors anchors, float volPct, int latencyMs);
    ~PlayerSession();
    PlayerSession(const PlayerSession&) = delete;
    PlayerSession& operator=(const PlayerSession&) = delete;

    void start();
    bool ensureRaop(uint32_t sampleRate);
    void updateTarget(RaopTarget t);
    void stop();

private:
    StreamStats currentStats();
    void startStream(const StrmStart& st);
    void streamLoop(std::stop_token st);
    void onRaopDeviceClosed();
    void onIcyMeta(std::string_view block);
    void pushToRaop(std::stop_token st, std::span<const std::byte> data, const PcmFormat& fmt);
    // One pipeline for every stream format: bytes go through the stream's
    // Decoder (mp3 decode / pcm normalization), drained in 1152-frame
    // chunks. Returns false when the decoder failed and the stream must
    // abort.
    bool feedStream(std::stop_token st, std::span<const std::byte> data,
                    PcmFormat& fmt, PcmFileSink* sink, bool toOutput = true);
    void feedRing(std::stop_token st, const std::vector<int16_t>& samples);
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

    PcmFormat format_{};
    uint32_t bytesPerFrame_ = 4;

    // The stream format's decoder (mp3/pcm); null while no stream runs.
    std::unique_ptr<Decoder> decoder_;

    std::mutex mutex_;
    std::mutex targetMutex_;
    uint64_t pauseUntilMs_ = 0;
    uint64_t receivedBytes_ = 0;
    uint64_t fedBytes_ = 0;
    uint64_t fedSamples_ = 0;

    std::optional<RaopTarget> raopTarget_;
    RaopPlayer::CredentialSink credSink_;
    std::unique_ptr<RaopPlayer> raop_;
    std::string raopIdentity_;
    // Session resilience flags (see streamLoop exit handling + callbacks).
    std::atomic<bool> flushed_{false};     // strm f: keep session for next track
    std::atomic<bool> deviceLost_{false};  // receiver ended the session
    std::atomic<bool> retryUsed_{false};   // one transparent retry per stream
    // Last ICY title: dedupes the repeated meta blocks some stations send,
    // and is re-applied to a recreated receiver session after a retry.
    std::string lastTitle_;
    // --vol-pct: fixed-mode level, and in lms mode the pre-AUDG fallback
    // applied to every new session before RECORD (so audio never starts at
    // the receiver's hardware default).
    VolumeAnchors anchors_;   // --vol-map dB anchors over the LMS slider
    VolumeMode volumeMode_;
    float fixedVolumePct_;
    // Last LMS slider percent seen via AUDG (lms mode); 0 = none. Re-applied
    // by ensureRaop() on session recreation. Mute pushes (0) are not stored
    // so LMS's end-of-fade zero gain can't mute the next session.
    double lastLmsPct_ = 0.0;
    // Scheduled AirPlay latency in ms (--ap-latency-ms).
    int latencyMs_;
};

} // namespace squeeze2raop2
