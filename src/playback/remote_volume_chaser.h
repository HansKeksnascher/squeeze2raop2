#pragma once

#include "playback/volume_map.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace squeeze2raop2 {

// Drives the receiver-initiated volume chase. An AirPlay 2 receiver reports the
// absolute volume its buttons set, but slimproto only exposes relative volume
// buttons, so the bridge walks LMS toward the reported target one paced step at
// a time. The pure per-step decision lives in remote_volume.h; this class owns
// the cross-thread state and the paced stepper thread. onReceiverVolume() runs
// on the sender thread, the stepper on its own.
class RemoteVolumeChaser {
public:
    // `anchors` maps the receiver's dB to the LMS slider; `linkAlive` reports
    // whether the LMS connection still exists; `sendButton(up)` emits one
    // volume-button press; `lmsSliderPct` reads LMS's current slider view.
    RemoteVolumeChaser(VolumeAnchors anchors, std::function<bool()> linkAlive,
                       std::function<void(bool up)> sendButton,
                       std::function<double()> lmsSliderPct);
    ~RemoteVolumeChaser();
    RemoteVolumeChaser(const RemoteVolumeChaser&) = delete;
    RemoteVolumeChaser& operator=(const RemoteVolumeChaser&) = delete;

    void start();
    // Cancel any in-flight chase and join the stepper (idempotent).
    void stop();
    // Record a receiver volume (unit 0..1) and begin chasing it. `echoPct` is
    // the last AirPlay percent we applied, so an echo of our own SET_PARAMETER
    // back on the event channel is ignored.
    void onReceiverVolume(double unit, double echoPct);

private:
    // One paced step toward target_. Caller holds mutex_.
    void pumpLocked();

    const VolumeAnchors anchors_;
    std::function<bool()> linkAlive_;
    std::function<void(bool up)> sendButton_;
    std::function<double()> lmsSliderPct_;

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