// Pins the receiver-initiated volume chase shell (volume_controller.cpp): the
// AUDG -> slider mapping and its mute/remember rules, echo suppression of our
// own SET_PARAMETER volume, the fixed-vs-lms mode gate, the LMS-link guard, the
// paced step gate, convergence, and the stall give-up. The pure per-step rule
// (decideVolumeNudge) is pinned in test_volume_map.cpp.

#include "playback/volume_controller.h"

#include "airplay/airplay_output.h"
#include "common/util.h"

#include "check.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace squeeze2raop2;
using namespace squeeze2raop2::test;

namespace squeeze2raop2 {

// Test-only access to the controller internals (friend of VolumeController).
struct VolumeControllerTestAccess {
    // Run one paced step as the stepper would, without the thread.
    static void pump(VolumeController& c) {
        std::lock_guard<std::mutex> lock(c.mutex_);
        c.pumpLocked();
    }
    static bool pending(const VolumeController& c) { return c.pending_.load(); }
    static double slider(const VolumeController& c) { return c.lmsSliderPct_.load(); }
    static double lastPct(const VolumeController& c) { return c.lastLmsPct_.load(); }
    // Pretend the last paced press happened long enough ago to allow another,
    // so tests do not sleep kStepMs per step.
    static void ageStep(VolumeController& c) {
        const uint64_t last = c.lastStepMs_.load();
        if (last != 0) c.lastStepMs_.store(nowMs() - 1000);
    }
};

}  // namespace squeeze2raop2

namespace {

// The stepper thread and the test thread both touch the button log, so guard it.
struct ButtonLog {
    void add(uint32_t code) {
        std::lock_guard<std::mutex> lock(m);
        codes.push_back(code);
    }
    size_t size() const {
        std::lock_guard<std::mutex> lock(m);
        return codes.size();
    }
    uint32_t back() const {
        std::lock_guard<std::mutex> lock(m);
        return codes.back();
    }
    bool empty() const {
        std::lock_guard<std::mutex> lock(m);
        return codes.empty();
    }

    mutable std::mutex m;
    std::vector<uint32_t> codes;
};

struct Harness {
    AirplayOutput output{"test", "AABBCCDDEEFF", std::nullopt, nullptr, CredentialSink{}, 50};
    VolumeAnchors anchors = *VolumeAnchors::parse(kDefaultVolumeMap);
    ButtonLog buttons;
    std::atomic<bool> alive{true};
};

VolumeController makeController(Harness& h, bool feedback = true, VolumeMode mode = VolumeMode::Lms,
                                float fixedPct = 0.7f) {
    return VolumeController(
        h.output, h.anchors, mode, fixedPct, feedback,
        [&h] { return h.alive.load(std::memory_order_relaxed); },
        [&h](uint32_t code) { h.buttons.add(code); });
}

}  // namespace

SQ2_TEST(volume_controller, audg_mapping_and_mute) {
    Harness h;
    auto vc = makeController(h);

    // A non-mute AUDG slider is mapped through the anchors and remembered.
    vc.onLmsVolume(50.0, 50.0);
    expect(VolumeControllerTestAccess::slider(vc) == 50.0, "slider stored");
    expect(VolumeControllerTestAccess::lastPct(vc) == h.anchors.airplayPctFromLms(50.0),
           "non-mute slider remembered for the next session");

    // A mute push (LMS stop-fade end / fresh-player registration) is NOT
    // remembered, or the next session would start muted.
    const double before = VolumeControllerTestAccess::lastPct(vc);
    vc.onLmsVolume(0.0, 0.0);
    expect(VolumeControllerTestAccess::slider(vc) == 0.0, "slider follows mute");
    expect(VolumeControllerTestAccess::lastPct(vc) == before, "mute is not remembered");

    // Stereo AUDG averages left/right.
    vc.onLmsVolume(40.0, 60.0);
    expect(VolumeControllerTestAccess::slider(vc) == 50.0, "L/R averaged");
}

SQ2_TEST(volume_controller, fixed_mode_ignores_audg) {
    Harness h;
    auto vc = makeController(h, /*feedback=*/true, VolumeMode::Fixed);
    vc.onLmsVolume(50.0, 50.0);
    // Slider view still tracks AUDG; the remembered level stays 0 so launch()
    // uses the fixed percent.
    expect(VolumeControllerTestAccess::slider(vc) == 50.0, "slider view tracks AUDG");
    expect(VolumeControllerTestAccess::lastPct(vc) == 0.0, "fixed mode remembers nothing");
}

SQ2_TEST(volume_controller, echo_is_suppressed) {
    Harness h;
    auto vc = makeController(h);
    vc.onLmsVolume(50.0, 50.0);  // lastPct == 50
    const double echo = h.anchors.airplayPctFromLms(50.0);

    // A receiver event equal to the volume we pushed is our own echo.
    vc.onReceiverVolume(echo / 100.0);
    expect(!VolumeControllerTestAccess::pending(vc), "echo does not start a chase");

    // A clearly different receiver volume does start one.
    vc.onReceiverVolume(0.70);
    expect(VolumeControllerTestAccess::pending(vc), "a real change starts a chase");
}

SQ2_TEST(volume_controller, gated_off) {
    {
        Harness h;
        auto vc = makeController(h, /*feedback=*/false);
        vc.onLmsVolume(50.0, 50.0);
        vc.onReceiverVolume(0.70);
        expect(!VolumeControllerTestAccess::pending(vc), "feedback off ignores receiver volume");
    }
    {
        Harness h;
        auto vc = makeController(h, /*feedback=*/true, VolumeMode::Fixed);
        vc.onReceiverVolume(0.70);
        expect(!VolumeControllerTestAccess::pending(vc), "fixed mode ignores receiver volume");
    }
}

SQ2_TEST(volume_controller, link_gone_drops_chase) {
    Harness h;
    auto vc = makeController(h);
    vc.onLmsVolume(50.0, 50.0);
    vc.onReceiverVolume(0.70);
    require(VolumeControllerTestAccess::pending(vc), "chase pending");

    h.alive.store(false, std::memory_order_relaxed);
    VolumeControllerTestAccess::pump(vc);
    expect(!VolumeControllerTestAccess::pending(vc), "dead LMS link drops the chase");
    expect(h.buttons.empty(), "no button press without a link");
}

SQ2_TEST(volume_controller, paced_step_and_convergence) {
    Harness h;
    auto vc = makeController(h);
    vc.onLmsVolume(50.0, 50.0);

    // Target 0.70 -> AirPlay 70 -> LMS ~70; one step up should fire at once.
    vc.onReceiverVolume(0.70);
    VolumeControllerTestAccess::pump(vc);
    require(h.buttons.size() == 1, "first step fires immediately");
    expect(h.buttons.back() == kVolUpButton, "target above presses volume up");

    // A second step too soon after the first is paced out.
    VolumeControllerTestAccess::pump(vc);
    expect(h.buttons.size() == 1, "second step is paced");

    // Simulate LMS applying the press: slider moves to the target, then the
    // next (aged) step converges and stops.
    vc.onLmsVolume(70.0, 70.0);
    VolumeControllerTestAccess::ageStep(vc);
    VolumeControllerTestAccess::pump(vc);
    expect(h.buttons.size() == 1, "convergence stops without another press");
    expect(!VolumeControllerTestAccess::pending(vc), "converged chase is cleared");
}

SQ2_TEST(volume_controller, stall_guard_gives_up) {
    Harness h;
    auto vc = makeController(h);
    vc.onLmsVolume(50.0, 50.0);
    vc.onReceiverVolume(0.90);  // target ~90, LMS never moves

    // First step fires immediately, then each aged step registers no movement.
    int guard = 0;
    while (VolumeControllerTestAccess::pending(vc) && guard++ < 100) {
        VolumeControllerTestAccess::pump(vc);
        VolumeControllerTestAccess::ageStep(vc);
    }
    expect(!VolumeControllerTestAccess::pending(vc), "stall guard eventually gives up");
    // kMaxStall = 6: one press per stalled step before giving up, not unbounded.
    expect(h.buttons.size() == 6, "gives up after the stall budget, not more");
    expect(h.buttons.back() == kVolUpButton, "stall presses were volume up");
}

SQ2_TEST(volume_controller, start_stop_thread) {
    Harness h;
    auto vc = makeController(h);
    vc.onLmsVolume(50.0, 50.0);
    vc.start();
    vc.onReceiverVolume(0.70);
    // The stepper wakes and fires the first press.
    for (int i = 0; i < 50 && h.buttons.empty(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    expect(!h.buttons.empty(), "stepper thread fires a step");
    vc.stop();
    expect(!VolumeControllerTestAccess::pending(vc), "stop clears the chase");
    vc.stop();  // idempotent
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
