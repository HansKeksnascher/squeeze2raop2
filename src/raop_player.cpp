#include "raop_player.h"

#include "log.h"
#include "util.h"

#include <atomic>

namespace sq2 {

namespace {

fxchain::RaopDeviceInfo::Auth authFor(const RaopTarget& target) {
    if (!target.password.empty()) return fxchain::RaopDeviceInfo::Auth::Password;
    if (!target.storedCreds.empty()) return fxchain::RaopDeviceInfo::Auth::HapPin;
    return fxchain::RaopDeviceInfo::Auth::HapTransient;
}

void forwardSenderLog(fxchain::RaopLogLevel level, const std::string& msg) {
    sq2::log::Level mapped = sq2::log::Level::Debug;
    switch (level) {
    case fxchain::RaopLogLevel::Info: mapped = sq2::log::Level::Info; break;
    case fxchain::RaopLogLevel::Warn: mapped = sq2::log::Level::Warn; break;
    }
    sq2::log::write(mapped, std::string("[ap] ") + msg);
}

} // namespace

RaopPlayer::RaopPlayer(std::string deviceName, std::string identity, RaopTarget target)
    : name_(std::move(deviceName)),
      identity_(std::move(identity)),
      target_(std::move(target)) {
    ringStorage_ = std::make_unique<fxchain::RingBuffer<int16_t>>(1 << 18);

    fxchain::RaopEvents events;
    events.launched = [this](bool ok, const std::string& error) {
        if (!ok) log::warn("[ap] {} start failure: {}", name_, error);
        else log::info("[ap] {} session launched", name_);
    };
    events.closed = [this]() {
        log::info("[ap] {} session closed", name_);
        if (onClosed_) onClosed_();
    };
    events.pinRequired = [this](const std::string& targetHost) {
        log::warn("[ap] {} requires a PIN for pairing; not yet supported in bridge (target={})",
                  name_, targetHost);
    };
    events.credentialsObtained = [this](const std::string& deviceId,
                                        const std::string& credsJson) {
        log::info("[ap] {} stored long-term credentials ({} bytes)", name_, credsJson.size());
        if (onCredentials_) onCredentials_(deviceId, credsJson);
    };

    sender_ = std::make_unique<fxchain::RaopSender>(
        loop_, std::move(events),
        fxchain::RaopLogSink([](fxchain::RaopLogLevel level, const std::string& msg) {
            forwardSenderLog(level, msg);
        }));
    sender_->attachRing(ringStorage_.get());

    sender_->setIdentity({name_, identity_, "iPhone14,3"});
    sender_->setInputFormat(44100);
    sender_->setAuth(authFor(target_), target_.airplay2, identity_,
                     target_.storedCreds, target_.password);
}

RaopPlayer::~RaopPlayer() { stop(); }

void RaopPlayer::start() {
    loop_.clearStopRequest();
    sender_->start(target_.host, target_.port, name_);
    pumpThread_.emplace([this] { loop_.run(*sender_); });
}

void RaopPlayer::stop() {
    if (sender_) sender_->stop();
    if (pumpThread_.has_value()) {
        loop_.requestStop();
        pumpThread_->join();
        pumpThread_.reset();
    }
}

void RaopPlayer::setVolume(double pct) {
    if (sender_) sender_->setVolume(pct);
}

void RaopPlayer::setNowPlaying(const std::string& title, const std::string& artist,
                               const std::string& album) {
    if (sender_) sender_->setNowPlaying(title, artist, album);
}

bool RaopPlayer::active() const {
    return sender_ && sender_->active();
}

}
