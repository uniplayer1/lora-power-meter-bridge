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


# Which Home Assistant device an entity belongs to. The meter readings and the
# meter node's own report sit on a device of their own ("Stromzähler Keller"),
# linked to the gateway; the gateway's airtime, memory and reception stay on
# the gateway. Home Assistant prefixes entity names with the device name, so
# with everything on one device the gateway's airtime read like the meter's.
# The keys (and with them the unique ids) did not change.
OWNER_GATEWAY = "gateway"
OWNER_NODE = "node"
NODE_DEVICE_NAME = "Stromzähler Keller"
NODE_MODEL = "T3-S3 Zähler-Funkmodul"


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
    owner: str = OWNER_GATEWAY
    diagnostic: bool = False
    # Available only while this part of the JSON exists (the node's report
    # arrives a few seconds after the link is up, and is null before).
    requires: tuple[str, ...] | None = None


_N = OWNER_NODE
_REPORT = ("peer_status",)

SENSORS: tuple[EntitySpec, ...] = (
    # The meter, read in the basement and received over the link.
    EntitySpec("import", "Bezug", ("meter", "import_kwh"), "kWh", "energy", "total_increasing", 3, _N),
    EntitySpec("export", "Einspeisung", ("meter", "export_kwh"), "kWh", "energy", "total_increasing", 3, _N),
    EntitySpec("power", "Leistung", ("meter", "power_w"), "W", "power", "measurement", 1, _N),
    EntitySpec("power_l1", "Leistung L1", ("meter", "l1_w"), "W", "power", "measurement", 1, _N),
    EntitySpec("power_l2", "Leistung L2", ("meter", "l2_w"), "W", "power", "measurement", 1, _N),
    EntitySpec("power_l3", "Leistung L3", ("meter", "l3_w"), "W", "power", "measurement", 1, _N),
    EntitySpec("meter_age", "Alter der Messung", ("meter", "age"), "s", "duration", "measurement", 0, _N, True),
    EntitySpec("telegrams", "Telegramme", ("meter", "telegrams"), None, None, "total_increasing", 0, _N, True),
    EntitySpec("crc_errors", "Prüfsummenfehler", ("meter", "crc_errors"), None, None, "total_increasing", 0, _N, True),
    # The meter node's own status, from its report every few minutes.
    EntitySpec("node_duty", "Sendezeit-Budget genutzt", ("peer_status", "duty_pct"), "%", None, "measurement", 1, _N, True, _REPORT),
    EntitySpec("node_blocked", "Zurückgehaltene Sendungen", ("peer_status", "blocked"), None, None, "measurement", 0, _N, True, _REPORT),
    EntitySpec("node_rssi", "Empfang vom Gateway RSSI", ("peer_status", "rssi"), "dBm", "signal_strength", "measurement", 0, _N, True, _REPORT),
    EntitySpec("node_snr", "Empfang vom Gateway SNR", ("peer_status", "snr"), "dB", None, "measurement", 1, _N, True, _REPORT),
    EntitySpec("node_heap", "Freier Speicher", ("peer_status", "heap"), "B", "data_size", "measurement", 0, _N, True, _REPORT),
    EntitySpec("node_heap_min", "Speicher-Tiefstwert", ("peer_status", "heap_min"), "B", "data_size", "measurement", 0, _N, True, _REPORT),
    EntitySpec("node_uptime", "Laufzeit", ("peer_status", "uptime"), "s", "duration", "total_increasing", 0, _N, True, _REPORT),
    EntitySpec("node_bytes", "Bytes vom Lesekopf", ("peer_status", "bytes"), None, None, "total_increasing", 0, _N, True, _REPORT),
    EntitySpec("node_report_age", "Letzter Statusbericht", ("peer_status", "age"), "s", "duration", "measurement", 0, _N, True, _REPORT),
    # The gateway itself.
    EntitySpec("rssi", "Empfang vom Keller RSSI", ("link", "rssi"), "dBm", "signal_strength", "measurement", 0, diagnostic=True),
    EntitySpec("snr", "Empfang vom Keller SNR", ("link", "snr"), "dB", None, "measurement", 1, diagnostic=True),
    EntitySpec("link_age", "Letztes Paket vom Keller", ("link", "age"), "s", "duration", "measurement", 0, diagnostic=True),
    EntitySpec("duty", "Sendezeit-Budget genutzt", ("link", "duty_pct"), "%", None, "measurement", 1, diagnostic=True),
    EntitySpec("uptime", "Laufzeit", ("device", "uptime"), "s", "duration", "total_increasing", 0, diagnostic=True),
    EntitySpec("heap", "Freier Speicher", ("device", "heap"), "B", "data_size", "measurement", 0, diagnostic=True),
)

BINARY_SENSORS: tuple[EntitySpec, ...] = (
    EntitySpec("peer", "Funkverbindung zum Keller", ("link", "peer"), None, "connectivity"),
    EntitySpec("meter_valid", "Messung gültig", ("meter", "valid"), None, "connectivity", owner=_N),
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
