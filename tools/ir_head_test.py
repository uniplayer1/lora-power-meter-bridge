#!/usr/bin/env python3
"""Talk to the IR read head directly, through a USB-to-TTL adapter.

This is the receiver test without any firmware in between: the head pulls its
data line LOW on infrared light and leaves it HIGH otherwise, so a working head
produces bytes while it is lit and silence while it is dark. Holding a TV remote
at the head is a known-good IR source that costs nothing.

    python3 tools/ir_head_test.py --seconds 30
    python3 tools/ir_head_test.py --port /dev/ttyUSB0 --baud 9600

Run it, then hold a TV remote against the head with a button pressed. The
per-second counter must jump. It staying at 0 is the answer too: nothing is
driving the line.
"""

from __future__ import annotations

import argparse
import collections
import glob
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required")


def find_port() -> str | None:
    for pattern in ("/dev/ttyUSB*", "/dev/serial/by-id/*FTDI*", "/dev/serial/by-id/*USB*"):
        ports = sorted(glob.glob(pattern))
        if ports:
            return ports[0]
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--dump", action="store_true",
                    help="print every byte received, in hex, and identify the protocol")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        print("No USB serial adapter found. /dev/ttyUSB* is empty - check the cable")
        print("(charge-only cables are the usual cause) and plug it in directly.")
        return 1

    try:
        ser = serial.Serial(port, args.baud, timeout=0.1)
    except Exception as err:  # noqa: BLE001
        print(f"cannot open {port}: {err}")
        return 1

    print(f"{port} @ {args.baud} baud, {args.seconds:.0f}s. Now hold a TV remote at the head")
    print("and keep a button pressed.\n")
    print("  second   bytes   total   most frequent byte values")
    total = 0
    raw = bytearray()
    quiet = 0
    deadline = time.time() + args.seconds
    second = 0
    while time.time() < deadline:
        second += 1
        window_end = time.time() + 1.0
        seen: collections.Counter[int] = collections.Counter()
        count = 0
        while time.time() < window_end:
            data = ser.read(256)
            if data:
                count += len(data)
                seen.update(data)
                raw.extend(data)
        total += count
        if count == 0:
            quiet += 1
        top = " ".join(f"{b:02X}x{n}" for b, n in seen.most_common(5))
        print(f"  {second:6d}   {count:5d}   {total:5d}   {top}")
    ser.close()

    print()
    if total == 0:
        print("RESULT: complete silence. Nothing is driving the data line. Check, in this")
        print("        order: VCC on 3V3 (never 5V), GND, head data-out on the adapter's")
        print("        RXD, and the adapter itself (does it show up as a port at all?).")
    elif quiet == second:
        print("RESULT: silence in every window.")
    else:
        print(f"RESULT: {total} bytes, activity in {second - quiet} of {second} seconds.")
        print("        The receiver works. If the meter still says nothing, the problem is")
        print("        on the meter side: alignment, or the interface needs unlocking.")

    if args.dump and raw:
        print()
        print(f"--- every byte received ({len(raw)}) ---")
        for off in range(0, len(raw), 32):
            chunk = raw[off:off + 32]
            hexpart = " ".join(f"{b:02X}" for b in chunk)
            text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
            print(f"  {off:06X}  {hexpart:<96}  {text}")

        sml = raw.find(b"\x1b\x1b\x1b\x1b")
        print()
        if sml >= 0:
            print(f"SML telegram starts at offset {sml} - this meter speaks SML, the")
            print("firmware's default. Point it at the meter and it will parse on its own.")
        elif b"/" in raw:
            print("No SML start marker, but the bytes carry ASCII - this is D0")
            print("(IEC 62056-21). Set the meter protocol to D0; the firmware sends the")
            print("wake-up itself.")
        else:
            print("No SML start marker in the bytes above.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
