#!/usr/bin/env python3
"""An unknown codec announced via 'codc' that the bridge cannot decode is
rejected with STMn instead of feeding garbage into the ring."""

import sys

import harness


def body(c):
    c.wait_bridge(lambda t: "codc: unsupported codec" in t,
                  what="the bridge to reject the unknown codc")


if __name__ == "__main__":
    case = harness.Case(
        "codc_unsupported",
        lms_args=["--stream-seconds", "4", "--autostart", "2", "--format", "?",
                  "--codc", "x", "--no-volume"],
        players=["Kitchen"],
    )
    sys.exit(harness.run(case, body))