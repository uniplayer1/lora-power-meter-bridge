#!/usr/bin/env python3
"""Find which /dev/ttyACM* port a LilyGO board is printing on.

Boards whose application owns the USB CDC (ARDUINO_USB_CDC_ON_BOOT=1) cannot be
identified with esptool, and they re-enumerate under a *different* ttyACM number
every time the application starts. Matching on serial output is the only
reliable way to find them - which is also how flash.sh confirms that the image
it just wrote is the one actually running.

Modes:
  default        print one line per port that produced output (optionally only
                 ports whose output contains --expect)
  --match-only   print "<port>\t<line>" for every line matching --expect, which
                 is how the firmware version is read back off a running board

Exit status is 0 when at least one port matched, 1 otherwise.
"""

from __future__ import annotations

import argparse
import glob
import sys
import threading
import time

sys.path.insert(0, str(__import__("pathlib").Path(__file__).parent))

from serial_util import open_port


def collect(port: str, seconds: float, results: dict[str, list[str]]) -> None:
    lines: list[str] = []
    ser = open_port(port, 115200, timeout=0.3)
    if ser is None:
        return
    try:
        deadline = time.time() + seconds
        while time.time() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            text = raw.decode("utf-8", "replace").rstrip()
            if text:
                lines.append(text)
    except Exception:  # noqa: BLE001
        return
    finally:
        ser.close()
    if lines:
        results[port] = lines


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=4.0, help="seconds to listen per port")
    parser.add_argument("--expect", default="", help="only count output containing this text")
    parser.add_argument("--match-only", action="store_true",
                        help="print every matching line instead of the port list")
    args = parser.parse_args()

    ports = sorted(glob.glob("/dev/ttyACM*"))
    if not ports:
        return 1

    results: dict[str, list[str]] = {}
    threads = [
        threading.Thread(target=collect, args=(port, args.timeout, results), daemon=True)
        for port in ports
    ]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    matched = 0
    for port in ports:
        lines = results.get(port)
        if not lines:
            continue
        hits = [line for line in lines if not args.expect or args.expect in line]
        if not hits:
            continue
        matched += 1
        if args.match_only:
            for line in hits:
                print(f"{port}\t{line}")
        else:
            print(port)
            print(f"{port}\t{hits[0]}", file=sys.stderr)

    return 0 if matched else 1


if __name__ == "__main__":
    sys.exit(main())
