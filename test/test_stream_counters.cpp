// Pins the stream accounting: the fullness clamp, the played-time estimate
// (fed minus ring-queued) and the adopted output rate.

#include "stream_counters.h"

#include "check.h"

#include <cstdint>
#include <cstdio>

using namespace sq2t;
using squeeze2raop2::StreamCounters;
using squeeze2raop2::StreamStats;

int main() {
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

    std::printf("ok\n");
    return 0;
}
