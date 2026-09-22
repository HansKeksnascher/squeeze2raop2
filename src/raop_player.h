#pragma once

#include "raop_auth.h"
#include "raop_loop.h"
#include "raop_sender.h"
#include "raop_types.h"
#include "ring_buffer.h"

#include <atomic>
#include <functional>
#include <memory>
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
        if (sender_) sender_->setLatency(uint32_t(int64_t(ms) * 44100 / 1000));
    }
    void flush() {
        if (sender_) sender_->flush();
    }
    void setNowPlaying(const std::string& title, const std::string& artist,
                       const std::string& album);
    // Fires only for device-initiated closes / session failures (our own
    // stop() is quiet by design in the sender).
    void setClosedCallback(std::function<void()> cb) { onClosed_ = std::move(cb); }
    void discardAudio() { ringStorage_->reset(); }

    void setInputRate(uint32_t rate) {
        if (sender_) sender_->setInputFormat(rate);
    }

    bool active() const;

private:
    std::string name_;
    std::string identity_;
    RaopTarget target_;

    // Destruction order matters: sender_ holds a RaopIo& to loop_, so loop_
    // (and ringStorage_) must outlive sender_ — i.e. be declared BEFORE it.
    std::unique_ptr<fxchain::RingBuffer<int16_t>> ringStorage_;
    fxchain::RaopLoop loop_;
    std::unique_ptr<fxchain::RaopSender> sender_;
    // The pump's exit condition lives inside fxchain::RaopLoop::run() (its
    // own atomic), so no stop_token can drive it — jthread is used for its
    // auto-join safety net only.
    std::jthread pumpThread_;
    CredentialSink onCredentials_;
    std::function<void()> onClosed_;
    std::atomic<bool> launched_{false};
};

}  // namespace squeeze2raop2
