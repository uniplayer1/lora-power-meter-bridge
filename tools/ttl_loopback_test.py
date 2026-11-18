#!/usr/bin/env python3
"""Prove the USB-to-TTL adapter itself, before blaming the read head.

Put one wire between the adapter's TXD and its RXD, then run this. If the bytes
come back, the adapter, its cable, the driver and the whole reading path are
proven - and whatever is wrong sits on the head's side of the wire. If they do
not come back, nothing on the head side can be trusted either, because the
measuring instrument itself is broken.

    python3 tools/ttl_loopback_test.py [--port /dev/ttyUSB0] [--baud 9600]
"""

from __future__ import annotations

import argparse
import glob
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required")

PATTERN = bytes([0x55, 0xAA, 0x1B, 0x1B, 0x01, 0x02, 0x7F, 0x80, 0xFE, 0x00])


def find_port() -> str | None:
    ports = sorted(glob.glob("/dev/ttyUSB*"))
    return ports[0] if ports else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="")
    ap.add_argument("--baud", type=int, default=9600)
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        print("No USB serial adapter found (/dev/ttyUSB* is empty).")
        return 1

    try:
        ser = serial.Serial(port, args.baud, timeout=0.5)
    except Exception as err:  # noqa: BLE001
        print(f"cannot open {port}: {err}")
        return 1

    print(f"{port} @ {args.baud} baud, TXD <-> RXD jumpered?")
    ser.reset_input_buffer()
    ser.write(PATTERN)
    ser.flush()
    time.sleep(0.2)
    back = ser.read(len(PATTERN) + 8)
    ser.close()

    if not back:
        print("\nFAIL: nothing came back. Either the TXD/RXD jumper is missing, the")
        print("      adapter is faulty, or this is a charge-only cable (the port")
        print("      exists but no data lines).")
        return 1

    print(f"  sent    : {PATTERN.hex(' ')}")
    print(f"  received: {back.hex(' ')}")
    if back[: len(PATTERN)] == PATTERN:
        print("\nPASS: the adapter, the cable and the reading path all work.")
        print("      Everything on this side is proven - the fault is on the head,")
        print("      its supply, or its wiring to the adapter.")
        return 0

    print("\nPARTIAL: bytes came back but not identical - the adapter works, the")
    print("         signal is being distorted. Check the baud rate and the jumper.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
