// Pins the stream pump's pure skip/underrun decisions (playback_stream.h):
// how many decoded frames a skip-ahead request drops, and when an empty ring
// while running counts as an output underrun.

#include "playback/playback_stream.h"

#include "check.h"

using namespace squeeze2raop2::test;
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
