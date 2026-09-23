// Pins the stream loop's ring-health sampler: the 10 s summary boundary and
// the one warn/recover latch per starvation episode.

#include "playback/ring_telemetry.h"

#include "check.h"

#include <cstdint>

using namespace squeeze2raop2::test;
using squeeze2raop2::RingTelemetry;

SQ2_TEST(ring_telemetry, summary_window) {
    RingTelemetry t;
    expect(!t.observe(1000, 1000).has_value(), "first sample arms the window");
    expect(!t.observe(500, 5000).has_value(), "no boundary before 10 s");
    const auto w = t.observe(2000, 11000);
    expect(w.has_value() && *w == 10000, "10 s boundary returns the window");
    expect(!t.observe(7, 12000).has_value(), "window re-arms after the summary");
    const auto w2 = t.observe(7, 21000);
    expect(w2.has_value() && *w2 == 10000, "second window");
}

SQ2_TEST(ring_telemetry, starvation_latch) {
    RingTelemetry s;
    expect(!s.starved(), "not starved initially");
    (void)s.observe(0, 100);
    expect(s.starved(), "starve latched on a zero sample");
    (void)s.observe(0, 200);
    expect(s.starved(), "starve stays latched while empty");
    (void)s.observe(50, 300);
    expect(!s.starved(), "recovery clears the latch");
}