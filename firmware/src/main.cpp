/*
 * LilyGO T3-S3 power-meter link.
 *
 * One firmware, two roles. The role is chosen once in the setup page and kept
 * in NVS:
 *
 *   Meter   (basement) - reads the electricity meter through an IR read head on
 *                        UART1, sends the values over 868 MHz LoRa, and keeps
 *                        its own access point up as an on-site status display.
 *   Gateway (flat)     - receives over LoRa and serves the values as JSON for
 *                        Home Assistant. Its AP exists only until it has been
 *                        provisioned.
 *
 * Everything that transmits passes the duty-cycle accountant first: 868.0-868.6
 * MHz allows 1 % airtime over a rolling hour, and that is enforced in code.
 */

#include <Arduino.h>
#include <Esp.h>

#include "app_config.h"
#include "cli.h"
#include "config.h"
#include "display_ui.h"
#include "firmware_version.h"
#include "ha_mqtt.h"
#include "meter_reader.h"
#include "net.h"
#include "radio_link.h"
#include "time_util.h"
#include "wall_clock.h"
#include "web_server.h"

// These two are referenced from other translation units (the web UI drives the
// display and reports the MQTT state), so they live at global scope.
DisplayUi displayUi;
HaMqtt haMqtt;

namespace {

RadioLink radioLink;
MeterReader meterReader;
WebUi webUi;

uint32_t lastSendMs = 0;
uint32_t lastTimeSyncMs = 0;
uint32_t lastTimeSyncTryMs = 0;
bool timePeerWasOnline = false;
uint32_t lastIdentityMs = 0;

// LED: solid when everything works, slow blink without a peer, fast blink when
// the meter has gone quiet - a glance at the basement tells the story.
void updateLed(uint32_t now) {
  const LinkStats& link = radioLink.stats();
  if (!link.radioOk) {
    digitalWrite(BOARD_LED, ((now / 150) % 2) ? HIGH : LOW);
    return;
  }
  if (!link.peerOnline) {
    digitalWrite(BOARD_LED, ((now / 700) % 2) ? HIGH : LOW);
    return;
  }
  if (configStore.roleIs(DeviceRole::Meter) && !meterReader.fresh(now, METER_STALE_MS)) {
    digitalWrite(BOARD_LED, ((now / 250) % 2) ? HIGH : LOW);
    return;
  }
  digitalWrite(BOARD_LED, LED_ON);
}

// Fills the meter model from a telemetry payload, so the gateway's display and
// REST output show what actually came over the air.
void applyIncomingTelemetry(const core::MeterTelemetry& telemetry) {
  meterReader.injectTelemetry(telemetry);
}

void handleInbound(const core::Packet& packet, uint32_t now) {
  switch (packet.type) {
    case core::PacketType::Data: {
      core::MeterTelemetry telemetry;
      if (!core::unpackTelemetry(packet.payload, packet.payloadLen, telemetry)) {
        logEvent("telemetry payload from %04X malformed", packet.src);
        return;
      }
      applyIncomingTelemetry(telemetry);
      // Push it to Home Assistant immediately rather than waiting for the
      // periodic publish: the reading is the whole point of the device.
      haMqtt.publishState(now, meterReader, radioLink);
      // Surfaced by the gateway's API; the node's own send is what matters there.
      if (configStore.roleIs(DeviceRole::Gateway)) {
        logEvent("reading from %04X: %.1f W, %.4f kWh", packet.src, telemetry.powerW,
                 telemetry.importWh10 / 10000.0);
      }
      break;
    }

    case core::PacketType::Status:
      break;  // liveness only; the link layer already tracked it

    case core::PacketType::TimeSync: {
      // Seconds and milliseconds since 1970 (UTC), taken just before the frame
      // went on air. Its airtime is added back: at SF7 that is ~70 ms, at SF12
      // over a second - far more than any other error in the chain.
      if (packet.payloadLen < 6) break;
      const uint64_t seconds = core::getLe(packet.payload, 4);
      const uint64_t millisPart = core::getLe(packet.payload + 4, 2);
      const uint32_t airtime =
          radioLink.estimatedAirtimeMs(core::kHeaderSize + packet.payloadLen + core::kLinkTagSize);
      clockSetFromRadio(seconds * 1000ULL + millisPart + airtime);
      break;
    }

    default:
      break;
  }
  (void)now;
}

// Role scheduler. The node is the one with something to say.
void schedulerTick(uint32_t now) {
  const DeviceConfig& cfg = configStore.get();
  if (!configStore.provisioned() || !radioLink.stats().radioOk) return;
  // Never faster than the airtime budget sustains at the current spreading
  // factor: running it dry silences the link for the rest of the hour.
  const uint32_t floorMs = radioLink.minSendIntervalMs();
  const uint32_t intervalMs = cfg.sendIntervalMs > floorMs ? cfg.sendIntervalMs : floorMs;
  if (now - lastSendMs < intervalMs) return;
  lastSendMs = now;

  if (cfg.role != DeviceRole::Meter) return;

  if (!meterReader.fresh(now, METER_STALE_MS)) {
    // No meter data: say so instead of sending stale numbers, so the gateway can
    // tell "basement link broken" from "meter link broken".
    uint8_t payload[4] = {0, 0, 0, 1};
    if (!radioLink.send(core::PacketType::Status, payload, sizeof(payload), cfg.peerShortId,
                        false)) {
      logEvent("status send refused (duty cycle)");
    }
    return;
  }

  const core::MeterTelemetry telemetry = core::toTelemetry(meterReader.values());
  if (!radioLink.sendTelemetry(telemetry, now)) {
    logEvent("telemetry deferred (duty cycle or radio busy)");
  }
}

// A board that has internet time passes it on: when the link comes up, when
// the other board has restarted, and every few hours against drift. Sealed like
// every other frame, so only the paired board can set the clock.
void timeSyncTick(uint32_t now) {
  const LinkStats& link = radioLink.stats();
  const bool restarted = radioLink.takePeerRestarted();
  if (clockSource() != ClockSource::Ntp || !link.peerOnline || !radioLink.encryptionEnabled()) {
    timePeerWasOnline = false;
    return;
  }
  const bool due = !timePeerWasOnline || restarted || lastTimeSyncMs == 0 ||
                   olderThan(now, lastTimeSyncMs, TIME_SYNC_INTERVAL_MS);
  timePeerWasOnline = true;
  if (!due) return;
  if (lastTimeSyncTryMs != 0 && !olderThan(now, lastTimeSyncTryMs, TIME_SYNC_RETRY_MS) && !restarted) {
    return;
  }
  lastTimeSyncTryMs = now;
  const uint64_t epochMs = clockEpochMs();
  if (epochMs == 0) return;
  uint8_t payload[6];
  core::putLe(payload, epochMs / 1000ULL, 4);
  core::putLe(payload + 4, epochMs % 1000ULL, 2);
  if (radioLink.send(core::PacketType::TimeSync, payload, sizeof(payload), configStore.get().peerShortId,
                     false)) {
    lastTimeSyncMs = now;
  } else {
    // Refused by the airtime budget or the radio: the retry gate above tries
    // again in half a minute.
    timePeerWasOnline = false;
  }
}

void logIdentity() {
  const DeviceConfig& cfg = configStore.get();
  logEvent("%s role=%s name='%s' mac=%s code=%s ap=%s", FW_ID,
           cfg.role == DeviceRole::Meter ? "meter" : (cfg.role == DeviceRole::Gateway ? "gateway" : "unset"),
           cfg.deviceName, configStore.macString(), configStore.setupCode(), configStore.apSsid());
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println(FW_BANNER);
  Serial.println(FW_TOKEN);

  pinMode(BOARD_LED, OUTPUT);
  digitalWrite(BOARD_LED, LED_ON);

  // Logged so the event log can answer "why did the board go quiet?": a held or
  // stuck BOOT button keeps GPIO0 low and the chip re-enters the ROM downloader
  // on the next reset - no application, no WiFi, nothing on USB.
  Serial.printf("[BOOT] reset reason %d, BOOT pin %s\n", static_cast<int>(esp_reset_reason()),
                digitalRead(0) == LOW ? "LOW (held or stuck - this selects the downloader!)" : "released");

  configStore.begin();
  logIdentity();

  displayUi.begin();
  radioLink.begin(configStore.get());
  meterReader.begin(configStore.get());
  netBegin();
  clockBegin();
  haMqtt.begin(configStore.get());
  webUi.begin(&radioLink, &meterReader);
  displayUi.attach(&radioLink, &meterReader);
  cliBegin();

  lastSendMs = millis();
  Serial.printf("setup done: role=%d ip=%s ap=%s\n", static_cast<int>(configStore.get().role),
                netIp().c_str(), netApUp() ? "up" : "down");
}

void loop() {
  const uint32_t now = millis();

  netLoop();
  radioLink.loop(now);
  meterReader.loop(now);
  webUi.loop();
  displayUi.loop(now);
  haMqtt.loop(now, meterReader, radioLink);
  cliPoll(radioLink, meterReader);

  core::Packet packet;
  while (radioLink.popReceived(packet)) handleInbound(packet, now);

  schedulerTick(now);
  clockLoop();
  timeSyncTick(millis());
  updateLed(now);

  // Identity line: makes the board findable on the serial console and proves to
  // flash.sh which firmware is actually running.
  // Every 5 s: frequent enough that a host can find the board without knowing
  // which /dev/ttyACM* it landed on, quiet enough to stay out of the way.
  if (now - lastIdentityMs > 5000) {
    lastIdentityMs = now;
    Serial.printf("[HB] %s up=%lus role=%d peer=%d meter=%s heap=%lu duty=%lu%%\n", FW_TOKEN,
                  (unsigned long)(now / 1000), static_cast<int>(configStore.get().role),
                  radioLink.stats().peerOnline ? 1 : 0, meterReader.activeProtocolName(),
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)radioLink.budget().usedPercent(now));
  }

  if (webUi.restartPending() && now >= webUi.restartAtMs()) {
    Serial.println(F("restarting"));
    delay(120);
    ESP.restart();
  }
}
