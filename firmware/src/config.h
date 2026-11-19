#pragma once
// Pins and compile-time defaults for the power-meter link firmware.
//
// Device identity is NOT a compile-time constant: one image runs on both
// boards, the role is chosen once in the setup page and stored in NVS.

#include <Arduino.h>

#include "utilities.h"
#include <ctype.h>
#include <string.h>

// ── Pins the board exposes for the meter read head ────────────
// GPIO43/44 are the QWIIC "Uart1" pins, the only free GPIOs on the SX1262
// variant besides 10 and 21. They are only free because the board JSON moves
// Serial to the native USB CDC (ARDUINO_USB_CDC_ON_BOOT) - without that they
// are UART0 and the console owns them.
// Defaults for a board whose read head is wired to the expansion header, where
// IO40/IO41 are free: GPIO43/44 sit on the QWIIC connector, which not every
// installation has. Both are JTAG pads that the ESP32-S3 only uses if an
// external probe is attached - the built-in USB-Serial/JTAG runs on GPIO19/20,
// so flashing and the console are unaffected. Still settable at runtime; these
// are just what a factory reset falls back to.
#define METER_UART_RX_PIN 40  // read head data out -> node RX
#define METER_UART_TX_PIN 41  // node TX -> read head (D0 wake-up only)
#define METER_UART_NUM 1

// Which pins may carry the meter UART. The ESP32-S3 can route a UART almost
// anywhere, so the question is what must NOT be touched. Blocked:
//   0            BOOT button and boot-mode strapping
//   1            battery divider
//   2,11,13,14   SD card (wired on the board, unused by this firmware)
//   3            LoRa MISO, and the JTAG-source strapping pin
//   5,6,7,8      LoRa SPI and reset
//   17,18        OLED I2C
//   19,20        native USB (console and flashing)
//   26..32       SPI flash / PSRAM
//   33,34        LoRa DIO1 and BUSY
//   35,36        quad-PSRAM module pins
//   37           onboard LED
//   39..42        usable, but they are the external JTAG pads - fine here
//                 because nothing attaches a JTAG probe
//   45           VDD_SPI strapping - driving it selects the 1.8 V flash rail
//   46           boot-mode strapping - a UART line idles HIGH, which would stop
//                the ROM serial bootloader, so the board could not be flashed
//                while the read head is attached
inline bool meterPinAllowed(int pin) {
  if (pin < 1 || pin > 48) return false;
  switch (pin) {
    case 0: case 1: case 2: case 3: case 5: case 6: case 7: case 8:
    case 11: case 13: case 14: case 17: case 18: case 19: case 20:
    case 26: case 27: case 28: case 29: case 30: case 31: case 32:
    case 33: case 34: case 35: case 36: case 37: case 45: case 46:
      return false;
    default:
      return true;
  }
}
// Two GPIOs on the expansion header that nothing on the board claims, kept named
// so a future use does not have to rediscover them.
constexpr uint8_t kSparePin1 = 10;
constexpr uint8_t kSparePin2 = 21;

// ── LoRa link defaults ───────────────────────────────────────
// 868.1 MHz sits inside 868.0-868.6 MHz (ERC/REC 70-03 Annex 1 h1.5, BNetzA
// Vfg. 91/2025): at most 25 mW ERP and 1 % duty cycle over a rolling hour.
// 868.0 exactly is the band edge and a BW125 signal there spills out of band,
// which is why the default moved up.
#define LORA_DEFAULT_FREQ_MHZ 868.1f
// The window the firmware actually transmits in, and the range it accepts from
// the web form. Below or above this a 125 kHz signal is either outside
// 868.0-868.6 MHz or so close to the edge that it spills out of band - which is
// a legal problem, not a broken link, so such a value is refused rather than
// stored. One definition, used by the save path, the load-time repair and the
// value the settings page shows.
#define LORA_FREQ_MIN_MHZ 868.0625f
#define LORA_FREQ_MAX_MHZ 868.5375f
#define LORA_DEFAULT_BW_KHZ 125.0f
#define LORA_DEFAULT_SF 7
#define LORA_DEFAULT_CR 5  // 4/5 - the least airtime per byte
#define LORA_DEFAULT_POWER_DBM 14
#define LORA_DEFAULT_CURRENT_LIMIT 140
#define LORA_SYNC_WORD 0x12
#define LORA_PREAMBLE 8

// Legal airtime budget: 1 % of one hour.
#define LORA_DUTY_WINDOW_MS (60UL * 60UL * 1000UL)
#define LORA_DUTY_BUDGET_MS (36UL * 1000UL)

// ── Timing ───────────────────────────────────────────────────
// 15 s keeps a meter at SF7 at about 60 % of its hourly airtime budget. 10 s
// needed 37 s of the 36 s allowed, so the budget ran out every hour and the
// link went quiet until it recovered.
#define SEND_INTERVAL_DEFAULT_MS 15000UL
#define SEND_INTERVAL_MIN_MS 2000UL
#define SEND_INTERVAL_MAX_MS 600000UL
#define PAIRING_RETRY_MS 3000UL
// How long a pairing attempt runs before it reports failure. Long enough to
// move a board into range, short enough that nobody is left wondering.
#define PAIRING_TIMEOUT_MS 60000UL
// The peer counts as lost after this long without a packet. Readings and their
// acknowledgements arrive every send interval; a silent side sends a heartbeat
// every HEARTBEAT_MS, so this tolerates three lost heartbeats in a row.
#define PEER_TIMEOUT_MS 130000UL
#define PEER_PROBE_MS 75000UL
// Sent only after this long without any other transmission.
#define HEARTBEAT_MS 30000UL
// With nothing received for this long, the receiver is checked and re-armed.
#define RX_WATCHDOG_MS 45000UL
#define MQTT_RETRY_MS 5000UL

// ── WiFi ─────────────────────────────────────────────────────
#define AP_SSID_PREFIX "LilyGo-Power"
#define AP_CHANNEL 6
#define AP_MAX_CLIENTS 4
// If a configured network cannot be joined, the setup AP comes back so a wrong
// password can never lock the owner out of the device.
#define STA_FAIL_BACKOFF_MS (5UL * 60UL * 1000UL)
#define STA_CONNECT_TIMEOUT_MS 20000UL
// How long the network has to hold before the access point is given up. A link
// that drops and returns would otherwise take the AP - and the page the owner is
// using - down with it on every single join.
#define STA_STABLE_MS (120UL * 1000UL)

// ── Time ─────────────────────────────────────────────────────
// Wall-clock time comes from NTP on a board with internet access (normally the
// gateway) and is passed to the other board over the link.
#define TIME_ZONE_POSIX "CET-1CEST,M3.5.0,M10.5.0/3"  // Europe/Berlin
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.cloudflare.com"
// How often the board with internet time re-sends it. A crystal drifts well
// under a second in that time; the sync costs about 70 ms of airtime.
#define TIME_SYNC_INTERVAL_MS (6UL * 60UL * 60UL * 1000UL)
#define TIME_SYNC_RETRY_MS (30UL * 1000UL)

// ── Meter ────────────────────────────────────────────────────
#define METER_SML_BAUD 9600
#define METER_D0_WAKE_BAUD 300
#define METER_D0_DATA_BAUD 9600
// Passive listen window before attempting an active D0 wake-up.
#define METER_AUTODETECT_MS 12000UL
// Passive listen window per polarity in auto mode, and how many active D0
// wake-up rounds to try before cycling back to listening. The cycle matters in
// the field: a head that is only positioned on the meter later must still be
// found without a reboot.
#define METER_LISTEN_MS 15000UL
#define METER_D0_ROUNDS 3
// The meter simulator is a test mode and switches itself off: its readings look
// exactly like real ones, and a forgotten simulator sends an owner hunting for
// a fault on the meter side that does not exist.
#define SIMULATOR_TIMEOUT_MS (15UL * 60UL * 1000UL)
// A telegram older than this means the meter link is broken, not slow.
#define METER_STALE_MS 30000UL

// ── Web ──────────────────────────────────────────────────────
#define WEB_PORT 80
// Login throttling: after this many wrong codes in a row the page waits
// LOGIN_LOCKOUT_MS before it answers another attempt.
#define LOGIN_ATTEMPT_LIMIT 5
#define LOGIN_LOCKOUT_MS (30UL * 1000UL)

#define SESSION_TIMEOUT_MS (30UL * 60UL * 1000UL)
#define WIFI_SCAN_MAX 20

// ── Storage ──────────────────────────────────────────────────
#define CFG_NAMESPACE "powermeter"
#define LOG_CAPACITY 48
// The state document is built into one buffer reserved at boot (see
// stateJsonBuffer()); MQTT publishes it without the log.
#define STATE_JSON_RESERVE 14336
#define MQTT_BUFFER_SIZE 6144
