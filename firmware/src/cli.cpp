#include "cli.h"

#include <Esp.h>
#include <WiFi.h>
#include <esp_system.h>

#include "config.h"
#include "display_ui.h"
#include "firmware_version.h"
#include "ha_mqtt.h"
#include "net.h"
#include "time_util.h"

namespace {

String buffer;

void printHelp() {
  Serial.println(F("commands:"));
  Serial.println(F("  status              one-line health summary"));
  Serial.println(F("  config              dump all settings"));
  Serial.println(F("  duty                airtime budget for the rolling hour"));
  Serial.println(F("  meter dump          raw last telegram + parser view"));
  Serial.println(F("  meter sim on|off    synthesise telegrams"));
  Serial.println(F("  meter loopback on|off   drive telegrams out of TX (jumper TX to RX)"));
  Serial.println(F("  meter protocol auto|sml|d0"));
  Serial.println(F("  meter polarity auto|normal|inverted"));
  Serial.println(F("  meter pins <rx> <tx>   move the read-head UART (e.g. 39 40)"));
  Serial.println(F("  meter scope [ms]    measure the RX line itself (idle high/low/activity)"));
  Serial.println(F("  provision meter|gateway [name]   set the role and restart"));
  Serial.println(F("  log                 the on-device event log (incl. login attempts)"));
  Serial.println(F("  webpass [<new>|reset]  show/change/reset the web login code"));
  Serial.println(F("  page [next|N]       show or set the OLED page"));
  Serial.println(F("  mqtt host <addr> [port]  point the device at a broker"));
  Serial.println(F("  mqtt off            broker off"));
  Serial.println(F("  mqtt dump           discovery topics + state payload"));
  Serial.println(F("  mqtt announce       publish the Home Assistant discovery now"));
  Serial.println(F("  apname <name>       rename this device's own access point"));
  Serial.println(F("  apcode <code>       change the AP password / setup code (min 8)"));
  Serial.println(F("  link secret [hex]   show or set the link secret (32 hex chars, normally agreed by pairing)"));
  Serial.println(F("  radio tx            send one status packet"));
  Serial.println(F("  radio pair          start pairing"));
  Serial.println(F("  selftest            on-device self check"));
  Serial.println(F("  reboot | factory    restart / wipe settings"));
  Serial.println(F("  help"));
}

void printStatus(RadioLink& link, MeterReader& meter) {
  const DeviceConfig& cfg = configStore.get();
  const LinkStats& ls = link.stats();
  const MeterStats& ms = meter.stats();
  const core::MeterValues& values = meter.values();
  const uint32_t now = millis();

  Serial.printf("fw=%s mac=%s role=%d name=%s\n", FW_ID, configStore.macString(),
                static_cast<int>(cfg.role), cfg.deviceName);
  Serial.printf("net=%04X peer=%04X paired=%d online=%d rssi=%d snr=%.1f age=%lus\n", cfg.netId,
                cfg.peerShortId, link.paired() ? 1 : 0, ls.peerOnline ? 1 : 0, ls.peerRssi,
                ls.peerSnr, (unsigned long)ageSeconds(now, ls.lastPeerSeenMs));
  Serial.printf("pairing: %s attempts=%lu result='%s'\n", link.pairStateName(),
                (unsigned long)ls.pairingAttempts, ls.pairingResult);
  Serial.printf("meter=%s valid=%d age=%lus power=%.1fW import=%.4fkWh crc_err=%lu telegrams=%lu\n",
                meter.activeProtocolName(), values.valid ? 1 : 0,
                (unsigned long)ageSeconds(now, ms.lastTelegramAtMs),
                values.powerW(core::kObisPowerTotal), values.energyWh(core::kObisImportTotal) / 1000.0,
                (unsigned long)ms.crcErrors, (unsigned long)ms.telegrams);
  // The raw byte count is the first thing to look at when the meter reports
  // nothing: zero means nothing is driving the RX pin at all (head unpowered,
  // swapped wires, or simply not sitting on the meter yet), which is a
  // completely different problem from "bytes arrive but do not decode".
  Serial.printf("meter wire: bytes=%lu parse_err=%lu polarity_flips=%lu d0=%lu/%lu inverted=%d\n",
                (unsigned long)ms.bytesSeen, (unsigned long)ms.parseErrors,
                (unsigned long)ms.inversionsTried, (unsigned long)ms.d0Sessions,
                (unsigned long)ms.d0Failures, ms.invertedNow ? 1 : 0);
  Serial.printf("radio ok=%d tx=%lu rx=%lu fail=%lu blocked=%lu airtime=%lums duty=%lu%%\n",
                ls.radioOk ? 1 : 0, (unsigned long)ls.txPackets, (unsigned long)ls.rxPackets,
                (unsigned long)ls.txFailures, (unsigned long)ls.txBlocked,
                (unsigned long)ls.airtimeMs, (unsigned long)link.budget().usedPercent(now));
  // Two counters, not one: "the air was noisy" and "this frame was not written by
  // our peer" look the same on screen and are entirely different faults.
  Serial.printf("link crypto: %s, tag failures=%lu, plaintext dropped=%lu\n",
                link.encryptionEnabled() ? "on" : "off", (unsigned long)ls.rxAuthFailures,
                (unsigned long)ls.rxPlaintextDropped);
  Serial.printf("net mode=%s ip=%s host=%s heap=%lu uptime=%lus\n", netModeName(), netIp().c_str(),
                netHostName().c_str(), (unsigned long)ESP.getFreeHeap(),
                (unsigned long)(now / 1000));
  Serial.printf("ap ssid='%s' code=%s up=%d clients=%lu portal=%d\n", configStore.apSsid(),
                configStore.setupCode(), netApUp() ? 1 : 0, (unsigned long)netApClients(),
                netPortalActive() ? 1 : 0);
  // Reset reasons matter on this board: a held or stuck BOOT button (GPIO0 low
  // at reset) drops the chip into the ROM downloader, which looks exactly like
  // "the board vanished" - no WiFi, no serial output, no application.
  const char* reason = "unknown";
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: reason = "power-on"; break;
    case ESP_RST_SW: reason = "software"; break;
    case ESP_RST_PANIC: reason = "panic/crash"; break;
    case ESP_RST_INT_WDT: reason = "interrupt watchdog"; break;
    case ESP_RST_TASK_WDT: reason = "task watchdog"; break;
    case ESP_RST_WDT: reason = "watchdog"; break;
    case ESP_RST_BROWNOUT: reason = "BROWNOUT"; break;
    case ESP_RST_DEEPSLEEP: reason = "deep sleep wake"; break;
    case ESP_RST_EXT: reason = "external pin"; break;
    case ESP_RST_SDIO: reason = "sdio"; break;
    default: break;
  }
  Serial.printf("boot: %s (%d)  btn_raw=%d\n", reason, static_cast<int>(esp_reset_reason()),
                digitalRead(BUTTON_PIN) == LOW ? 1 : 0);

  const MqttStats& mq = haMqtt.stats();
  Serial.printf("oled page=%u/%u btn=%d  mqtt enabled=%d connected=%d announced=%d ok=%lu fail=%lu\n",
                displayUi.page() + 1, DisplayUi::pageCount(), displayUi.buttonDown() ? 1 : 0,
                mq.enabled ? 1 : 0, mq.connected ? 1 : 0, mq.announced ? 1 : 0,
                (unsigned long)mq.connects, (unsigned long)mq.failures);
}

void printConfig() {
  const DeviceConfig& cfg = configStore.get();
  Serial.printf("schema=%u role=%d name='%s'\n", cfg.schema, static_cast<int>(cfg.role),
                cfg.deviceName);
  Serial.printf("net=%04X pairKey=%08lX peer=%04X\n", cfg.netId,
                (unsigned long)cfg.pairKey, cfg.peerShortId);
  Serial.printf("meter protocol=%d polarity=%d baud=%lu interval=%lums\n",
                static_cast<int>(cfg.meterProtocol), static_cast<int>(cfg.meterInvert),
                (unsigned long)cfg.meterBaud, (unsigned long)cfg.sendIntervalMs);
  Serial.printf("meter pins rx=%u tx=%u\n", cfg.meterRxPin, cfg.meterTxPin);
  Serial.printf("radio freq=%.3f sf=%u cr=%u power=%ddBm\n", cfg.loraFreqMhz, cfg.loraSf,
                cfg.loraCr, cfg.loraPowerDbm);
  Serial.printf("wifi configured=%d ssid='%s' keepAp=%d\n", cfg.staConfigured ? 1 : 0, cfg.staSsid,
                cfg.keepApOn ? 1 : 0);
  // Printed in full: whoever has the serial console already has the device.
  // If these two ever differ, that is why a login fails.
  Serial.printf("web password='%s' ap code='%s'\n", cfg.webPassword, cfg.apPassword);
}

void printDuty(RadioLink& link) {
  const core::DutyCycleBudget& budget = link.budget();
  const uint32_t now = millis();
  Serial.printf("window=%lus budget=%lums used=%lums remaining=%lums percent=%lu%%\n",
                (unsigned long)(budget.windowMs() / 1000), (unsigned long)budget.budgetMs(),
                (unsigned long)budget.usedMs(now), (unsigned long)budget.remainingMs(now),
                (unsigned long)budget.usedPercent(now));
  Serial.printf("transmissions=%lu blocked=%lu airtime_total=%lums\n",
                (unsigned long)budget.transmissions(), (unsigned long)budget.blockedCount(),
                (unsigned long)budget.transmittedMs());
}

void runSelfTest(RadioLink& link, MeterReader& meter) {
  int pass = 0, fail = 0;
  auto check = [&](const char* name, bool ok) {
    Serial.printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    ok ? ++pass : ++fail;
  };

  Serial.println(F("selftest:"));
  check("config loaded", configStore.get().schema != 0);
  check("identity derived from MAC", strlen(configStore.macString()) == 17);
  check("setup code present", strlen(configStore.setupCode()) == 8);
  check("duty budget is 1% of an hour",
        link.budget().budgetMs() == LORA_DUTY_BUDGET_MS &&
            link.budget().windowMs() == LORA_DUTY_WINDOW_MS);
  check("radio initialised", link.stats().radioOk);
  if (!link.stats().radioOk) {
    Serial.printf("       radio error code: %d\n", link.stats().radioError);
  }
  check("airtime accounting positive", link.estimatedAirtimeMs(23) > 0);
  check("meter UART configured", meter.stats().bytesSeen >= 0);
  check("web server reachable", netIp() != "-");
  check("heap sane", ESP.getFreeHeap() > 20000);

  Serial.printf("selftest: %d passed, %d failed\n", pass, fail);
}

void handleCommand(const String& line, RadioLink& link, MeterReader& meter) {
  String cmd = line;
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd == "help" || cmd == "?") {
    printHelp();
  } else if (cmd == "status") {
    printStatus(link, meter);
  } else if (cmd == "config") {
    printConfig();
  } else if (cmd == "duty") {
    printDuty(link);
  } else if (cmd == "selftest") {
    runSelfTest(link, meter);
  } else if (cmd == "reboot") {
    Serial.println(F("rebooting"));
    delay(100);
    ESP.restart();
  } else if (cmd == "factory") {
    configStore.reset();
    Serial.println(F("factory reset - rebooting into setup"));
    delay(100);
    ESP.restart();
  } else if (cmd == "log") {
    Serial.printf("event log: %u entries (oldest first)\n", static_cast<unsigned>(eventLog.size()));
    for (size_t i = 0; i < eventLog.size(); ++i) {
      String line;
      if (eventLog.at(i, line)) Serial.printf("  %s\n", line.c_str());
    }
  } else if (cmd == "webpass" || cmd.startsWith("webpass ")) {
    DeviceConfig& cfg = configStore.mutableGet();
    String argument = cmd.length() > 8 ? cmd.substring(8) : String("");
    argument.trim();
    if (argument.length() == 0) {
      Serial.printf("web password '%s' / ap code '%s'\n", cfg.webPassword, cfg.apPassword);
    } else if (argument == "reset") {
      snprintf(cfg.webPassword, sizeof(cfg.webPassword), "%s", cfg.apPassword);
      configStore.save();
      Serial.printf("web password reset to '%s'\n", cfg.webPassword);
    } else if (argument.length() < 8) {
      Serial.println(F("at least 8 characters"));
    } else {
      snprintf(cfg.webPassword, sizeof(cfg.webPassword), "%s", argument.c_str());
      configStore.save();
      Serial.printf("web password set to '%s'\n", cfg.webPassword);
    }
  } else if (cmd == "page" || cmd.startsWith("page ")) {
    const String argument = cmd.length() > 5 ? cmd.substring(5) : String("");
    if (argument == "next" || argument.length() == 0) {
      displayUi.nextPage();
    } else {
      displayUi.setPage(static_cast<uint8_t>(argument.toInt() > 0 ? argument.toInt() - 1 : 0));
    }
    Serial.printf("oled page %u/%u (BOOT button on the board does the same)\n",
                  displayUi.page() + 1, DisplayUi::pageCount());
  } else if (cmd.startsWith("mqtt host ")) {
    String rest = cmd.substring(10);
    rest.trim();
    const int space = rest.indexOf(' ');
    const String host = space > 0 ? rest.substring(0, space) : rest;
    const int port = space > 0 ? rest.substring(space + 1).toInt() : 1883;
    if (host.length() == 0) {
      Serial.println(F("usage: mqtt host <addr> [port]"));
    } else {
      DeviceConfig& cfg = configStore.mutableGet();
      snprintf(cfg.mqttHost, sizeof(cfg.mqttHost), "%s", host.c_str());
      cfg.mqttPort = port > 0 ? static_cast<uint16_t>(port) : 1883;
      cfg.mqttEnabled = true;
      configStore.save();
      haMqtt.applyConfig(cfg);
      Serial.printf("mqtt broker set to %s:%u - 'mqtt dump' shows what it will publish\n",
                    cfg.mqttHost, cfg.mqttPort);
    }
  } else if (cmd == "mqtt off") {
    DeviceConfig& cfg = configStore.mutableGet();
    cfg.mqttEnabled = false;
    cfg.mqttHost[0] = '\0';
    configStore.save();
    haMqtt.applyConfig(cfg);
    Serial.println(F("mqtt disabled"));
  } else if (cmd == "mqtt dump") {
    String dump = haMqtt.describeDiscovery(configStore.get(), meter, link);
    Serial.println(dump);
  } else if (cmd == "mqtt announce") {
    const bool ok = haMqtt.announce(millis());
    Serial.printf("mqtt announce: %s (connected=%d)\n", ok ? "sent" : "not connected",
                  haMqtt.stats().connected ? 1 : 0);
  } else if (cmd.startsWith("apname ")) {
    String name = cmd.substring(7);
    name.trim();
    if (configStore.setApSsid(name)) {
      netApplyApSettings();
      Serial.printf("ap ssid now '%s'\n", configStore.apSsid());
    } else {
      Serial.println(F("apname rejected: 4-32 characters, letters/digits/-_ . only"));
    }
  } else if (cmd.startsWith("apcode ")) {
    String code = cmd.substring(7);
    code.trim();
    if (configStore.setSetupCode(code)) {
      netApplyApSettings();
      Serial.printf("ap code now %s\n", configStore.setupCode());
    } else {
      Serial.println(F("apcode rejected: at least 8 characters"));
    }
  } else if (cmd.startsWith("provision")) {
    // Recovery path: a board can be given its role over USB, without a phone
    // and without the setup page.
    DeviceConfig& cfg = configStore.mutableGet();
    if (cmd.indexOf("gateway") >= 0) {
      cfg.role = DeviceRole::Gateway;
    } else if (cmd.indexOf("meter") >= 0) {
      cfg.role = DeviceRole::Meter;
    } else {
      Serial.println(F("usage: provision meter|gateway [name]"));
      return;
    }
    if (cfg.deviceName[0] == '\0') {
      snprintf(cfg.deviceName, sizeof(cfg.deviceName), "%s",
               cfg.role == DeviceRole::Meter ? "Keller" : "Wohnung");
    }
    configStore.save();
    Serial.printf("role=%d saved, restarting\n", static_cast<int>(cfg.role));
    delay(200);
    ESP.restart();
  } else if (cmd == "link secret" || cmd.startsWith("link secret ")) {
    String argument = cmd.length() > 12 ? cmd.substring(12) : String("");
    argument.trim();
    if (argument.length() == 0) {
      uint8_t secret[core::kLinkSecretSize];
      if (!configStore.linkSecret(secret)) {
        Serial.println(F("no link secret - pairing agrees one ('link secret <32 hex chars>' sets one by hand)"));
      } else {
        char hex[2 * core::kLinkSecretSize + 1];
        for (size_t i = 0; i < core::kLinkSecretSize; ++i) {
          snprintf(hex + 2 * i, 3, "%02X", secret[i]);
        }
        Serial.printf("link secret %s - encryption %s\n", hex,
                      link.encryptionEnabled() ? "on" : "off");
      }
    } else if (argument == "off") {
      configStore.clearLinkSecret();
      link.updateLinkKey();
      Serial.println(F("link secret cleared - readings travel in the clear again"));
    } else if (argument.length() != 2 * core::kLinkSecretSize) {
      Serial.printf("link secret needs %u hex characters\n",
                    static_cast<unsigned>(2 * core::kLinkSecretSize));
    } else {
      uint8_t secret[core::kLinkSecretSize];
      bool valid = true;
      for (size_t i = 0; i < core::kLinkSecretSize && valid; ++i) {
        auto nibble = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          return -1;
        };
        const int high = nibble(argument[2 * i]);
        const int low = nibble(argument[2 * i + 1]);
        if (high < 0 || low < 0) {
          valid = false;
          break;
        }
        secret[i] = static_cast<uint8_t>((high << 4) | low);
      }
      if (!valid) {
        Serial.println(F("link secret rejected: hex characters only"));
      } else if (configStore.setLinkSecret(secret)) {
        link.updateLinkKey();
        Serial.printf("link secret set - encryption %s\n",
                      link.encryptionEnabled() ? "on" : "off");
      } else {
        Serial.println(F("link secret rejected"));
      }
    }
  } else if (cmd == "radio tx") {
    uint8_t payload[2] = {0xAB, 0xCD};
    const bool ok = link.send(core::PacketType::Status, payload, sizeof(payload),
                              configStore.get().peerShortId, false);
    Serial.printf("radio tx: %s\n", ok ? "sent" : "refused (duty cycle or radio)");
  } else if (cmd == "radio pair") {
    link.startPairing(millis());
    Serial.printf("pairing started, timeout %lus - 'status' shows the progress\n",
                  (unsigned long)(link.pairingTimeoutMs() / 1000));
  } else if (cmd == "radio unpair") {
    link.clearPairing();
    Serial.println(F("pairing cleared; the other end answers the next request again"));
  } else if (cmd == "meter dump") {
    String dump;
    meter.dumpTelegram(dump);
    Serial.println(dump);
  } else if (cmd.startsWith("meter sim")) {
    const bool on = cmd.endsWith("on");
    meter.setSimulation(on);
    Serial.printf("simulator %s\n", on ? "on" : "off");
  } else if (cmd.startsWith("meter loopback")) {
    const bool on = cmd.endsWith("on");
    meter.setLoopback(on);
    Serial.printf("loopback %s (jumper GPIO%d -> GPIO%d)\n", on ? "on" : "off", METER_UART_TX_PIN,
                  METER_UART_RX_PIN);
  } else if (cmd.startsWith("meter protocol")) {
    if (cmd.endsWith("sml")) {
      configStore.mutableGet().meterProtocol = MeterProtocol::Sml;
      meter.setProtocol(MeterProtocol::Sml);
    } else if (cmd.endsWith("d0")) {
      configStore.mutableGet().meterProtocol = MeterProtocol::D0;
      meter.setProtocol(MeterProtocol::D0);
    } else {
      configStore.mutableGet().meterProtocol = MeterProtocol::Auto;
      meter.setProtocol(MeterProtocol::Auto);
    }
    configStore.save();
    Serial.printf("meter protocol -> %s\n", meter.activeProtocolName());
  } else if (cmd.startsWith("meter scope")) {
    int ms = 250;
    if (sscanf(cmd.c_str(), "meter scope %d", &ms) != 1) ms = 250;
    Serial.println(meter.runScope(static_cast<uint32_t>(ms)));
  } else if (cmd.startsWith("meter pins")) {
    int rx = 0;
    int tx = 0;
    if (sscanf(cmd.c_str(), "meter pins %d %d", &rx, &tx) != 2) {
      Serial.printf("usage: meter pins <rx> <tx>   (currently rx=%d tx=%d)\n", meter.rxPin(),
                    meter.txPin());
      Serial.println(F("free pins: 15 16 38 39 40 41 42 43 44 (10 and 21 on QWIIC); "));
      Serial.println(F("avoid 0 1 2 3 5 6 7 8 11 13 14 17 18 19 20 26-37 45 46"));
    } else if (meter.setPins(static_cast<uint8_t>(rx), static_cast<uint8_t>(tx))) {
      Serial.printf("meter UART now rx=%d tx=%d\n", meter.rxPin(), meter.txPin());
    } else {
      Serial.println(F("rejected: pin is reserved (see 'help') or rx == tx"));
    }
  } else if (cmd.startsWith("meter polarity")) {
    MeterInvert polarity = MeterInvert::Auto;
    if (cmd.endsWith("inverted")) polarity = MeterInvert::Inverted;
    else if (cmd.endsWith("normal")) polarity = MeterInvert::Normal;
    configStore.mutableGet().meterInvert = polarity;
    meter.setPolarity(polarity);
    configStore.save();
    Serial.printf("meter polarity -> %d (rx inverted: %d)\n", static_cast<int>(polarity),
                  meter.stats().invertedNow ? 1 : 0);
  } else {
    Serial.printf("unknown command: %s (try 'help')\n", cmd.c_str());
  }
}

}  // namespace

void cliBegin() {
  buffer.reserve(128);
  Serial.println(F("serial shell ready - type 'help'"));
}

void cliPoll(RadioLink& link, MeterReader& meter) {
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      handleCommand(buffer, link, meter);
      buffer = "";
      continue;
    }
    if (buffer.length() < 120) buffer += static_cast<char>(c);
  }
}
