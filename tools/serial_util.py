#!/usr/bin/env python3
"""Open a serial port and name the real reason when it will not open.

A permission denial and a port held open by another program are host problems.
Both used to surface as "nothing is driving the data line" or "charge-only
cable", which sends you to the wrong end of the wire: adapter, cable and read
head are fine.
"""

from __future__ import annotations

import sys

try:
    import serial
except ImportError:
    sys.exit("pyserial is required")

RULE_HINT = """\
Fix it for the whole machine, once (needs sudo):

  sudo install -m644 udev/99-lora-power.rules /etc/udev/rules.d/
  sudo udevadm control --reload && sudo udevadm trigger --subsystem-match=tty
"""


def say(text: str = "") -> None:
    """Diagnostics go to stderr, so stdout stays parseable as a port list."""
    print(text, file=sys.stderr)


def open_port(port: str, baud: int, timeout: float = 0.1) -> serial.Serial | None:
    """Open `port`, or explain the failure on stderr and return None."""
    try:
        return serial.Serial(port, baud, timeout=timeout)
    except serial.SerialException as err:
        say(f"cannot open {port}: {err}")
        say()
        text = str(err).lower()
        if "permission denied" in text:
            say(f"{port} is root-owned and this user cannot open it.")
            say("Either join the owning group - a new membership only takes effect")
            say("after logging out and back in - or install the rule below:")
            say()
            say(RULE_HINT)
        elif "busy" in text:
            say(f"{port} is held open by another program. ModemManager probes this")
            say("adapter as if it were a modem, and a leftover monitor holds it too.")
            say("The rule below tells ModemManager to leave it alone:")
            say()
            say(RULE_HINT)
        elif "no such file" in text:
            say(f"{port} does not exist: the adapter is not plugged in, or it")
            say("dropped off USB and came back under a different number.")
        return None
