#!/usr/bin/env python3
"""End-to-end check of a flashed board over USB serial.

Runs the firmware's own command shell and asserts on the answers. This is the
fast loop that does not need WiFi, a meter or a second board: the built-in meter
simulator feeds synthesised SML telegrams through the real parser, so the whole
chain (framing -> parse -> telemetry -> REST payload) is exercised on hardware.

    python3 tools/e2e.py            # auto-detect the board
    python3 tools/e2e.py --port /dev/ttyACM2
"""

from __future__ import annotations

import argparse
import glob
import re
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required")


class Board:
    def __init__(self, port: str, timeout: float = 8.0):
        self.port = port
        self.timeout = timeout
        self.ser = serial.Serial(port, 115200, timeout=0.2)
        self.ser.reset_input_buffer()

    def close(self) -> None:
        self.ser.close()

    def read_until_idle(self, seconds: float, quiet: float = 0.5) -> str:
        """Collect output until the port has been silent for `quiet` seconds.

        Waiting a fixed time instead would let a slow answer land in the next
        command's read window and be attributed to the wrong check.
        """
        start = time.time()
        last_data = start
        out = []
        while time.time() - start < seconds and time.time() - last_data < quiet:
            raw = self.ser.readline()
            if raw:
                last_data = time.time()
                out.append(raw.decode("utf-8", "replace").rstrip())
        return "\n".join(out)

    def command(self, cmd: str, wait: float = 1.2) -> str:
        self.ser.write((cmd + "\n").encode())
        self.ser.flush()
        return self.read_until_idle(wait)

    def wait_ready(self, tries: int = 10) -> bool:
        """Wait until the board actually answers commands.

        The USB CDC takes a moment to become bidirectional after the host opens
        the port; commands fired before that are silently lost, which looks
        exactly like a firmware bug. Probe until one round trip completes.
        """
        for _ in range(tries):
            self.ser.write(b"help\n")
            self.ser.flush()
            out = self.read_until_idle(2.0, quiet=0.4)
            if "commands:" in out or "unknown command" in out:
                return True
        return False

    def wait_for_boot(self) -> str:
        deadline = time.time() + self.timeout
        seen = []
        while time.time() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").rstrip()
            seen.append(line)
            if "FW:" in line or "[HB]" in line:
                break
        return "\n".join(seen)


def find_port() -> str | None:
    try:
        out = subprocess.check_output(
            [sys.executable, "tools/serial_find.py", "--timeout", "12"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except subprocess.CalledProcessError:
        return None
    ports = [line.strip() for line in out.splitlines() if line.strip()]
    return ports[0] if ports else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="")
    ap.add_argument("--keep-sim", action="store_true", help="leave the simulator running")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        print("FAIL: no board is talking on /dev/ttyACM*")
        return 1

    print(f"board on {port}")
    board = Board(port)
    results: list[tuple[str, bool, str]] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        results.append((name, ok, detail))
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}{(' - ' + detail) if detail else ''}")

    try:
        boot = board.wait_for_boot()
        check("board answers its command shell", board.wait_ready())
        version = re.search(r"FW:(\S+)", boot)
        check("firmware reports its version over USB", version is not None,
              version.group(1) if version else boot[-120:])

        status = board.command("status", 6.0)
        check("status answers", "fw=" in status)
        check("identity is derived from the MAC, not a build flag",
              re.search(r"mac=([0-9A-F:]{17})", status) is not None)

        config_dump = board.command("config", 5.0)
        check("config dump answers", "schema=" in config_dump)
        check("meter protocol is configurable", "protocol=" in config_dump)
        check("radio stays inside the legal power limit",
              re.search(r"power=(\d+)dBm", config_dump) is not None
              and int(re.search(r"power=(\d+)dBm", config_dump).group(1)) <= 14,
              re.search(r"power=\d+dBm", config_dump).group(0) if "power=" in config_dump else "")

        duty = board.command("duty", 5.0)
        budget = re.search(r"budget=(\d+)ms", duty)
        check("duty budget is the legal 36 s per hour", budget is not None and budget.group(1) == "36000",
              budget.group(0) if budget else "")

        selftest = board.command("selftest", 8.0)
        passed = re.search(r"selftest: (\d+) passed, (\d+) failed", selftest)
        check("on-device self test passes", passed is not None and passed.group(2) == "0",
              passed.group(0) if passed else selftest[-160:])

        # The simulator drives synthesised SML telegrams through the real parser.
        board.command("meter sim on", 1.0)
        time.sleep(5.0)
        sim_status = board.command("status", 6.0)
        has_values = "meter=" in sim_status and "valid=1" in sim_status and "telegrams=0" not in sim_status
        check("meter simulator produces parsed values", has_values,
              next((l for l in sim_status.splitlines() if l.startswith("meter=")), ""))

        duty_after = board.command("duty", 5.0)
        check("airtime accounting is live", "transmissions=" in duty_after)

        if not args.keep_sim:
            board.command("meter sim off", 1.0)
    finally:
        board.close()

    failed = [name for name, ok, _ in results if not ok]
    print()
    print(f"{len(results) - len(failed)}/{len(results)} checks passed on {port}")
    if failed:
        print("failed: " + ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
