#include "airplay/airplay_output.h"

#include "airplay/raop_player.h"
#include "common/log.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace squeeze2raop2 {

AirplayOutput::AirplayOutput(std::string name, std::string identity,
                             std::optional<RaopTarget> target, CredentialSink credSink,
                             int latencyMs)
    : name_(std::move(name)),
      identity_(std::move(identity)),
      target_(std::move(target)),
      credSink_(std::move(credSink)),
      latencyMs_(latencyMs) {}

AirplayOutput::~AirplayOutput() { stop(false); }

std::shared_ptr<RaopPlayer> AirplayOutput::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return player_;
}

AirplayOutput::State AirplayOutput::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!player_) return State::Absent;
    if (player_->active()) return State::Running;
    return player_->launched() ? State::Closed : State::Prepared;
}

bool AirplayOutput::hasTarget() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return target_ && target_->port;
}

bool AirplayOutput::hasPlayer() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return player_ != nullptr;
}

bool AirplayOutput::airplay2() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return target_ && target_->airplay2;
}

bool AirplayOutput::prepare(uint32_t sampleRate) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (player_ && (player_->active() || !player_->launched())) return true;
    if (player_) {  // dead session (e.g. receiver teardown): recreate
        player_->stop();
        player_.reset();
    }
    // A fresh session must not inherit the previous one's receiver-loss flag.
    lost_.store(false, std::memory_order_relaxed);
    if (!target_) return false;
    CredentialSink sink = credSink_;
    player_ = std::make_shared<RaopPlayer>(name_, identity_, *target_);
    player_->setCredentialSink(std::move(sink));
    player_->setClosedCallback([this] { onClosed(); });
    player_->setInputRate(sampleRate);
    // Scheduled stream latency: must be set BEFORE start() (it is part of the
    // RTP timeline the receiver schedules against).
    player_->setLatencyMs(latencyMs_);
    return true;
}

void AirplayOutput::launch(double volumePct) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!player_) return;
    if (!player_->active()) {
        log::info("[ap] session launching for {} ({})", name_,
                  target_ && target_->airplay2 ? "ap2" : "ap1");
        player_->start();
    }
    player_->setVolume(volumePct);
}

void AirplayOutput::updateTarget(RaopTarget target) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (player_ && player_->active()) return;  // in-flight audio keeps its setup
    target_ = std::move(target);
}

void AirplayOutput::setInputRate(uint32_t rate) {
    auto player = snapshot();
    if (player) player->setInputRate(rate);
}

bool AirplayOutput::setVolume(double pct) {
    auto player = snapshot();
    if (player && player->active()) {
        player->setVolume(pct);
        return true;
    }
    return false;
}

void AirplayOutput::setNowPlaying(const std::string& title, const std::string& artist,
                                  const std::string& album) {
    auto player = snapshot();
    if (player) player->setNowPlaying(title, artist, album);
}

bool AirplayOutput::push(std::span<const int16_t> samples, size_t channels, const Abort& abort) {
    const std::shared_ptr<RaopPlayer> player = snapshot();
    if (!player) return true;

    // The ring is always interleaved stereo: duplicate each mono sample in
    // place, walking backwards so unread lower-index samples are never
    // overwritten.
    if (channels == 1) {
        const size_t frames = samples.size();
        monoScratch_.resize(frames * 2);
        for (size_t i = frames; i-- > 0;) {
            const int16_t s = samples[i];
            monoScratch_[2 * i] = s;
            monoScratch_[2 * i + 1] = s;
        }
        samples = std::span<const int16_t>(monoScratch_);
    }

    size_t offset = 0;
    while (offset < samples.size() && !abort()) {
        const size_t freeSpace = player->availableWrite();
        if (freeSpace == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }
        const size_t take = std::min(freeSpace, samples.size() - offset);
        if (!player->push(samples.subspan(offset, take))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }
        offset += take;
    }
    return offset == samples.size();
}

size_t AirplayOutput::queued() const {
    auto player = snapshot();
    return player ? player->availableRead() : 0;
}

size_t AirplayOutput::capacity() const {
    auto player = snapshot();
    return player ? player->bufferCapacity() : 0;
}

void AirplayOutput::silence() {
    auto player = snapshot();
    if (!player) return;
    player->flush();
    player->discardAudio();
}

void AirplayOutput::stop(bool flushReceiver) {
    std::shared_ptr<RaopPlayer> player;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        player = std::move(player_);
    }
    if (!player) return;
    if (flushReceiver) player->flush();
    player->discardAudio();
    player->stop();
}

bool AirplayOutput::lost() const { return lost_.load(std::memory_order_relaxed); }

bool AirplayOutput::consumeLost() { return lost_.exchange(false, std::memory_order_relaxed); }

void AirplayOutput::onClosed() { lost_.store(true, std::memory_order_relaxed); }

}  // namespace squeeze2raop2
