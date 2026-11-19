#pragma once
// Meter acquisition: IR read head on UART1 (RX/TX from METER_UART_RX_PIN and
// METER_UART_TX_PIN, movable at runtime and found automatically if wired
// differently - see MeterReader::runDetection()).
//
// The meter in the field is a German one with an optical interface, and which
// of the two protocols it speaks is unknown - modern meters push SML
// unrequested every 1-2 s, older ones answer an IEC 62056-21 (D0) wake-up in
// mode C. So the reader detects instead of assuming, and the owner can pin the
// protocol and the signal polarity in the web UI when detection gets it wrong.
//
// Two test modes make the whole path testable without the meter:
//   sim       - synthesised SML telegrams fed straight into the framer
//   loopback  - the same telegrams written to TX, with TX jumpered to RX, so
//               the real UART, polarity handling and framing run

#include <Arduino.h>
#include <HardwareSerial.h>

#include "app_config.h"
#include "sml.h"

struct MeterStats {
  uint32_t telegrams = 0;
  uint32_t crcErrors = 0;
  uint32_t parseErrors = 0;
  // Telegrams that did not fit the framer buffer. Zero on a healthy install;
  // non-zero means the meter sends longer telegrams than the buffer allows.
  uint32_t overflowErrors = 0;
  uint32_t lastTelegramAtMs = 0;
  uint32_t bytesSeen = 0;
  uint32_t d0Sessions = 0;
  uint32_t d0Failures = 0;
  uint32_t inversionsTried = 0;
  MeterProtocol activeProtocol = MeterProtocol::Auto;
  MeterInvert polarity = MeterInvert::Auto;
  bool invertedNow = false;
  bool simulating = false;
  bool loopback = false;
  // Gateway only: values arrived over the LoRa link, not from a local read head.
  bool remoteFeed = false;
};

class MeterReader {
 public:
  void begin(const DeviceConfig& config);
  void loop(uint32_t nowMs);

  // Moves the UART to different pins at runtime (the ESP32-S3 can route a UART
  // almost anywhere). Returns false when a pin is reserved - see
  // meterPinAllowed().
  bool setPins(uint8_t rxPin, uint8_t txPin);

  // Measures the RX pin as a plain input for a moment and reports what the read
  // head is doing to the line: idle high, idle low, or actually switching. This
  // works whatever the baud rate, polarity or protocol is - the UART is detached
  // for the measurement - so it separates "the head says nothing" from "we are
  // listening wrongly".
  String runScope(uint32_t durationMs = 250);
  uint8_t rxPin() const { return rxPin_; }
  uint8_t txPin() const { return txPin_; }

  const core::MeterValues& values() const { return values_; }
  const MeterStats& stats() const { return stats_; }

  bool fresh(uint32_t nowMs, uint32_t maxAgeMs = METER_STALE_MS) const;
  bool protocolResolved() const { return stats_.activeProtocol != MeterProtocol::Auto; }
  const char* activeProtocolName() const;

  // Gateway side: fills the meter model from a telemetry payload that arrived
  // over the link, so the display and the REST API show what really came in.
  void injectTelemetry(const core::MeterTelemetry& telemetry);

  void setProtocol(MeterProtocol protocol);
  void setPolarity(MeterInvert polarity);
  void setSimulation(bool on);
  void setLoopback(bool on);

  // Raw telegram + parser view, for `meter dump` and the web UI's diagnostics.
  void dumpTelegram(String& out) const;

 private:
  void applyPolarity();
  void feedByte(uint8_t byte);
  String rawTailHex() const;
  void onTelegramComplete();
  void runDetection(uint32_t nowMs);
  void runSimulation(uint32_t nowMs);
  bool runD0SessionSection();
  void startD0Session(uint32_t nowMs);
  void abortD0Session();

  void openPort(uint32_t baud, uint32_t serialConfig);
  void restorePort();

  // Walks kHuntPins one step and listens there. Returns false once every
  // candidate has been tried. Only runs while no telegram has ever been parsed.
  bool huntNextPin(uint32_t nowMs);

  HardwareSerial serial_{METER_UART_NUM};
  uint8_t rxPin_ = METER_UART_RX_PIN;
  uint8_t txPin_ = METER_UART_TX_PIN;
  core::SmlFramer framer_;
  core::MeterValues values_;
  core::SmlParseStats lastParseStats_;
  MeterStats stats_;

  MeterProtocol wantedProtocol_ = MeterProtocol::Auto;
  MeterInvert wantedPolarity_ = MeterInvert::Auto;
  uint32_t baud_ = METER_SML_BAUD;

  uint32_t detectionStartedAtMs_ = 0;
  uint8_t d0Rounds_ = 0;
  uint32_t lastSimAtMs_ = 0;
  uint32_t lastD0AtMs_ = 0;
  bool polarityFlipped_ = false;
  bool portOpen_ = false;
  bool hunting_ = false;
  uint8_t huntIndex_ = 0;

  // D0 (IEC 62056-21 mode C) session state
  uint32_t d0StartedAtMs_ = 0;
  uint8_t d0Stage_ = 0;
  char d0Buffer_[512] = {0};
  size_t d0Length_ = 0;

  uint8_t lastTelegram_[core::SmlFramer::kMaxTelegram] = {0};
  size_t lastTelegramLen_ = 0;

  // The last bytes that arrived, whatever they were. When the framer has not
  // completed a telegram, this is the only way to tell line noise from the
  // beginning of a real telegram (which would start with 1B 1B 1B 1B).
  static constexpr size_t kRawTail = 64;
  uint8_t rawTail_[kRawTail] = {0};
  uint32_t rawTotal_ = 0;
};
