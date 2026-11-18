"""Constants and the entity table.

Kept free of homeassistant imports on purpose: this file is imported by the
offline test suite (tools/ha_payload_test.py) as well as by the integration, so
the mapping from the device's JSON to entities can be checked without running
Home Assistant.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Sequence

DOMAIN = "lilygo_power"
MANUFACTURER = "LilyGo"
MODEL = "T3-S3 power meter link"
DEFAULT_SCAN_INTERVAL = 10  # seconds; matches the device's own send interval
CONF_HOST = "host"
API_PATH = "/api/v1/state"


@dataclass(frozen=True)
class EntitySpec:
    """One entity, and where its value lives in the device's JSON."""

    key: str
    name: str
    path: tuple[str, ...]
    unit: str | None = None
    device_class: str | None = None
    state_class: str | None = None
    suggested_precision: int | None = None


SENSORS: tuple[EntitySpec, ...] = (
    EntitySpec("import", "Bezug", ("meter", "import_kwh"), "kWh", "energy", "total_increasing", 3),
    EntitySpec("export", "Einspeisung", ("meter", "export_kwh"), "kWh", "energy", "total_increasing", 3),
    EntitySpec("power", "Leistung", ("meter", "power_w"), "W", "power", "measurement", 1),
    EntitySpec("power_l1", "Leistung L1", ("meter", "l1_w"), "W", "power", "measurement", 1),
    EntitySpec("power_l2", "Leistung L2", ("meter", "l2_w"), "W", "power", "measurement", 1),
    EntitySpec("power_l3", "Leistung L3", ("meter", "l3_w"), "W", "power", "measurement", 1),
    EntitySpec("meter_age", "Alter der Messung", ("meter", "age"), "s", "duration", "measurement", 0),
    EntitySpec("telegrams", "Telegramme", ("meter", "telegrams"), None, None, "total_increasing", 0),
    EntitySpec("crc_errors", "Pruefsummenfehler", ("meter", "crc_errors"), None, None, "total_increasing", 0),
    EntitySpec("rssi", "Link RSSI", ("link", "rssi"), "dBm", "signal_strength", "measurement", 0),
    EntitySpec("snr", "Link SNR", ("link", "snr"), "dB", None, "measurement", 1),
    EntitySpec("link_age", "Link Alter", ("link", "age"), "s", "duration", "measurement", 0),
    EntitySpec("duty", "Sendezeit", ("link", "duty_pct"), "%", None, "measurement", 1),
    EntitySpec("uptime", "Laufzeit", ("device", "uptime"), "s", "duration", "total_increasing", 0),
    EntitySpec("heap", "Freier Speicher", ("device", "heap"), "B", None, "measurement", 0),
)

BINARY_SENSORS: tuple[EntitySpec, ...] = (
    EntitySpec("peer", "Keller-Link", ("link", "peer"), None, "connectivity"),
    EntitySpec("meter_valid", "Messung gueltig", ("meter", "valid"), None, "connectivity"),
)


def dig(data: Any, path: Sequence[str]) -> Any:
    """Reads a nested key, returning None instead of raising.

    A missing value must not take the whole integration down: a device that has
    just rebooted answers with nulls for everything it has not measured yet.
    """
    current = data
    for key in path:
        if not isinstance(current, dict) or key not in current:
            return None
        current = current[key]
    return current
