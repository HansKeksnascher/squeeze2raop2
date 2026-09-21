#pragma once

#include "raop_auth.h"
#include "raop_loop.h"
#include "raop_sender.h"
#include "ring_buffer.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>

namespace sq2 {

struct RaopTarget {
    std::string host;
    uint16_t port = 7000;
    bool airplay2 = true;
    std::string password;
    std::string storedCreds;
};

class RaopPlayer {
public:
    using CredentialSink = std::function<void(const std::string&, const std::string&)>;

    RaopPlayer(std::string deviceName, std::string identity, RaopTarget target);
    ~RaopPlayer();
    RaopPlayer(const RaopPlayer&) = delete;
    RaopPlayer& operator=(const RaopPlayer&) = delete;

    // the sender drives `loop`; ownership covers both
    void setCredentialSink(CredentialSink sink) { onCredentials_ = std::move(sink); }

    void start();
    void stop();

    size_t availableWrite() const { return ringStorage_->availableWrite(); }
    bool push(std::span<const int16_t> stereoSamples) {
        return ringStorage_->tryPush(stereoSamples);
    }
    void setVolume(double pct);
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

    std::unique_ptr<fxchain::RingBuffer<int16_t>> ringStorage_;
    std::unique_ptr<fxchain::RaopSender> sender_;
    fxchain::RaopLoop loop_;
    std::optional<std::thread> pumpThread_;
    CredentialSink onCredentials_;
    std::function<void()> onClosed_;
};

}
