#!/usr/bin/env python3
"""strm-s transition type 2 (fade in) ramps the start of a track: the WAV sink
starts quiet and reaches full level within the transition period."""

import sys

import harness


def body(c):
    c.wait_bridge(lambda t: "stream ended" in t, timeout=30.0,
                  what="the faded stream to end after the stop")
    wavs = c.wav_files()
    assert wavs, "a non-empty WAV was written under %s" % c.workdir
    early = harness.wav_peak(wavs[0], skip_s=0.0, take_s=0.2)
    late = harness.wav_peak(wavs[0], skip_s=2.0, take_s=0.5)
    assert late > 3000, "the steady-state tone is present (got %d)" % late
    assert early < late * 0.7, "fade-in keeps the start quiet (%d vs %d)" % (early, late)


if __name__ == "__main__":
    case = harness.Case(
        "fade",
        lms_args=["--stream-seconds", "3", "--transition", "2", "--transition-secs", "1",
                  "--no-volume", "--stop-after-sec", "3"],
        players=["Kitchen"],
    )
    case.default_sink = str(case.workdir / "kitchen.wav")
    sys.exit(harness.run(case, body))