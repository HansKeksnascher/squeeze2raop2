#pragma once

#include "airplay/raop_types.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace squeeze2raop2 {

// The bridge's fxchain binding (ring + sender + pump); AirplayOutput owns one
// and recreates it per receiver session. Defined in raop_player.h so the
// third-party sender headers stay out of this header's include graph.
class RaopPlayer;

// The bridge's live connection to one AirPlay receiver: owns the sender
// (RaopPlayer) and its ring, the target/credentials, and the volume/metadata
// application. This is the only place the session's cross-thread state lives
// (the sender is polled from the stream thread and parked on a coarse keep-alive
// driver between tracks, while the volume/metadata path comes from the
// slimproto reader thread), so the synchronization is behind this one mutex.
class AirplayOutput {
public:
    // Sender lifecycle, derived from the live sender (never cached): a
    // prepared-but-unlaunched player must not be recreated, and a started-then
    // -ended one must be.
    enum class State : std::uint8_t { Absent, Prepared, Running, Closed };

    // Polled by push(); true aborts the backpressure loop.
    using Abort = std::function<bool()>;

    AirplayOutput(std::string name, std::string identity, std::optional<RaopTarget> target,
                  CredentialSink credSink, int latencyMs);
    ~AirplayOutput();
    AirplayOutput(const AirplayOutput&) = delete;
    AirplayOutput& operator=(const AirplayOutput&) = delete;

    State state() const;
    bool running() const { return state() == State::Running; }

    bool hasTarget() const;
    bool hasPlayer() const;
    // True when the target speaks native AirPlay 2 (else classic RAOP/AP1).
    bool airplay2() const;

    // Create or reuse the sender without starting it. Returns false when there
    // is no target. Absent/Prepared -> ready; Closed -> recreate; Running ->
    // no-op.
    bool prepare(uint32_t sampleRate);
    // Start a fresh session if needed and apply the volume (must be after
    // start(): the sender wipes pending volume).
    void launch(double volumePct);
    void updateTarget(RaopTarget target);

    void setInputRate(uint32_t rate);
    // Drive the sender's non-blocking host from the caller's thread. pump()
    // services sockets + one timer pass; pumpUntil() also blocks (paced) until
    // the deadline; park()/unpark() start/stop the between-tracks keep-alive.
    void pump(std::chrono::milliseconds maxWait = std::chrono::milliseconds(0));
    void pumpUntil(std::chrono::steady_clock::time_point deadline);
    void park();
    void unpark();
    // Returns true when applied to a live session, false when there is none
    // (the caller then remembers the level for the next session).
    bool setVolume(double pct);
    void setNowPlaying(const std::string& title, const std::string& artist,
                       const std::string& album);
    // Receiver-originated output-volume changes (AP2 event channel), unit
    // volume 0..1. Stored and applied to each sender the output creates.
    void setRemoteVolumeCallback(std::function<void(double)> cb);

    // Blocking ring push with backpressure; returns false if aborted early.
    // `samples` is native s16 in `channels` channels; the ring is always
    // interleaved stereo, so mono is duplicated internally. Stream-thread
    // only (it may reuse an internal scratch buffer).
    bool push(std::span<const int16_t> samples, size_t channels, const Abort& abort);
    size_t queued() const;    // ring occupancy in samples
    size_t capacity() const;  // ring capacity in samples
    // Same values in bytes of interleaved s16, for the STAT output buffer
    // fields (squeezelite reports the output buffer in bytes).
    size_t queuedBytes() const { return queued() * sizeof(int16_t); }
    size_t capacityBytes() const { return capacity() * sizeof(int16_t); }

    // FLUSH the receiver's buffered audio and drop our ring, keeping the
    // session (pause / track flush).
    void silence();
    // Tear the session down. `flushReceiver` also FLUSHes first (user stop);
    // the natural-end path has already drained and passes false.
    void stop(bool flushReceiver);

    bool lost() const;   // receiver closed the session (peek)
    bool consumeLost();  // ...and clear

private:
    [[nodiscard]] std::shared_ptr<RaopPlayer> snapshot() const;
    void onClosed();

    std::string name_;
    std::string identity_;
    std::optional<RaopTarget> target_;
    CredentialSink credSink_;
    std::function<void(double)> remoteVolumeSink_;
    int latencyMs_ = 500;
    mutable std::mutex mutex_;  // guards player_/target_/credSink_
    std::shared_ptr<RaopPlayer> player_;
    std::atomic<bool> lost_{false};
    // Mono->stereo expansion scratch (push() only; stream-thread owned).
    std::vector<int16_t> monoScratch_;
};

}  // namespace squeeze2raop2
