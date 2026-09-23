#!/usr/bin/env python3
"""Native ADTS AAC: the bridge advertises the 'aac' cap, attaches the libxaac
decoder for a format-'a' stream and writes decoded PCM to the sink."""

import sys

import harness

FIXTURE = harness.TEST_ROOT / "fixtures" / "tone.aac"


def body(c):
    c.wait_bridge(lambda t: "decoder: aac stream" in t,
                  what="the bridge to attach the aac decoder")
    c.wait_bridge(lambda t: "stream ended" in t, timeout=30.0,
                  what="the aac stream to end after the stop")
    assert c.wav_files(), "a non-empty WAV was written under %s" % c.workdir


if __name__ == "__main__":
    case = harness.Case(
        "aac_adts_stream",
        lms_args=["--stream-seconds", "2", "--format", "a", "--aac-fixture", str(FIXTURE),
                  "--container", "adts", "--no-volume"],
        players=["Kitchen"],
    )
    case.default_sink = str(case.workdir / "kitchen.wav")
    sys.exit(harness.run(case, body))
