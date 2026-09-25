#pragma once

#include "airplay/raop_types.h"
#include "common/util.h"
#include "lms/slimproto_protocol.h"
#include "raop_auth.h"
#include "raop_loop.h"
#include "raop_sender.h"
#include "ring_buffer.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>

namespace squeeze2raop2 {

class RaopPlayer {
public:
    RaopPlayer(std::string deviceName, std::string identity, RaopTarget target);
    ~RaopPlayer();
    RaopPlayer(const RaopPlayer&) = delete;
    RaopPlayer& operator=(const RaopPlayer&) = delete;

    // the sender drives `loop`; ownership covers both
    void setCredentialSink(CredentialSink sink) { onCredentials_ = std::move(sink); }

    void start();
    void stop();

    // The sender is a sans-i/o state machine and its poll host is non-blocking,
    // so there is no dedicated pump thread: whoever owns the audio lifecycle
    // drives it from its own thread. pump() services sockets + one tick pass;
    // pumpUntil() loops it until a deadline (also used as the pace wait).
    void pump(std::chrono::milliseconds maxWait = std::chrono::milliseconds(0));
    void pumpUntil(std::chrono::steady_clock::time_point deadline);
    // Coarse keep-alive driver for the between-tracks window, when the session
    // is left running but no stream thread is alive to pump it.
    void startKeepAlive();
    void stopKeepAlive();

    // Ring telemetry for the host: occupancy and capacity in interleaved
    // samples, plus whether start() has ever run (a prepared-but-unlaunched
    // player must not be recreated just because the receiver looks idle).
    size_t availableRead() const { return ringStorage_->availableRead(); }
    size_t bufferCapacity() const { return ringStorage_->capacity(); }
    bool launched() const { return launched_.load(std::memory_order_relaxed); }

    size_t availableWrite() const { return ringStorage_->availableWrite(); }
    bool push(std::span<const int16_t> stereoSamples) {
        return ringStorage_->tryPush(stereoSamples);
    }
    void setVolume(double pct);
    void setLatencyMs(int ms) {
        std::lock_guard<std::mutex> lock(senderMutex_);
        if (sender_) sender_->setLatency(uint32_t(int64_t(ms) * kDefaultSampleRate / kMsPerSecond));
    }
    void flush() {
        std::lock_guard<std::mutex> lock(senderMutex_);
        if (sender_) sender_->flush();
    }
    void setNowPlaying(const std::string& title, const std::string& artist,
                       const std::string& album);
    // Fires only for device-initiated closes / session failures (our own
    // stop() is quiet by design in the sender).
    void setClosedCallback(std::function<void()> cb) { onClosed_ = std::move(cb); }
    // Fires when the receiver changes its own output volume (AP2 event
    // channel); the argument is the receiver's unit volume, 0..1.
    void setRemoteVolumeCallback(std::function<void(double)> cb) {
        onRemoteVolume_ = std::move(cb);
    }
    void discardAudio() { ringStorage_->reset(); }

    void setInputRate(uint32_t rate) {
        std::lock_guard<std::mutex> lock(senderMutex_);
        if (sender_) sender_->setInputFormat(rate);
    }

    bool active() const;

private:
    void keepAliveLoop();

    std::string name_;
    std::string identity_;
    RaopTarget target_;

    // Destruction order matters: sender_ holds a RaopIo& to loop_, so loop_
    // (and ringStorage_) must outlive sender_ — i.e. be declared BEFORE it.
    std::unique_ptr<fxchain::RingBuffer<int16_t>> ringStorage_;
    fxchain::RaopLoop loop_;
    std::unique_ptr<fxchain::RaopSender> sender_;
    // Every call into sender_/loop_ is serialized: RaopLoop is single-threaded
    // and RaopSender is strictly so, while audio/metadata/volume arrive from
    // different threads (stream vs slimproto reader). mutable: active() is const.
    mutable std::mutex senderMutex_;
    // Between-tracks keep-alive: a coarse pump loop, started at a flush exit
    // and joined before a new stream thread takes over the pump (or on stop()).
    std::atomic<bool> keepAlive_{false};
    std::jthread keepAliveThread_;
    CredentialSink onCredentials_;
    std::function<void()> onClosed_;
    std::function<void(double)> onRemoteVolume_;
    std::atomic<bool> launched_{false};
};

}  // namespace squeeze2raop2
