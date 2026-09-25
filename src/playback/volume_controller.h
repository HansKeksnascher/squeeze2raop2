#pragma once

#include "airplay/airplay_output.h"
#include "app/config.h"
#include "playback/volume_map.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace squeeze2raop2 {

// All volume policy for one player session: the LMS slider -> AirPlay percent
// mapping (via VolumeAnchors), the fixed vs lms mode switch, the pre-AUDG
// fallback level, and the receiver-initiated feedback chase. Owns the paced
// stepper thread: an AirPlay 2 receiver reports the volume its buttons set as
// an absolute unit volume, but slimproto only exposes relative volume buttons,
// so the controller walks LMS toward that target one paced BUTN at a time (the
// pure per-step rule lives in volume_map.h). It also installs the receiver
// callback on the output, so the session only has to forward the two volume
// entry points.
class VolumeController {
public:
    // `linkAlive` reports whether the LMS control connection exists;
    // `sendButton(code)` emits one raw volume-button press; `output`'s
    // remote-volume callback is installed here.
    VolumeController(AirplayOutput& output, VolumeAnchors anchors, VolumeMode mode, float fixedPct,
                     bool feedback, std::function<bool()> linkAlive,
                     std::function<void(uint32_t code)> sendButton);
    ~VolumeController();
    VolumeController(const VolumeController&) = delete;
    VolumeController& operator=(const VolumeController&) = delete;

    void start();
    // Cancel any in-flight chase and join the stepper (idempotent).
    void stop();

    // LMS AUDG slider update (left/right percent), mapped and applied.
    void onLmsVolume(double l, double r);
    // Receiver-originated output-volume change (AP2 event channel, unit 0..1).
    void onReceiverVolume(double unit);
    // Start the AirPlay session with the right initial volume (last AUDG value
    // in lms mode, else the fixed --vol-pct level).
    void launch();
    // The latest LMS slider percent (shared with the chase).
    double sliderPct() const { return lmsSliderPct_.load(std::memory_order_relaxed); }

private:
    // One paced step toward target_. Caller holds mutex_.
    void pumpLocked();

    AirplayOutput& output_;
    const VolumeAnchors anchors_;
    const VolumeMode volumeMode_;
    const float fixedVolumePct_;
    const bool volumeFeedback_;
    std::function<bool()> linkAlive_;
    std::function<void(uint32_t code)> sendButton_;

    // Last AirPlay sender percent applied from the LMS slider (lms mode); 0 =
    // none. Re-applied by launch() on session recreation, and used to recognise
    // the receiver echoing our own SET_PARAMETER volume back on the event
    // channel. Mute pushes (0) are not stored, so LMS's end-of-fade zero gain
    // can't mute the next session.
    std::atomic<double> lastLmsPct_{0.0};
    // LMS's own slider view (AUDG), read by the stepper.
    std::atomic<double> lmsSliderPct_{0.0};

    // Receiver-volume chase state (the paced stepper thread blocks here).
    std::atomic<bool> pending_{false};
    std::atomic<double> target_{0.0};
    std::atomic<int> dir_{0};  // -1 down, +1 up, 0 unset
    std::atomic<int> budget_{0};
    std::atomic<uint64_t> lastStepMs_{0};  // pacing
    std::atomic<double> lastLms_{0.0};     // stall detection
    std::atomic<int> stall_{0};
    std::mutex mutex_;
    // Signalled by onReceiverVolume(); the stepper blocks here instead of
    // polling, and the stop_token overload wakes it on cancellation.
    std::condition_variable_any cv_;
    std::jthread thread_;
};

}  // namespace squeeze2raop2