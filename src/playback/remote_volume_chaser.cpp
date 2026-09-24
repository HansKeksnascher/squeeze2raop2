// Receiver-initiated volume chase: see remote_volume_chaser.h. The pure
// per-step rule is in remote_volume.h; this file is the pacing/threading shell.
// Extracted from PlayerSession so the session no longer carries the stepper
// state and its own thread.

#include "playback/remote_volume_chaser.h"

#include "common/log.h"
#include "common/util.h"
#include "playback/remote_volume.h"

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

RemoteVolumeChaser::RemoteVolumeChaser(VolumeAnchors anchors, std::function<bool()> linkAlive,
                                       std::function<void(bool up)> sendButton,
                                       std::function<double()> lmsSliderPct)
    : anchors_(std::move(anchors)),
      linkAlive_(std::move(linkAlive)),
      sendButton_(std::move(sendButton)),
      lmsSliderPct_(std::move(lmsSliderPct)) {}

RemoteVolumeChaser::~RemoteVolumeChaser() { stop(); }

void RemoteVolumeChaser::start() {
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

void RemoteVolumeChaser::stop() {
    pending_.store(false, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void RemoteVolumeChaser::onReceiverVolume(double unit, double echoPct) {
    unit = std::clamp(unit, 0.0, 1.0);
    const double airplayPct = unit * 100.0;
    // Ignore an echo of the volume we last pushed to the receiver (some
    // receivers report it back on the event channel); only a real user change
    // should move LMS.
    if (echoPct > 0.0 && std::abs(airplayPct - echoPct) <= kEchoTolerancePct) return;
    // Unit 0 is the receiver's mute/floor: LMS 0 is its mute, so map it there.
    const double target =
        (airplayPct <= 0.0) ? 0.0 : anchors_.lmsPctFromDb(dbFromAirplayPct(airplayPct));
    const double lms = lmsSliderPct_();
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

void RemoteVolumeChaser::pumpLocked() {
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
    const double lms = lmsSliderPct_();
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
    if (sendButton_) sendButton_(up);
}

}  // namespace squeeze2raop2
