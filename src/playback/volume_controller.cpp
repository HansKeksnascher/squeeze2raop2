#include "playback/volume_controller.h"

#include "common/log.h"
#include "common/util.h"
#include "lms/slimproto_protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace squeeze2raop2 {

namespace {

// Stop nudging once LMS is within this many slider points of the target, and
// cap the steps per receiver change so a bogus/looping event can't spin.
constexpr double kTolerancePct = 1.0;
constexpr int kMaxSteps = 60;
// LMS treats presses within its 140 ms IR window as repeats of a held button,
// where the increment computes to 0 (so the volume never moves while an AUDG
// is still emitted). Pace nudges so each lands as a fresh press.
constexpr int kStepMs = 250;
// Give up after this many paced steps with no measurable LMS movement.
constexpr int kMaxStall = 6;
constexpr double kProgressPct = 0.5;
// A receiver may echo the volume we push via SET_PARAMETER back as an event.
// Anything within this many AirPlay percent of the last applied value is our
// own echo, not a user change.
constexpr double kEchoTolerancePct = 2.0;

}  // namespace

VolumeController::VolumeController(AirplayOutput& output, VolumeAnchors anchors, VolumeMode mode,
                                   float fixedPct, bool feedback, std::function<bool()> linkAlive,
                                   std::function<void(uint32_t code)> sendButton)
    : output_(output),
      anchors_(std::move(anchors)),
      volumeMode_(mode),
      fixedVolumePct_(fixedPct),
      volumeFeedback_(feedback),
      linkAlive_(std::move(linkAlive)),
      sendButton_(std::move(sendButton)) {
    // Receiver-initiated volume (HomePod/Sonos buttons) flows back through the
    // sender's AP2 event channel into this controller.
    output_.setRemoteVolumeCallback([this](double unit) { onReceiverVolume(unit); });
}

VolumeController::~VolumeController() { stop(); }

void VolumeController::start() {
    if (thread_.joinable()) return;
    thread_ = std::jthread([this](std::stop_token st) {
        std::unique_lock lock(mutex_);
        while (!st.stop_requested()) {
            // Block until there is a target to chase; the stop token wakes it.
            cv_.wait(lock, st, [this] { return pending_.load(std::memory_order_relaxed); });
            if (st.stop_requested()) break;
            if (!pending_.load(std::memory_order_relaxed)) continue;

            // Fresh-press pacing: honour kStepMs between presses, but wake
            // early when a new receiver event resets the schedule.
            const uint64_t now = nowMs();
            const uint64_t lastStep = lastStepMs_.load(std::memory_order_relaxed);
            if (lastStep != 0 && now - lastStep < static_cast<uint64_t>(kStepMs)) {
                const auto remaining = static_cast<int64_t>(kStepMs - (now - lastStep));
                cv_.wait_until(
                    lock, st,
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(remaining),
                    [this] {
                        return !pending_.load(std::memory_order_relaxed) ||
                               lastStepMs_.load(std::memory_order_relaxed) == 0;
                    });
                if (st.stop_requested()) break;
                continue;
            }
            pumpLocked();
        }
    });
}

void VolumeController::stop() {
    pending_.store(false, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void VolumeController::onLmsVolume(double l, double r) {
    // The recovered LMS slider percent goes through the --vol-map dB anchors
    // before it reaches the AirPlay sender's 0..100 % domain (0 % = -144 mute
    // sentinel, 100 % = 0 dB).
    double lmsPct = (l == r) ? r : (l + r) / 2.0;
    lmsSliderPct_.store(lmsPct, std::memory_order_relaxed);
    if (volumeMode_ == VolumeMode::Fixed) {
        log::info(log::Area::Ses, "volume l={:.0f} r={:.0f} -> {} (ignored, fixed at {})", l, r,
                  lmsPct, fixedVolumePct_);
        return;
    }
    double pct = anchors_.airplayPctFromLms(lmsPct);
    // Remember the slider, not mute pushes: LMS's stop-fade ends at gain 0
    // and fresh players get a 0-gain push on registration, so a stored 0
    // would mute the next session until the first AUDG.
    if (pct > 0.0) lastLmsPct_.store(pct, std::memory_order_relaxed);
    if (output_.setVolume(pct)) {
        log::info(log::Area::Ap, "volume {:.1f} pct applied (lms)", pct);
    } else {
        log::info(log::Area::Ses, "volume -> {:.1f} pct ({})", pct,
                  pct > 0.0 ? "remembered for next session" : "mute, not remembered");
    }
}

// Receiver changed its own output volume (HomePod/Sonos buttons). The receiver
// reports a unit volume (0..1, unit = db/30 + 1), which is the same domain the
// sender's setVolume() pct uses: airplayPct = unit * 100. Invert the anchor
// table to get the LMS slider that corresponds, then chase it with volume
// button nudges (slimproto has no absolute player->server volume).
void VolumeController::onReceiverVolume(double unit) {
    if (!volumeFeedback_ || volumeMode_ != VolumeMode::Lms) return;
    unit = std::clamp(unit, 0.0, 1.0);
    const double airplayPct = unit * kAirplayPctMax;
    // Ignore an echo of the volume we last pushed to the receiver (some
    // receivers report it back on the event channel); only a real user change
    // should move LMS.
    const double echoPct = lastLmsPct_.load(std::memory_order_relaxed);
    if (echoPct > 0.0 && std::abs(airplayPct - echoPct) <= kEchoTolerancePct) return;
    // Unit 0 is the receiver's mute/floor: LMS 0 is its mute, so map it there.
    const double target =
        (airplayPct <= 0.0) ? 0.0 : anchors_.lmsPctFromDb(dbFromAirplayPct(airplayPct));
    const double lms = lmsSliderPct_.load(std::memory_order_relaxed);
    if (std::abs(target - lms) < kTolerancePct) {
        log::info(log::Area::Ses, "receiver volume {:.3f} -> LMS {:.1f} (already there)", unit,
                  target);
        pending_.store(false, std::memory_order_relaxed);
        cv_.notify_one();
        return;
    }
    log::info(log::Area::Ses, "receiver volume {:.3f} -> LMS {:.1f} (from {:.1f})", unit, target,
              lms);
    target_.store(target, std::memory_order_relaxed);
    dir_.store(0, std::memory_order_relaxed);
    budget_.store(kMaxSteps, std::memory_order_relaxed);
    stall_.store(0, std::memory_order_relaxed);
    lastStepMs_.store(0, std::memory_order_relaxed);  // first step may fire at once
    pending_.store(true, std::memory_order_relaxed);
    cv_.notify_one();
    // The paced stepper thread sends the nudges.
}

// Start the AirPlay sender with the right initial volume. Volume must be
// applied AFTER start(): RaopSender::start() wipes pendingVolumeDb_ ("never
// carry volume between devices"), so a pre-start setVolume is lost and
// startStreaming_ falls back to 0 dB = full blast. Post-start it only stores
// until the handshake finishes; startStreaming_ sends the stored value before
// the audio pacer starts. In lms mode the last AUDG slider value wins; without
// one yet the fixed --vol-pct level covers the first seconds until LMS pushes
// the slider.
void VolumeController::launch() {
    double pct = fixedVolumePct_;
    if (volumeMode_ == VolumeMode::Lms) {
        const double remembered = lastLmsPct_.load(std::memory_order_relaxed);
        if (remembered > 0.0) pct = remembered;
    }
    log::info(log::Area::Ap, "volume {:.1f} pct applied (post-start)", pct);
    output_.launch(pct);
}

void VolumeController::pumpLocked() {
    if (!pending_.load(std::memory_order_relaxed)) return;
    if (!linkAlive_ || !linkAlive_()) {
        pending_.store(false, std::memory_order_relaxed);
        log::warn(log::Area::Ses, "receiver volume dropped, LMS link is gone");
        return;
    }

    // Fresh-press pacing: each nudge must be at least kStepMs after the
    // previous one or LMS folds it into a held-button repeat.
    const uint64_t now = nowMs();
    const uint64_t lastStep = lastStepMs_.load(std::memory_order_relaxed);
    if (lastStep != 0 && now - lastStep < static_cast<uint64_t>(kStepMs)) return;

    const double target = target_.load(std::memory_order_relaxed);
    const double lms = lmsSliderPct_.load(std::memory_order_relaxed);
    const int dir = dir_.load(std::memory_order_relaxed);
    const int budget = budget_.load(std::memory_order_relaxed);

    const VolumeNudge nudge = decideVolumeNudge(target, lms, dir, budget, kTolerancePct);
    switch (nudge) {
    case VolumeNudge::Reached:
        pending_.store(false, std::memory_order_relaxed);
        log::info(log::Area::Ses, "receiver volume converged at LMS {:.1f}", lms);
        return;
    case VolumeNudge::Overshoot:
        pending_.store(false, std::memory_order_relaxed);
        log::info(log::Area::Ses, "receiver volume overshoot guard at LMS {:.1f} (target {:.1f})",
                  lms, target);
        return;
    case VolumeNudge::Exhausted:
        pending_.store(false, std::memory_order_relaxed);
        log::warn(log::Area::Ses, "receiver volume gave up at LMS {:.1f} (target {:.1f})", lms,
                  target);
        return;
    case VolumeNudge::Up:
    case VolumeNudge::Down: break;
    }

    // Stall guard: if several paced presses produced no LMS movement, the
    // button path is not working for this player; stop instead of spinning.
    if (lastStep != 0 && std::abs(lms - lastLms_.load(std::memory_order_relaxed)) < kProgressPct) {
        const int stall = stall_.load(std::memory_order_relaxed) + 1;
        stall_.store(stall, std::memory_order_relaxed);
        if (stall >= kMaxStall) {
            pending_.store(false, std::memory_order_relaxed);
            log::warn(log::Area::Ses,
                      "receiver volume gave up at LMS {:.1f} (target {:.1f}): no movement", lms,
                      target);
            return;
        }
    } else {
        stall_.store(0, std::memory_order_relaxed);
    }

    const bool up = nudge == VolumeNudge::Up;
    log::info(log::Area::Ses, "receiver volume nudge {} (LMS {:.1f} -> target {:.1f})",
              up ? "up" : "down", lms, target);
    budget_.store(budget - 1, std::memory_order_relaxed);
    dir_.store(up ? 1 : -1, std::memory_order_relaxed);
    lastStepMs_.store(now, std::memory_order_relaxed);
    lastLms_.store(lms, std::memory_order_relaxed);
    if (sendButton_) sendButton_(up ? kVolUpButton : kVolDownButton);
}

}  // namespace squeeze2raop2