# Setup (for the person installing the two boards)

You need: the two boards, their LoRa antennas, two USB-C power supplies, and a
phone. Ten minutes.

The boards must already run the firmware. If they are new, flash them first; it
takes a minute in the browser: **[FLASHING.md](FLASHING.md)**.

## 1. Antennas first

Screw on both antennas **before** plugging anything in.

## 2. Which board is which

Both boards run exactly the same firmware. You decide what each one does during
setup, and the choice is remembered:

- **Keller · Zähler** — goes next to the electricity meter. Reads the meter
  through the infrared head and sends the values over LoRa. Keeps its own WiFi
  permanently so you can check on it from a phone.
- **Wohnung · Gateway** — stays in the flat, receives over LoRa and feeds Home
  Assistant.

## 3. Set up the basement board

1. Plug it in. The display shows the WiFi name (e.g. `LilyGo-Power-ABCD`) and an
   8-character code.
2. On your phone, join that WiFi network with that code.
3. The setup page opens by itself (if not, browse to `http://192.168.4.1`).
4. Choose **Keller · Zähler** and give it a name. Home WiFi is optional for this
   board. The read-head settings (protocol, polarity, pins) are folded away
   because automatic detection is right for almost every meter.
5. Save. The board starts reading the meter and keeps its WiFi up. If you
   changed its WiFi name or code, the page shows the new ones: rejoin with them
   and open `http://192.168.4.1`. Otherwise it goes straight to the dashboard,
   which you can come back to any time.

The basement board does **not** need your home WiFi.

## 4. Set up the flat board

1. Plug it in, join its WiFi (`LilyGo-Power-XXXX`, same idea), open
   `http://192.168.4.1`.
2. Choose **Wohnung · Gateway**, pick your home WiFi from the list and enter its
   password.
3. Save. The page stays open while the board joins your home WiFi and then
   shows its new address (IP and `http://lilygo-power-XXXX.local`; the IP is on
   the display too). Switch your phone back to your home WiFi and tap the
   address. The board's own WiFi switches off about two minutes later. If it
   cannot join (wrong password, 5 GHz-only network), the page says so and lets
   you correct it.
4. Pair the two boards: on **one** board's page → *Funk* → **Koppeln**, with both
   boards in range (set them up next to each other the first time). The other
   board answers by itself; both show *verbunden* within a few seconds.
   Pressing it on both boards also works. **Neu koppeln** on a paired board
   keeps the old pairing if no other board answers within a minute.
5. Nothing to type for encryption: pairing agrees a new key by itself. Both
   boards show the same four-digit *Kontrollzahl* (page *Funk* → *Verschlüsselung*,
   and on the display's *Funkstrecke* page). If the two numbers differ, pair again.

## 5. Home Assistant

1. On the gateway page, tab *Home Assistant* → download the **YAML-Paket**
   (`power_meter.yaml`).
2. Copy it into your Home Assistant config folder under `packages/`.
3. Add this once to `configuration.yaml`:

   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```

4. Restart Home Assistant. Entities appear: the readings named *Stromzähler …*
   (import/export energy, power and per phase), the basement board's own status
   as *Keller …* and the flat board's as *Wohnung …*. Over MQTT or with the
   integration they are grouped into two devices, *Stromzähler Keller* and the
   gateway.
5. Optional: download the **Dashboard-Karte** too and paste it as a new Lovelace
   card. If you run an MQTT broker, enter it on the same tab instead; the
   entities then appear without any file.

## 6. Mount the meter head

See `docs/WIRING.md`. Test the electronics first with the loopback jumper, then
place the head on the meter.

## Day to day

- **Basement board**: join its WiFi to see status, meter values, link quality
  and the airtime used. The display cycles with the BOOT button.
- **Reset**: web page → *System* → *Werkszustand* starts over from step 3. To
  change only the role, use *System* → *Rolle ändern*.
- **Code off the display**: *System* → *Anmeldung* → *Code auf dem Display
  ausblenden*. Meant for a board that stands where visitors walk past it; the
  code stays the WiFi password, so read it off once before switching it on. It
  is still in the browser's password manager or your notes. Forgotten and no
  longer readable: plug the board into USB and run `webpass show on` in the
  console.
- **Update**: web page → *System* → *Firmware-Update*, pick the
  `…-update.bin` from the latest release. Settings are kept. Details:
  [FLASHING.md](FLASHING.md#2-updating-later-over-wifi-ota).
- **Troubleshooting**: if the meter shows nothing, *Zähler* → *Lesekopf* says
  whether anything arrives on the wire, and *Diagnose* → *Leitung messen* tests
  the head itself. If bytes arrive but nothing decodes, set the other polarity.
  If the link is down, check the antennas and that both boards use the same
  frequency and SF on the *Funk* tab.

## For whoever maintains the firmware

```
./flash.sh            # build once, flash every board found, verify the version
./flash.sh native     # host unit tests
python3 tools/e2e.py  # on-device end-to-end checks over USB
./flash.sh version    # read the firmware stamp off a running board
```
