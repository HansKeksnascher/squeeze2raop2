#!/usr/bin/env python3
"""A silent LMS control connection is detected by the protocol watchdog
([global] server-timeout-ms) and reconnected, rather than hanging until TCP
keepalive gives up."""

import sys

import harness


def body(c):
    # The fake LMS sends nothing after HELO; the bridge must time out at 1.2 s
    # and dial again, producing a second HELO.
    c.wait_lms(lambda t: harness.count(t, "HELO device_id") >= 2, timeout=15.0,
               what="the bridge to reconnect after server silence")


if __name__ == "__main__":
    case = harness.Case(
        "watchdog",
        lms_args=["--silent"],
        players=["Kitchen"],
        server_timeout_ms=1200,
    )
    sys.exit(harness.run(case, body))