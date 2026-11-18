#include "display_ui.h"

#include <Wire.h>

#include "config.h"
#include "firmware_version.h"
#include "net.h"
#include "time_util.h"

void DisplayUi::begin() {
  Wire.begin(I2C_SDA, I2C_SCL);
  display_.begin();
  display_.clearBuffer();
  display_.setFont(u8g2_font_6x12_tf);
  display_.drawStr(0, 12, "Power Meter");
  display_.drawStr(0, 28, FW_ID);
  display_.sendBuffer();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
}

void DisplayUi::attach(RadioLink* link, MeterReader* meter) {
  link_ = link;
  meter_ = meter;
  dirty_ = true;
}

void DisplayUi::nextPage() {
  page_ = static_cast<uint8_t>((page_ + 1) % kPageCount);
  dirty_ = true;
  logEvent("display page %u/%u", page_ + 1, kPageCount);
}

void DisplayUi::setPage(uint8_t page) {
  page_ = static_cast<uint8_t>(page % kPageCount);
  dirty_ = true;
  logEvent("display page %u/%u", page_ + 1, kPageCount);
}

void DisplayUi::loop(uint32_t nowMs) {
  // BOOT button cycles the pages. Sampling is time based and needs two
  // consecutive low readings before it counts as a press, then re-arms only
  // once the button is released - a single edge detect at what is effectively
  // loop speed is far too easy to miss with a hand-pressed button.
  if (nowMs - lastButtonSampleMs_ >= 20) {
    lastButtonSampleMs_ = nowMs;
    buttonNow_ = digitalRead(BUTTON_PIN) == LOW;
    if (buttonNow_) {
      if (lowSamples_ < 255) ++lowSamples_;
    } else {
      lowSamples_ = 0;
      pressArmed_ = false;
    }
    if (!pressArmed_ && lowSamples_ >= 2) {
      pressArmed_ = true;
      nextPage();
    }
  }

  if (!dirty_ && nowMs - lastDrawMs_ < 1000) return;
  lastDrawMs_ = nowMs;
  dirty_ = false;
  draw();
}

void DisplayUi::drawHeader(const char* title) {
  const DeviceConfig& cfg = configStore.get();
  display_.setFont(u8g2_font_6x12_tf);
  display_.drawStr(0, 10, title);
  display_.setFont(u8g2_font_5x7_tf);
  char right[24];
  // The counter is the page indicator. For the first half minute the button that
  // pages is named too - as "BOOT-Taste", not "BOOT": a bare "BOOT" in the corner
  // of the screen reads as a boot state, and it was mistaken for one on a board
  // that was running perfectly well.
  if (millis() < 30000) {
    snprintf(right, sizeof(right), "%u/%u BOOT-Taste", page_ + 1, kPageCount);
  } else {
    snprintf(right, sizeof(right), "%u/%u", page_ + 1, kPageCount);
  }
  display_.drawStr(128 - 7 * strlen(right), 10, right);
  display_.drawHLine(0, 13, 128);
}

void DisplayUi::drawLinkPage(const char* title) {
  const LinkStats& link = link_->stats();
  const uint32_t now = millis();
  char line[32];

  drawHeader(title);
  display_.setFont(u8g2_font_6x12_tf);
  if (link.pairState == PairState::Running) {
    display_.drawStr(0, 28, "Koppeln laeuft");
  } else if (link.pairState == PairState::TimedOut) {
    display_.drawStr(0, 28, "Koppeln: Fehler");
  } else {
    display_.drawStr(0, 28, link.peerOnline ? "Gegenstelle: online" : "Gegenstelle: offline");
  }
  display_.setFont(u8g2_font_5x7_tf);
  snprintf(line, sizeof(line), "%ddBm / %.1f dB", link.peerRssi, link.peerSnr);
  display_.drawStr(0, 42, line);
  display_.setFont(u8g2_font_4x6_tf);
  snprintf(line, sizeof(line), "Zeitbudget %lu%% von %lus  tx%lu rx%lu",
           (unsigned long)link_->budget().usedPercent(now),
           (unsigned long)(link_->budget().budgetMs() / 1000), (unsigned long)link.txPackets,
           (unsigned long)link.rxPackets);
  display_.drawStr(0, 54, line);
  snprintf(line, sizeof(line), "net %04X  peer %04X", configStore.get().netId,
           configStore.get().peerShortId);
  display_.drawStr(0, 64, line);
}

void DisplayUi::drawAccessPage(const char* title, bool accessPoint) {
  // The setup code is the WLAN password *and* the first web login password, so
  // it belongs on screen for both roles - the flat's board needs it just as
  // much as the basement one, which is what the phone asks for when the page
  // asks to log in.
  // Line spacing matters here: the 12-pixel font used for the code needs 12 px
  // of clearance above its baseline, otherwise it draws straight through the
  // label above it and the code ends up looking like a blank line.
  // Two different things share this page, and conflating them is what made the
  // display print a password that no longer logs anybody in:
  //   * the WLAN code - the access point's password, which has to be readable
  //     or nobody can join the network the page is served from;
  //   * the dashboard password - the owner's, and once it is set individually
  //     the board stops being allowed to publish it.
  const bool customDashboardPassword =
      strcmp(configStore.get().webPassword, configStore.setupCode()) != 0;

  drawHeader(title);
  display_.setFont(u8g2_font_5x7_tf);

  if (customDashboardPassword) {
    display_.drawStr(0, 22, "Dashboard-Passwort:");
    display_.drawStr(0, 32, "geaendert (eigenes)");
    display_.drawStr(0, 44, "WLAN-Code:");
    display_.setFont(u8g2_font_6x12_tf);
    display_.drawStr(0, 58, configStore.setupCode());
  } else {
    display_.drawStr(0, 22, accessPoint ? "WLAN + Web-Code:" : "Web-Login Code:");
    display_.setFont(u8g2_font_6x12_tf);
    display_.drawStr(0, 38, configStore.setupCode());
    display_.setFont(u8g2_font_4x6_tf);
    display_.drawStr(0, 50, "auch WLAN-Passwort");
    // The link secret sits here, where the owner stands and copies it into the
    // other device. It replaces the address line, which is on every other page
    // anyway - the key is the one value that exists nowhere else.
    uint8_t secret[core::kLinkSecretSize];
    if (configStore.linkSecret(secret)) {
      char hex[2 * core::kLinkSecretSize + 1];
      for (size_t i = 0; i < core::kLinkSecretSize; ++i) {
        snprintf(hex + 2 * i, 3, "%02X", secret[i]);
      }
      char text[24];
      snprintf(text, sizeof(text), "Funk-Schluessel:");
      display_.drawStr(0, 56, text);
      display_.drawStr(0, 62, hex);
    } else {
      display_.drawStr(0, 58, accessPoint ? "http://192.168.4.1" : netIp().c_str());
    }
  }

  // The firmware stamp on screen: "is the new build really on this board?" was
  // asked often enough that the answer belongs where the board can show it.
  display_.setFont(u8g2_font_4x6_tf);
  display_.drawStr(0, 64, FW_ID);
}

void DisplayUi::drawUnprovisioned() {
  // The setup code is the WPA2 password of the access point. Without it on
  // screen the person standing in front of the board cannot join it, so it is
  // the one thing that must never be missing here.
  display_.setFont(u8g2_font_6x12_tf);
  display_.drawStr(0, 10, "Einrichten");
  display_.setFont(u8g2_font_5x7_tf);
  display_.drawStr(0, 22, "WLAN:");
  display_.drawStr(0, 32, configStore.apSsid());
  display_.drawStr(0, 44, "Code:");
  display_.setFont(u8g2_font_6x12_tf);
  display_.drawStr(0, 56, configStore.setupCode());
  display_.setFont(u8g2_font_5x7_tf);
  display_.drawStr(0, 64, "dann http://192.168.4.1");
}

void DisplayUi::draw() {
  const DeviceConfig& cfg = configStore.get();
  display_.clearBuffer();

  if (!configStore.provisioned()) {
    drawUnprovisioned();
    display_.sendBuffer();
    return;
  }

  const LinkStats& link = link_->stats();
  const MeterStats& meter = meter_->stats();
  const core::MeterValues& values = meter_->values();

  char line[32];
  const uint32_t now = millis();

  if (cfg.role == DeviceRole::Meter) {
    switch (page_ % kPageCount) {
      case 0:
        drawHeader("Keller");
        display_.setFont(u8g2_font_6x12_tf);
        display_.drawStr(0, 28, link.peerOnline ? "Link: OK" : "Link: getrennt");
        display_.setFont(u8g2_font_5x7_tf);
        snprintf(line, sizeof(line), "%ddBm  SNR %.1f", link.peerRssi, link.peerSnr);
        display_.drawStr(0, 42, line);
        display_.setFont(u8g2_font_6x12_tf);
        if (values.has(core::kObisPowerTotal)) {
          snprintf(line, sizeof(line), "%.1f W", values.find(core::kObisPowerTotal)->watts());
        } else {
          snprintf(line, sizeof(line), "keine Messung");
        }
        display_.drawStr(0, 60, line);
        break;

      case 1:
        drawHeader("Zaehler");
        display_.setFont(u8g2_font_4x6_tf);
        snprintf(line, sizeof(line), "Bezug       %.1f kWh",
                 values.energyWh(core::kObisImportTotal) / 1000.0);
        display_.drawStr(0, 26, line);
        snprintf(line, sizeof(line), "Einspeisung %.1f kWh",
                 values.energyWh(core::kObisExportTotal) / 1000.0);
        display_.drawStr(0, 38, line);
        snprintf(line, sizeof(line), "Leistung    %.1f W", values.powerW(core::kObisPowerTotal));
        display_.drawStr(0, 50, line);
        display_.setFont(u8g2_font_5x7_tf);
        snprintf(line, sizeof(line), "%s  %lus", meter_->activeProtocolName(),
                 (unsigned long)ageSeconds(now, meter.lastTelegramAtMs));
        display_.drawStr(0, 64, line);
        break;

      case 2:
        drawLinkPage("Funkstrecke");
        break;

      default:
        drawAccessPage("Zugang", true);
        break;
    }
  } else {
    switch (page_ % kPageCount) {
      case 0:
        drawHeader("Wohnung");
        display_.setFont(u8g2_font_6x12_tf);
        display_.drawStr(0, 28, netStateName());
        display_.setFont(u8g2_font_5x7_tf);
        snprintf(line, sizeof(line), "IP %s", netIp().c_str());
        display_.drawStr(0, 42, line);
        display_.setFont(u8g2_font_4x6_tf);
        // Its own access point is the address that works no matter what the
        // configured network is doing - it belongs on the screen, not only in
        // the web UI the owner cannot reach when the join is down.
        if (netApUp()) {
          snprintf(line, sizeof(line), "AP %s", netApIp().c_str());
        } else {
          snprintf(line, sizeof(line), "%s.local", netHostName().c_str());
        }
        display_.drawStr(0, 54, line);
        display_.drawStr(0, 64, "Home Assistant: REST /api/v1/state");
        break;

      case 1:
        drawHeader("Messwerte");
        display_.setFont(u8g2_font_6x12_tf);
        if (values.has(core::kObisPowerTotal)) {
          snprintf(line, sizeof(line), "%.1f W", values.find(core::kObisPowerTotal)->watts());
          display_.drawStr(0, 28, line);
        } else {
          display_.drawStr(0, 28, "keine Messung");
        }
        display_.setFont(u8g2_font_5x7_tf);
        snprintf(line, sizeof(line), "Alter: %lus",
                 (unsigned long)ageSeconds(now, meter.lastTelegramAtMs));
        display_.drawStr(0, 44, line);
        display_.setFont(u8g2_font_4x6_tf);
        snprintf(line, sizeof(line), "Bezug %.1f kWh  Einspeisung %.1f kWh",
                 values.energyWh(core::kObisImportTotal) / 1000.0,
                 values.energyWh(core::kObisExportTotal) / 1000.0);
        display_.drawStr(0, 58, line);
        break;

      case 2:
        drawLinkPage("Funkstrecke");
        break;

      default:
        drawAccessPage("Zugang", false);
        break;
    }
  }

  display_.sendBuffer();
}
