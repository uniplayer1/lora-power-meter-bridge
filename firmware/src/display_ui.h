#pragma once
// OLED status display.
//
// The screen is the only interface that works without a phone, so it carries
// the essentials per role: the basement node shows the LoRa link, the reading
// and - on its own page - the AP name and setup code a visitor needs to get in.

#include <Arduino.h>
#include <U8g2lib.h>

#include "app_config.h"
#include "meter_reader.h"
#include "radio_link.h"

class DisplayUi {
 public:
  void begin();
  void attach(RadioLink* link, MeterReader* meter);
  void loop(uint32_t nowMs);
  void forceRedraw() { dirty_ = true; }

  // Exposed so the owner can page the display from the web UI or the serial
  // shell, and so the raw button level can be reported - a page counter that
  // does not move is either a button problem or a firmware problem, and this
  // tells the two apart.
  uint8_t page() const { return page_; }
  static constexpr uint8_t pageCount() { return kPageCount; }
  bool buttonDown() const { return buttonNow_; }
  void nextPage();
  void setPage(uint8_t page);

 private:
  // Pages are cycled with the BOOT button; the header shows which one is up.
  static constexpr uint8_t kPageCount = 4;

  void draw();
  void drawHeader(const char* title);
  void drawUnprovisioned();
  void drawAccessPage(const char* title, bool accessPoint);
  void drawLinkPage(const char* title);

  // LilyGO T3-S3 panel: the SSD1306 alignment is the one that fits this OLED.
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C display_{U8G2_R0, U8X8_PIN_NONE};

  RadioLink* link_ = nullptr;
  MeterReader* meter_ = nullptr;

  uint8_t page_ = 0;
  uint32_t lastDrawMs_ = 0;
  uint32_t lastButtonSampleMs_ = 0;
  uint8_t lowSamples_ = 0;
  bool buttonNow_ = false;
  bool pressArmed_ = false;
  bool dirty_ = true;
};

// Single instance, defined in main.cpp: the web UI and the serial shell both
// need to see and drive it.
extern DisplayUi displayUi;
