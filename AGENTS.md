# AGENTS.md

## Decisions that shape this codebase

- **One image, two roles.** `env:t3s3` builds a single firmware for both boards;
  the role (basement meter node / flat gateway) is chosen once in the setup page
  and stored in NVS. Never reintroduce a per-device build flag or a `DEVICE_ID`
  define — identity comes from the chip MAC (`configStore.macString()`).
- **Home Assistant talks REST, not MQTT.** The gateway serves `/api/v1/state`
  and hands the owner the exact `packages/power_meter.yaml` (plus a Lovelace
  card) from `/ha/power_meter.yaml`, generated with its own IP and entity ids.
  MQTT discovery can be layered on later, but REST is the primary path.
- **The meter protocol is detected, not assumed.** `MeterProtocol::Auto` listens
  passively for SML, flips the RX polarity if not a single byte arrives, and
  only then tries an active IEC 62056-21 mode-C wake-up. Both protocol and
  polarity stay settable in the UI for meters that detection gets wrong.
- **Airtime is a legal limit, not a tuning knob.** 868.0–868.6 MHz allows 25 mW
  ERP (14 dBm) and 1 % duty cycle over a rolling hour (36 s). Output power is
  clamped to 14 dBm in `RadioLink::begin()` and every transmission passes
  `DutyCycleBudget`. Never bypass it, never raise the power.
- **The basement node keeps its AP forever** — it is the on-site status display.
  The gateway's AP exists only until it is provisioned, and comes back if the
  configured network cannot be joined.
- **Airtime is computed, not asked for.** `core::loraAirtimeMs()` implements the
  Semtech formula and is unit tested; do not swap it for a library call whose
  return unit you have not verified.

## Gotchas that cost time once already

- `#include <Esp.h>` — there is no `ESP.h` in this Arduino core.
- RadioLib does not bring up SPI: call
  `SPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN)` before
  `radio.begin()`, or the radio init fails with no obvious symptom.
- PlatformIO's Unity runner provides neither `TEST_CASE` nor `main()`: use
  `firmware/test/unity_case.h` (`#include "../unity_case.h"`).
- Unity needs `-DUNITY_INCLUDE_DOUBLE` for `TEST_ASSERT_DOUBLE_*`; without it
  every double assertion fails at runtime with "Double Precision Disabled".
- The USB CDC is not bidirectional the instant the host opens the port.
  `tools/e2e.py` probes with `help` until the board answers; do the same in any
  new harness before concluding the firmware is broken.
- **The dashboard JavaScript is a raw string in a C++ header, so nothing compiles
  it.** A stray `}` survived the merge in v0.1.5 and shipped: the page loaded, threw
  on the first parse error and left the owner stuck at a dead login screen with no
  working tabs. `python3 tools/check_web_assets.py` extracts both literals exactly
  as the firmware serves them and runs `node --check` (CI job *web assets*).
  Run it after any edit to `web_assets.h`; a compile and a green native test run
  say nothing about it.
- **`/api/state` is a hand-built JSON string, and one missing comma empties the
  whole dashboard.** The page's poll keeps the last good state on a parse error,
  so a malformed document shows every field as "–" and looks exactly like a
  device whose settings were wiped. After touching `buildStateJson()`, fetch the
  document and parse it (`curl -s <ip>/api/state | python3 -m json.tool`); the
  page now also raises a banner when it cannot parse. The device itself keeps
  measuring normally while this is broken.

## Firmware layout

- `firmware/lib/core/` — hardware-free logic (SML parser/framer, packet codec,
  duty-cycle accountant, airtime, ring buffer). No Arduino headers. This is what
  `pio test -e native` exercises.
- `firmware/src/` — the device: `app_config` (NVS settings + identity), `net`
  (AP/STA + captive portal), `radio_link`, `meter_reader`, `web_server`,
  `display_ui`, `cli`, `main`.
- `firmware/src/web_assets.h` — the whole web UI as PROGMEM strings. Edit the
  file directly; nothing is fetched from a CDN because the setup page is served
  from an access point without internet. Its header comment lists the layout
  rules (cards, `.actions`, `.pair`, `data-only` per role); keep to them instead
  of adding inline styles.
- **The in-page update (`System` → `Firmware-Update`) keeps the settings.** It
  writes only the inactive app slot; NVS, pairing and the WiFi configuration are
  untouched. When an update *does* look like it wiped the configuration, it is
  almost always one of three things, in this order: the state document no longer
  parses (web page empty, device fine), the `DeviceConfig` layout changed without
  appending at the end (see the comment in `app_config.h`), or a factory image was
  written where an `-update.bin` belongs.
- Releases come from CI: tag `v<VERSION>` → `.github/workflows/ci.yml` builds,
  `tools/make_release.py` packages (update / factory / browser-flasher parts),
  and the release plus the GitHub Pages flasher are published. The release
  version reaches the firmware through the `FW_VERSION` env var, never by
  editing `VERSION` in CI (that would stamp the build `-dirty`).

## Verification

- `./flash.sh` — builds, flashes every board, and reads the firmware stamp back
  off each one to prove the new image is really running.
- `./flash.sh native` — 33 host unit tests (SML, packet codec, duty cycle, crypto).
- `python3 tools/check_web_assets.py` — syntax-checks the shipped dashboard JS/CSS.
- `python3 tools/e2e.py` — 11 on-device checks over USB, including the meter
  simulator driving real SML telegrams through the parser.
- `./flash.sh monitor` then `meter loopback on` with TX jumpered to RX (GPIO40→39) —
  exercises the real UART path without the meter.
