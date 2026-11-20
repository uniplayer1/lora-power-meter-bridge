# LilyGo Power Meter (Home Assistant integration)

The gateway board advertises itself on the network over mDNS
(`_lilygopower._tcp`), so this integration can offer it as a discovered device:
**Settings → Devices & Services → Discovered → Add**. No broker, no YAML, no
typing an address.

## Install

1. Copy the folder `custom_components/lilygo_power` into your Home Assistant
   configuration directory, so you end up with
   `<config>/custom_components/lilygo_power/manifest.json`.
2. Restart Home Assistant.
3. The board should appear under *Discovered*. If it does not, use
   *Add integration → LilyGo Power Meter* and enter its address (shown on the
   device's own page).

Requires Home Assistant 2024.4 or newer (`config_flow.py` imports the current
zeroconf service info helper).

## What it creates

Two devices, so it is clear which board a value belongs to:

- **Stromzähler Keller** (linked to the gateway): the "Bezug"/"Einspeisung"
  energy pair (kWh, `total_increasing`, usable in the energy dashboard), current
  power overall and per phase, *Messung gültig*, and as diagnostics the age of
  the last reading, telegram and checksum-error counters plus the meter node's
  own report: share of its airtime budget used, held-back sends, how it hears
  the gateway, free memory, uptime, bytes from the read head and the age of the
  report.
- **The gateway** (named as on its setup page): its own airtime budget,
  reception of the meter node (RSSI/SNR, age of the last packet), uptime, free
  memory and *Funkverbindung zum Keller*.

The unique ids of entities that existed before the split are unchanged, so
entity ids and the energy history stay.

## How this differs from the other two paths

| Path | What it needs | What you do |
|---|---|---|
| **This integration** | nothing else | press Add on the discovered device |
| MQTT discovery | a broker (e.g. the Mosquitto add-on) | enter broker address on the device page |
| REST package (`/ha/power_meter.yaml`) | nothing | copy one YAML file into `config/packages/` |

All three read the same values from the same endpoint (`/api/v1/state`), so they
can coexist; use one.

## Why the device cannot just "appear" without any of this

Home Assistant only turns an announcement into entities when it has an
integration that understands *that* announcement. A device speaking plain HTTP
is not something HA can interpret on its own — that is what this integration is
for. It watches for `_lilygopower._tcp` and knows how to read the JSON behind
it.
