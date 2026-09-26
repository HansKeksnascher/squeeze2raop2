// Pins the stream accounting: the fullness clamp, the played-time estimate
// (fed minus ring-queued) and the adopted output rate.

#include "playback/stream_counters.h"

#include "check.h"

#include <cstdint>

using namespace squeeze2raop2::test;
using squeeze2raop2::StreamCounters;
using squeeze2raop2::StreamStats;

SQ2_TEST(stream_counters, accounting) {
    StreamCounters c;
    c.reset(44100);

    c.onReceived(1000);
    StreamStats s = c.stats();
    expect(s.bytesReceived == 1000, "bytes received");
    expect(s.streamBufferFullness == 1000, "fullness before feeding");
    expect(s.elapsedMs == 0, "no played time yet");

    // 2000 samples / 2 ch = 1000 frames; fed = received - pending.
    c.onReceived(1000);
    c.onFed(2000, 2, 400);
    s = c.stats();
    expect(s.streamBufferFullness == 400, "fullness after feeding");
    expect(s.elapsedMs == 22, "played time = fed frames");

    // Ring backlog subtracts from the played position: (1000 - 441) frames.
    c.setQueued(882);
    s = c.stats();
    expect(s.elapsedMs == 12, "played time minus queued frames");

    // A decoder backlog larger than what was received must not underflow.
    c.reset(44100);
    c.onFed(100, 2, 999999);
    s = c.stats();
    expect(s.streamBufferFullness == 0, "fullness clamped at 0");
    expect(s.bytesReceived == 0, "reset cleared received");

    // The elapsed clock follows the adopted output rate.
    c.reset(48000);
    c.onReceived(48000 * 4);
    c.onFed(48000, 2, 0);
    s = c.stats();
    expect(s.elapsedMs == 500, "elapsed uses the adopted rate");
}

SQ2_TEST(stream_counters, accessors_and_output_rate) {
    StreamCounters c;
    c.reset(44100);
    c.onReceived(1000);
    c.onFed(100, 2, 0);
    expect(c.bytesReceived() == 1000, "bytesReceived accessor");
    expect(c.fedSamples() == 50, "fedSamples accessor counts frames");

    // channels == 0 falls back to stereo rather than dividing by zero.
    c.reset(44100);
    c.onReceived(100);
    c.onFed(100, 0, 0);
    expect(c.fedSamples() == 50, "channels 0 treated as stereo");

    // setOutputRate adopts a decoder format change for the elapsed clock.
    c.reset(44100);
    c.onReceived(88200);
    c.onFed(88200, 2, 0);  // 44100 frames at the default rate = 1 s
    expect(c.stats().elapsedMs == 1000, "elapsed at the reset rate");
    c.setOutputRate(88200);
    expect(c.stats().elapsedMs == 500, "elapsed follows setOutputRate");
}