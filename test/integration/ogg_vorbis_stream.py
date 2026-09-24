#!/usr/bin/env python3
"""Native Ogg Vorbis: the bridge advertises the 'ogg' cap, attaches the
stb_vorbis decoder for a format-'o' stream and writes decoded PCM to the sink."""

import sys

import harness

FIXTURE = harness.TEST_ROOT / "fixtures" / "tone.ogg"


def body(c):
    c.wait_bridge(lambda t: "decoder: ogg stream" in t,
                  what="the bridge to attach the ogg decoder")
    c.wait_bridge(lambda t: "stream ended" in t, timeout=30.0,
                  what="the ogg stream to end after the stop")
    assert c.wav_files(), "a non-empty WAV was written under %s" % c.workdir


if __name__ == "__main__":
    case = harness.Case(
        "ogg_vorbis_stream",
        lms_args=["--stream-seconds", "2", "--format", "o", "--ogg-fixture", str(FIXTURE),
                  "--no-volume"],
        players=["Kitchen"],
    )
    case.default_sink = str(case.workdir / "kitchen.wav")
    sys.exit(harness.run(case, body))