#!/usr/bin/env python3
"""Checks the Home Assistant integration against a real device, without HA.

The integration's entity table lives in const.py, which imports nothing from
Home Assistant; this loads that one file directly (skipping the package's
__init__, which does need HA) and runs the live JSON through the same `dig()`
the entities use. If every row here has a value, the integration will show every
entity - which is otherwise only testable on a running Home Assistant.

    python3 tools/ha_payload_test.py [host]
"""

from __future__ import annotations

import importlib.util
import json
import sys
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CONST_PATH = REPO / "custom_components" / "lilygo_power" / "const.py"


def load_const():
    spec = importlib.util.spec_from_file_location("lilygo_power_const", CONST_PATH)
    module = importlib.util.module_from_spec(spec)
    # dataclasses looks the importing module up in sys.modules while building the
    # class, so it has to be registered before the module body runs.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def fetch_state(host: str) -> dict:
    url = f"http://{host}/api/v1/state"
    with urllib.request.urlopen(url, timeout=8) as response:
        return json.load(response)


def main() -> int:
    if len(sys.argv) > 1:
        host = sys.argv[1]
    else:
        # Use the hostname the device announces; fall back to the last known one.
        host = "lilygo-power-abcd.local"

    const = load_const()
    try:
        data = fetch_state(host)
    except Exception as err:  # noqa: BLE001 - report and fail
        print(f"FAIL: {host} did not answer /api/v1/state: {err}")
        return 1

    print(f"device: {data.get('device', {}).get('name')} "
          f"({data.get('device', {}).get('role')}, {data.get('device', {}).get('fw')})")
    print(f"url   : http://{host}{const.API_PATH}\n")

    missing = []
    for spec in const.SENSORS:
        value = const.dig(data, spec.path)
        unit = f" {spec.unit}" if spec.unit else ""
        shown = "-" if value is None else f"{value}{unit}"
        print(f"  sensor        {spec.key:14s} {spec.name:26s} {shown}")
        if value is None:
            missing.append(spec.key)

    for spec in const.BINARY_SENSORS:
        value = const.dig(data, spec.path)
        shown = "-" if value is None else ("an" if value else "aus")
        print(f"  binary_sensor {spec.key:14s} {spec.name:26s} {shown}")
        if value is None:
            missing.append(spec.key)

    print()
    if missing:
        # Not a failure per se: a device with no meter attached has no readings
        # yet, and reports null for them on purpose.
        print(f"{len(missing)} without a value yet: {', '.join(missing)}")
    total = len(const.SENSORS) + len(const.BINARY_SENSORS)
    print(f"entities: {total - len(missing)}/{total} have a value")
    return 0


if __name__ == "__main__":
    sys.exit(main())
