#include "airplay/raop_player.h"

#include "common/log.h"
#include "common/util.h"

#include <atomic>

namespace squeeze2raop2 {

namespace {

fxchain::RaopDeviceInfo::Auth authFor(const RaopTarget& target) {
    if (!target.password.empty()) return fxchain::RaopDeviceInfo::Auth::Password;
    // Classic RAOP (AP1) receivers (Sonos, legacy AirPlay speakers) do not
    // speak HAP pair-setup; attempting it gets a 470 and the receiver drops
    // the connection. Go straight to the RTSP handshake.
    if (!target.airplay2) return fxchain::RaopDeviceInfo::Auth::None;
    if (!target.storedCreds.empty()) return fxchain::RaopDeviceInfo::Auth::HapPin;
    return fxchain::RaopDeviceInfo::Auth::HapTransient;
}

void forwardSenderLog(fxchain::RaopLogLevel level, const std::string& msg) {
    // The sender has only Info/Warn; its Info is per-packet chatter, so map it
    // to our Debug and keep Warn. Strip the sender's hardcoded "Cast: " prefix.
    const squeeze2raop2::log::Level mapped = (level == fxchain::RaopLogLevel::Warn)
                                                 ? squeeze2raop2::log::Level::Warn
                                                 : squeeze2raop2::log::Level::Debug;
    std::string_view text = msg;
    if (text.starts_with("Cast: ")) text.remove_prefix(6);
    squeeze2raop2::log::write(mapped, squeeze2raop2::log::Area::Ap, text);
}

}  // namespace

RaopPlayer::RaopPlayer(std::string deviceName, std::string identity, RaopTarget target)
    : name_(std::move(deviceName)), identity_(std::move(identity)), target_(std::move(target)) {
    ringStorage_ = std::make_unique<fxchain::RingBuffer<int16_t>>(1 << 18);

    fxchain::RaopEvents events;
    events.launched = [this](bool ok, const std::string& error) {
        if (!ok)
            log::warn(log::Area::Ap, "{} start failure: {}", name_, error);
        else
            log::info(log::Area::Ap, "{} session launched", name_);
    };
    events.closed = [this]() {
        log::info(log::Area::Ap, "{} session closed", name_);
        if (onClosed_) onClosed_();
    };
    events.pinRequired = [this](const std::string& targetHost) {
        log::warn(log::Area::Ap,
                  "{} requires a PIN for pairing; not yet supported in bridge (target={})", name_,
                  targetHost);
    };
    events.credentialsObtained = [this](const std::string& deviceId, const std::string& credsJson) {
        log::info(log::Area::Ap, "{} stored long-term credentials ({} bytes)", name_,
                  credsJson.size());
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
    sender_->setAuth(authFor(target_), target_.airplay2, identity_, target_.storedCreds,
                     target_.password);
}

RaopPlayer::~RaopPlayer() { stop(); }

void RaopPlayer::start() {
    launched_.store(true);
    loop_.clearStopRequest();
    sender_->start(target_.host, target_.port, name_);
    pumpThread_ = std::jthread([this] { loop_.run(*sender_); });
}

void RaopPlayer::stop() {
    launched_.store(false);
    if (sender_) sender_->stop();
    loop_.requestStop();
    if (pumpThread_.joinable()) pumpThread_.join();
}

void RaopPlayer::setVolume(double pct) {
    if (sender_) sender_->setVolume(pct);
}

void RaopPlayer::setNowPlaying(const std::string& title, const std::string& artist,
                               const std::string& album) {
    if (sender_) sender_->setNowPlaying(title, artist, album);
}

bool RaopPlayer::active() const { return sender_ && sender_->active(); }

}  // namespace squeeze2raop2
