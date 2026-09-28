// Pins the stream pump's pure skip/underrun decisions (playback_stream.h):
// how many decoded frames a skip-ahead request drops, and when an empty ring
// while running counts as an output underrun.

#include "playback/playback_stream.h"

#include "check.h"

using namespace squeeze2raop2::test;
using squeeze2raop2::gateDeadlineFromJiffies;
using squeeze2raop2::outputUnderrun;
using squeeze2raop2::skipFramesFor;

SQ2_TEST(playback_stream, skip_frames) {
    expect(skipFramesFor(1000, 44100) == 44100, "1 s at 44.1 kHz");
    expect(skipFramesFor(500, 48000) == 24000, "0.5 s at 48 kHz");
    expect(skipFramesFor(0, 44100) == 0, "zero ms skips nothing");
    expect(skipFramesFor(1, 44100) == 44, "1 ms truncates frames");
}

SQ2_TEST(playback_stream, output_underrun) {
    expect(outputUnderrun(true, 0), "running with an empty ring underruns");
    expect(!outputUnderrun(true, 100), "running with queued audio is fine");
    expect(!outputUnderrun(false, 0), "an idle output is not an underrun");
}

// strm u scheduling: the payload is the player-local 1 kHz clock value for the
// output start, compared wrap-safely against nowMs().
SQ2_TEST(playback_stream, start_gate_deadline) {
    const uint64_t now = 1'000'000;
    expect(gateDeadlineFromJiffies(static_cast<uint32_t>(now + 300), now) == now + 300,
           "a future jiffies schedules the deadline");
    expect(gateDeadlineFromJiffies(static_cast<uint32_t>(now), now) == 0,
           "exactly now opens immediately");
    expect(gateDeadlineFromJiffies(static_cast<uint32_t>(now - 5), now) == 0,
           "a past jiffies opens immediately");
    expect(gateDeadlineFromJiffies(0, now) == 0, "zero (plain resume) opens immediately");
    // The player clock wraps every ~49.7 days: a deadline just after the wrap
    // must still be seen as future.
    const uint64_t wrapNow = 0xFFFF'FF00ULL;
    expect(gateDeadlineFromJiffies(0x0000'0020u, wrapNow) == wrapNow + 0x120,
           "deadline across the u32 wrap");
}
