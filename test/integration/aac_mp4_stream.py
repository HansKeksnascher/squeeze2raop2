#!/usr/bin/env python3
"""Native MP4/M4A AAC: LMS remuxes an mp4 source into the aac stream format
(pcm_sample_size '5'); the bridge demuxes the container and decodes it."""

import sys

import harness

FIXTURE = harness.TEST_ROOT / "fixtures" / "tone.m4a"


def body(c):
    c.wait_bridge(lambda t: "decoder: aac stream" in t,
                  what="the bridge to attach the aac decoder")
    c.wait_bridge(lambda t: "stream ended" in t, timeout=30.0,
                  what="the aac stream to end after the stop")
    assert c.wav_files(), "a non-empty WAV was written under %s" % c.workdir


if __name__ == "__main__":
    case = harness.Case(
        "aac_mp4_stream",
        lms_args=["--stream-seconds", "2", "--format", "a", "--aac-fixture", str(FIXTURE),
                  "--container", "mp4", "--no-volume"],
        players=["Kitchen"],
    )
    case.default_sink = str(case.workdir / "kitchen.wav")
    sys.exit(harness.run(case, body))
