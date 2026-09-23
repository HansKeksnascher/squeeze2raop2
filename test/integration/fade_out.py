#!/usr/bin/env python3
"""strm-s transition type 3 (fade out): on stop the bridge ramps the tail down
before tearing the output down, instead of cutting hard."""

import sys

import harness


def body(c):
    c.wait_bridge(lambda t: "stream ended" in t, timeout=30.0,
                  what="the stream to end after the fade-out stop")
    wavs = c.wav_files()
    assert wavs, "a non-empty WAV was written under %s" % c.workdir
    steady = harness.wav_peak(wavs[0], skip_s=4.5, take_s=0.2)
    tail = harness.wav_peak(wavs[0], skip_s=5.8, take_s=0.1)
    assert steady > 3000, "the steady-state tone is present (got %d)" % steady
    assert tail < steady * 0.5, "fade-out ramps the tail down (%d vs %d)" % (tail, steady)


if __name__ == "__main__":
    case = harness.Case(
        "fade_out",
        lms_args=["--stream-seconds", "5", "--transition", "3", "--transition-secs", "1",
                  "--no-volume", "--stop-after-sec", "3"],
        players=["Kitchen"],
    )
    case.default_sink = str(case.workdir / "kitchen.wav")
    sys.exit(harness.run(case, body))