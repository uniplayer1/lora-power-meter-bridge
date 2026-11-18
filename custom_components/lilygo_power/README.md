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

One device with a "Bezug"/"Einspeisung" energy pair (kWh, `total_increasing`,
usable in the energy dashboard), current power overall and per phase, the age of
the last reading, link quality (RSSI/SNR/age), the share of the legal transmit
budget used, telegram and checksum-error counters, uptime and free memory — plus
two binary sensors: *Keller-Link* (is the basement board answering?) and
*Messung gültig*.

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
