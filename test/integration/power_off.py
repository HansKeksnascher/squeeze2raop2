#!/usr/bin/env python3
"""LMS powering the player down ('aude' with enable_spdif=0) tears the AirPlay
session down instead of being ignored."""

import sys

import harness


def body(c):
    c.wait_bridge(lambda t: "aude power off" in t, what="the bridge to handle aude(0)")


if __name__ == "__main__":
    case = harness.Case(
        "power_off",
        lms_args=["--stream-seconds", "4", "--aude-off", "--no-volume"],
        players=["Kitchen"],
    )
    sys.exit(harness.run(case, body))