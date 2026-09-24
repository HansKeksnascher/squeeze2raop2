#!/usr/bin/env python3
"""Native Ogg Opus: the bridge advertises the 'ops' cap, attaches the
libogg+libopus decoder for a format-'u' stream and writes decoded PCM to the
sink."""

import sys

import harness

FIXTURE = harness.TEST_ROOT / "fixtures" / "tone.opus"


def body(c):
    c.wait_bridge(lambda t: "decoder: opus stream" in t,
                  what="the bridge to attach the opus decoder")
    c.wait_bridge(lambda t: "stream ended" in t, timeout=30.0,
                  what="the opus stream to end after the stop")
    assert c.wav_files(), "a non-empty WAV was written under %s" % c.workdir


if __name__ == "__main__":
    case = harness.Case(
        "opus_stream",
        lms_args=["--stream-seconds", "2", "--format", "u", "--opus-fixture", str(FIXTURE),
                  "--no-volume"],
        players=["Kitchen"],
    )
    case.default_sink = str(case.workdir / "kitchen.wav")
    sys.exit(harness.run(case, body))