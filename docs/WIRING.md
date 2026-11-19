# Wiring: IR read head on the LilyGO T3-S3

The read head is the "TTL to Infrared Meter Reader / IEC 62056-21" type: a
transparent TTL↔IR bridge with a photodiode for receiving and an IR LED for
sending. It needs three wires (four if the meter is an older D0 type).

## Pin map

| Read head | T3-S3 pin | Notes |
|---|---|---|
| VCC | **3V3** | Most heads run fine on 3.3 V. If yours needs 5 V, use the `5V` pin next to it. |
| GND | **GND** | |
| Data out (head TX) | **IO39** | the node's receive line |
| Data in (head RX) | **IO40** | only used for the active D0 wake-up. Leave unconnected if your meter pushes SML by itself. |

That is the default the firmware ships with and what a factory reset falls back
to. **If your head is wired elsewhere, nothing is lost:** the reader listens to
the configured pin in both polarities for 15 s, and when no telegram ever
arrives it walks the other free pins (39, 40, 38, 42, 41, 43, 44, 15, 16, 21, 10),
listening a few seconds on each. The first pin that completes a telegram is
adopted and stored, and the event log says so. The manual way is *Zähler* →
*Daten-Pins* in the web UI, or `meter pins <rx> <tx>` over USB.

IO39, IO40, IO41 and IO42 are on the 2×13 expansion header. They are the
ESP32-S3's JTAG pads, which the chip only uses when an external probe is
attached — the built-in USB-Serial/JTAG (flashing and the console) runs on
GPIO19/20, so they are free for this. They are also *not* strapping pins, unlike
45 and 46, so nothing they do at reset can upset the boot.

**Only the data-out line matters.** The second wire is needed solely for the
active D0 wake-up on older meters; with a passive head in front of an SML meter,
leave it off.

The pins can be moved at any time — web UI (*Zähler* → *Daten-Pins*) or
`meter pins <rx> <tx>` over USB. `meterPinAllowed()` refuses the pins that must
not carry a UART: 0/3/45/46 (strapping — 45 selects the flash rail, and 46 would
block the ROM bootloader, since a UART line idles HIGH), 19/20 USB, 26–37
flash/PSRAM/LoRa/LED, 17/18 OLED, 2/11/13/14 SD, 1 battery divider. Other free
pins: 15, 16, 38, 39, 40, 41, 42 on the header, 43/44 and 10/21 on the QWIIC
connectors.

Power the node from a USB-C supply. Do not power the read head from the meter.

## Test without the meter: loopback

Put a jumper wire between **the TX pin and the RX pin** (GPIO 40 → GPIO 39 with
the default pin map, or GPIO 43 → GPIO 44 if you moved the UART to the QWIIC
connector), then either

```
./flash.sh monitor        # then type: meter loopback on
```

or use the web UI (*Zähler* tab → *Loopback an*). The firmware writes synthesised
SML telegrams out of the TX pin, they come back in on the RX pin, and the parser has to
decode them from the real electrical signal. If values appear, the UART path,
the polarity handling and the parser are all confirmed — everything except the
meter itself.

If the values do *not* appear on real hardware, the culprit is almost always
polarity: set *Lesekopf-Polarität* to the other value in the web UI.

## Placing the head on the meter

- Most German meters have an optical interface on the front, usually labelled
  "Info" or marked with an IR diode symbol. The head has to sit straight on it.
- Many heads come with a magnet ring that snaps onto the meter — use it.
- Some meters only emit the basic reading until the interface is unlocked with a
  PIN or a magnet/torch sequence. Check the meter's manual; nothing in this
  firmware can work around that.
- The optical interface is read-only information. Never modify the meter's
  installation.

## If the meter is an older D0 / IEC 62056-21 type

Set the protocol explicitly (web UI → *Zähler* → *Protokoll* → `D0`). The
firmware then sends the 300-baud wake-up sequence from the TX pin and switches to
9600 baud for the readout. That is why the fourth wire matters here.

## Antennas

Both boards need their LoRa antenna attached **before** powering up. The SX1262
is configured to transmit at 14 dBm (25 mW ERP) — the legal maximum for
868.0–868.6 MHz — and running without an antenna reflects that power back into
the amplifier.
