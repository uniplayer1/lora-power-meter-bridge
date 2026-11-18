#include "meter_reader.h"

#include <math.h>
#include <string.h>

#include "config.h"
#include "time_util.h"

namespace {

// OBIS codes as they appear in D0 ASCII data, with the measurement they carry.
struct D0Obis {
  const char* token;
  uint32_t obis;
  uint8_t unit;
};

const D0Obis kD0Obis[] = {
    {"1.8.0", core::kObisImportTotal, core::kUnitWh},
    {"2.8.0", core::kObisExportTotal, core::kUnitWh},
    {"1.8.1", core::kObisImportT1, core::kUnitWh},
    {"1.8.2", core::kObisImportT2, core::kUnitWh},
    {"2.8.1", core::kObisExportT1, core::kUnitWh},
    {"2.8.2", core::kObisExportT2, core::kUnitWh},
    {"1.7.0", core::kObisPowerTotal, core::kUnitW},
    {"36.7.0", core::kObisPowerL1, core::kUnitW},
    {"56.7.0", core::kObisPowerL2, core::kUnitW},
    {"76.7.0", core::kObisPowerL3, core::kUnitW},
    {"32.7.0", core::kObisVoltageL1, core::kUnitV},
    {"52.7.0", core::kObisVoltageL2, core::kUnitV},
    {"72.7.0", core::kObisVoltageL3, core::kUnitV},
    {"31.7.0", core::kObisCurrentL1, core::kUnitA},
    {"51.7.0", core::kObisCurrentL2, core::kUnitA},
    {"71.7.0", core::kObisCurrentL3, core::kUnitA},
};

uint8_t unitFromToken(const char* token, size_t len) {
  auto eq = [&](const char* name) {
    return len == strlen(name) && strncmp(token, name, len) == 0;
  };
  if (eq("Wh")) return core::kUnitWh;
  if (eq("kWh")) return core::kUnitWh;  // rescaled by the caller
  if (eq("varh")) return core::kUnitVarh;
  if (eq("kvarh")) return core::kUnitVarh;
  if (eq("W")) return core::kUnitW;
  if (eq("kW")) return core::kUnitW;
  if (eq("V")) return core::kUnitV;
  if (eq("A")) return core::kUnitA;
  if (eq("Hz")) return core::kUnitHz;
  return core::kUnitNone;
}

// "000123.456*kWh" -> raw 123456 with scaler -3 (so the value stays 123.456 kWh
// in Wh terms once the prefix is folded in).
bool parseD0Value(const char* begin, const char* end, int64_t& raw, int8_t& scaler,
                  uint8_t& unit) {
  const char* star = nullptr;
  for (const char* p = begin; p < end; ++p) {
    if (*p == '*') {
      star = p;
      break;
    }
  }
  const char* valueEnd = star ? star : end;

  // sign, integer part, fraction
  const char* p = begin;
  bool negative = false;
  if (p < valueEnd && (*p == '-' || *p == '+')) {
    negative = (*p == '-');
    ++p;
  }
  int64_t integer = 0;
  int8_t scale = 0;
  int digits = 0;
  for (; p < valueEnd && isdigit(static_cast<unsigned char>(*p)); ++p) {
    integer = integer * 10 + (*p - '0');
    ++digits;
  }
  if (digits == 0) return false;
  if (p < valueEnd && *p == '.') {
    ++p;
    for (; p < valueEnd && isdigit(static_cast<unsigned char>(*p)); ++p) {
      if (scale < 6) {
        integer = integer * 10 + (*p - '0');
        --scale;
      }
    }
  }
  if (negative) integer = -integer;

  unit = star ? unitFromToken(star + 1, static_cast<size_t>(end - star - 1)) : core::kUnitNone;
  // A kWh/kW prefix scales the value by 1000 relative to the base unit.
  if (star && end - star - 1 > 1 && (star[1] == 'k')) scale += 3;

  raw = integer;
  scaler = scale;
  return true;
}

}  // namespace

void MeterReader::begin(const DeviceConfig& config) {
  wantedProtocol_ = config.meterProtocol;
  wantedPolarity_ = config.meterInvert;
  baud_ = config.meterBaud ? config.meterBaud : METER_SML_BAUD;

  rxPin_ = config.meterRxPin;
  txPin_ = config.meterTxPin;
  if (!meterPinAllowed(rxPin_)) rxPin_ = METER_UART_RX_PIN;
  if (!meterPinAllowed(txPin_) || txPin_ == rxPin_) txPin_ = METER_UART_TX_PIN;

  openPort(baud_, SERIAL_8N1);
  portOpen_ = true;
  detectionStartedAtMs_ = millis();
  stats_.activeProtocol = wantedProtocol_;
  stats_.polarity = wantedPolarity_;
  applyPolarity();

  logEvent("meter UART up: %lu baud, rx=%d tx=%d, protocol=%s, polarity=%s",
           static_cast<unsigned long>(baud_), rxPin_, txPin_,
           activeProtocolName(),
           wantedPolarity_ == MeterInvert::Auto
               ? "auto"
               : (wantedPolarity_ == MeterInvert::Inverted ? "inverted" : "normal"));
}

void MeterReader::restorePort() {
  openPort(baud_, SERIAL_8N1);
  portOpen_ = true;
  d0Stage_ = 0;
  detectionStartedAtMs_ = millis();
  framer_.reset();
}

String MeterReader::runScope(uint32_t durationMs) {
  if (!portOpen_) return String("UART ist nicht offen");
  if (durationMs < 20) durationMs = 20;
  if (durationMs > 3000) durationMs = 3000;

  // Detach the UART and look at the pin as a plain input: whatever the head is
  // doing to the line shows up here regardless of baud rate, polarity or
  // protocol, so "the head is silent" and "we listen wrongly" become
  // distinguishable facts instead of guesses.
  serial_.end();
  delay(20);
  pinMode(rxPin_, INPUT);

  uint32_t samples = 0;
  uint32_t high = 0;
  uint32_t edges = 0;
  int last = digitalRead(rxPin_);
  const uint32_t start = micros();
  const uint32_t spanUs = durationMs * 1000UL;
  while (static_cast<uint32_t>(micros() - start) < spanUs) {
    const int level = digitalRead(rxPin_);
    if (level) ++high;
    ++samples;
    if (level != last) {
      ++edges;
      last = level;
    }
  }

  restorePort();

  const uint32_t percentHigh = samples ? (high * 100UL) / samples : 0;
  const uint32_t edgesPerSecond = (edges * 1000UL) / durationMs;

  String out;
  out.reserve(256);
  out += "GPIO";
  out += String(rxPin_);
  out += " fuer ";
  out += String(durationMs);
  out += " ms gemessen:\n";
  out += "  high: ";
  out += String(percentHigh);
  out += " %   Flanken/s: ";
  out += String(edgesPerSecond);
  out += "\n  ";
  if (edgesPerSecond > 50) {
    out += "Der Lesekopf treibt die Leitung - es kommt Signal an. Stimmen Baudrate/Polaritaet nicht, "
           "faellt das hier trotzdem auf.";
  } else if (percentHigh > 95) {
    out += "Die Leitung liegt dauerhaft HIGH - der Ruhepegel eines Lesekopfs, der gerade nichts empfaengt. "
           "Test mit einer TV-Fernbedienung: Kopf bestromt lassen, Fernbedienung frontal auf den Kopf "
           "halten, eine Taste gedrueckt halten und erneut messen - die Flanken/s muessen springen. "
           "Passiert nichts: Versorgung (3V3 laut Aufdruck), GND oder Datenleitung an GPIO";
    out += String(rxPin_);
    out += " - und ob der Kopf auf der IR-Diode des Zaehlers sitzt.";
  } else if (percentHigh < 5) {
    out += "Die Leitung liegt dauerhaft LOW: entweder invertierter Ruhepegel (Polaritaet umstellen) oder "
           "der Kopf zieht die Leitung herunter - dann fehlt ihm die Versorgung.";
  } else {
    out += "Die Leitung schwankt ohne erkennbare Daten: sieht nach offener Leitung bzw. Einstreuung aus "
           "(Kopf nicht verbunden oder nicht versorgt).";
  }

  logEvent("scope GPIO%u: high=%lu%% flanks=%lu/s", rxPin_, static_cast<unsigned long>(percentHigh),
           static_cast<unsigned long>(edgesPerSecond));
  return out;
}

void MeterReader::openPort(uint32_t baud, uint32_t serialConfig) {
  serial_.begin(baud, serialConfig, rxPin_, txPin_);
  serial_.setTimeout(5);
  serial_.setRxInvert(stats_.invertedNow);
}

bool MeterReader::setPins(uint8_t rxPin, uint8_t txPin) {
  if (!meterPinAllowed(rxPin) || !meterPinAllowed(txPin) || rxPin == txPin) return false;

  rxPin_ = rxPin;
  txPin_ = txPin;
  configStore.mutableGet().meterRxPin = rxPin;
  configStore.mutableGet().meterTxPin = txPin;
  configStore.save();

  if (portOpen_) serial_.end();
  delay(20);
  openPort(baud_, SERIAL_8N1);
  portOpen_ = true;
  d0Stage_ = 0;
  detectionStartedAtMs_ = millis();
  logEvent("meter UART moved to rx=%d tx=%d", rxPin_, txPin_);
  return true;
}

void MeterReader::applyPolarity() {
  bool invert = false;
  if (wantedPolarity_ == MeterInvert::Inverted) {
    invert = true;
  } else if (wantedPolarity_ == MeterInvert::Auto) {
    invert = polarityFlipped_;
  }
  // Read heads are sold in both polarities; flipping the RX line is cheaper
  // than asking the owner which one they bought.
  serial_.setRxInvert(invert);
  stats_.invertedNow = invert;

  // Each polarity is measured on its own. A read head idles HIGH, so while the
  // RX line is inverted that idle level is read as a permanent break and the
  // UART emits a stream of 0x00 bytes: counting those as "bytes received"
  // would make a silent line look alive in the other phase.
  stats_.bytesSeen = 0;
  rawTotal_ = 0;
}

void MeterReader::setProtocol(MeterProtocol protocol) {
  wantedProtocol_ = protocol;
  stats_.activeProtocol = protocol;
  polarityFlipped_ = false;
  detectionStartedAtMs_ = millis();
  d0Stage_ = 0;
  applyPolarity();
  logEvent("meter protocol set to %s", activeProtocolName());
}

void MeterReader::setPolarity(MeterInvert polarity) {
  wantedPolarity_ = polarity;
  stats_.polarity = polarity;
  polarityFlipped_ = false;
  applyPolarity();
  logEvent("meter polarity set to %s",
           polarity == MeterInvert::Auto ? "auto"
                                         : (polarity == MeterInvert::Inverted ? "inverted"
                                                                             : "normal"));
}

void MeterReader::setSimulation(bool on) {
  stats_.simulating = on;
  if (on && !portOpen_) {
    openPort(baud_, SERIAL_8N1);
    portOpen_ = true;
  }
  logEvent("meter simulator %s (rx=%d)", on ? "on" : "off", rxPin_);
}

void MeterReader::setLoopback(bool on) {
  stats_.loopback = on;
  if (on) setSimulation(true);
  logEvent("meter loopback %s (GPIO%d -> GPIO%d)", on ? "on" : "off", txPin_, rxPin_);
}

void MeterReader::loop(uint32_t nowMs) {
  // Real traffic first: whatever the read head delivers, always parsed.
  while (portOpen_ && serial_.available() > 0) {
    const int byte = serial_.read();
    if (byte < 0) break;
    ++stats_.bytesSeen;
    feedByte(static_cast<uint8_t>(byte));
  }

  runSimulation(nowMs);
  if (!protocolResolved()) runDetection(nowMs);
}

void MeterReader::feedByte(uint8_t byte) {
  rawTail_[rawTotal_ % kRawTail] = byte;
  ++rawTotal_;

  if (framer_.feed(byte) == core::SmlFramer::Result::Telegram) {
    onTelegramComplete();
  }
}

String MeterReader::rawTailHex() const {
  String out;
  const uint32_t shown = rawTotal_ < kRawTail ? rawTotal_ : kRawTail;
  const uint32_t first = rawTotal_ - shown;
  for (uint32_t i = first; i < rawTotal_; ++i) {
    char byte[4];
    snprintf(byte, sizeof(byte), "%02X ", rawTail_[i % kRawTail]);
    out += byte;
  }
  return out;
}

void MeterReader::onTelegramComplete() {
  lastTelegramLen_ = framer_.size() < sizeof(lastTelegram_) ? framer_.size()
                                                            : sizeof(lastTelegram_);
  memcpy(lastTelegram_, framer_.data(), lastTelegramLen_);

  core::MeterValues parsed;
  if (core::parseSml(framer_.data(), framer_.size(), parsed, &lastParseStats_)) {
    for (uint8_t i = 0; i < parsed.count; ++i) values_.update(parsed.slots[i]);
    values_.valid = true;
    values_.crcOk = parsed.crcOk;
    ++values_.telegramCount;
    values_.lastTelegramAtMs = millis();
    ++stats_.telegrams;
    stats_.lastTelegramAtMs = millis();
    if (!parsed.crcOk) ++stats_.crcErrors;
    stats_.activeProtocol = MeterProtocol::Sml;
    stats_.parseErrors += 0;
  } else {
    ++stats_.parseErrors;
    ++values_.parseErrors;
  }
}

void MeterReader::runSimulation(uint32_t nowMs) {
  if (!stats_.simulating) return;

  // The simulator is a test mode, and leaving it running is dangerous: it
  // produces plausible-looking readings, so an owner can spend an evening
  // chasing a meter that was never the source. It switches itself off.
  static uint32_t simStartedMs = 0;
  if (simStartedMs == 0) simStartedMs = nowMs;
  if (nowMs - simStartedMs > SIMULATOR_TIMEOUT_MS) {
    simStartedMs = 0;
    stats_.simulating = false;
    stats_.loopback = false;
    logEvent("simulator switched off after %lu minutes (test mode only)",
             static_cast<unsigned long>(SIMULATOR_TIMEOUT_MS / 60000));
    return;
  }

  if (nowMs - lastSimAtMs_ < 2000) return;
  lastSimAtMs_ = nowMs;

  // A plausible household: 2216.76 kWh in, 12.34 kWh out, 430 W now.
  static int64_t importRaw = 22167615;
  importRaw += 43;  // ~43 * 1e-4 kWh every 2 s
  const core::SmlBuildEntry entries[] = {
      {core::kObisImportTotal, importRaw, -4, core::kUnitWh},
      {core::kObisExportTotal, 1234, -2, core::kUnitWh},
      {core::kObisPowerTotal, 43000, -2, core::kUnitW},
  };
  uint8_t telegram[256];
  const size_t len = core::buildSmlTelegram(entries, 3, telegram, sizeof(telegram));
  if (len == 0) return;

  if (stats_.loopback) {
    // Out of TX, back in through the jumper on RX: exercises UART, polarity
    // handling and framing for real.
    serial_.write(telegram, len);
    serial_.flush();
    return;
  }
  for (size_t i = 0; i < len; ++i) feedByte(telegram[i]);
}

void MeterReader::runDetection(uint32_t nowMs) {
  if (wantedProtocol_ == MeterProtocol::Sml) {
    // Explicitly passive: nothing to do but keep listening.
    if (stats_.activeProtocol != MeterProtocol::Sml) stats_.activeProtocol = MeterProtocol::Sml;
    return;
  }
  if (wantedProtocol_ == MeterProtocol::D0) {
    if (d0Stage_ == 0 && nowMs - lastD0AtMs_ > 15000) startD0Session(nowMs);
    if (d0Stage_ != 0) runD0SessionSection();
    return;
  }

  // A D0 session in progress owns the UART until it ends.
  if (d0Stage_ != 0) {
    runD0SessionSection();
    return;
  }

  // Auto mode cycles, rather than making one guess and settling on it:
  //   listen in the configured polarity -> listen inverted -> active D0 wake-up
  //   -> back to listening.
  // A meter that pushes SML needs no interaction at all, so listening comes
  // first; but a read head that only starts delivering once it is positioned on
  // the meter must still be picked up minutes later, without a reboot.
  const uint32_t listening = nowMs - detectionStartedAtMs_;
  if (listening < METER_LISTEN_MS) return;

  if (!polarityFlipped_) {
    polarityFlipped_ = true;
    ++stats_.inversionsTried;
    applyPolarity();
    detectionStartedAtMs_ = nowMs;
    logEvent("meter: %lu bytes in %lus, trying the inverted polarity",
             static_cast<unsigned long>(stats_.bytesSeen),
             static_cast<unsigned long>(METER_LISTEN_MS / 1000));
    return;
  }

  if (d0Rounds_ < METER_D0_ROUNDS) {
    ++d0Rounds_;
    startD0Session(nowMs);
    return;
  }

  d0Rounds_ = 0;
  polarityFlipped_ = false;
  applyPolarity();
  detectionStartedAtMs_ = nowMs;
  logEvent("meter: nothing on either polarity and no D0 answer - back to listening (%lu bytes)",
           static_cast<unsigned long>(stats_.bytesSeen));
}

void MeterReader::startD0Session(uint32_t nowMs) {
  detectionStartedAtMs_ = nowMs;
  lastD0AtMs_ = nowMs;
  d0Stage_ = 1;
  d0Length_ = 0;
  d0Buffer_[0] = '\0';
  ++stats_.d0Sessions;

  // IEC 62056-21 mode C: wake-up at 300 baud 7E1 with a break, then "/?!\r\n".
  serial_.end();
  delay(20);
  openPort(METER_D0_WAKE_BAUD, SERIAL_7E1);
  const uint8_t zero = 0x00;
  for (int i = 0; i < 30; ++i) serial_.write(zero);  // break
  serial_.write(reinterpret_cast<const uint8_t*>("/?!\r\n"), 5);
  serial_.flush();
  logEvent("meter: D0 wake-up sent at %d baud", METER_D0_WAKE_BAUD);
}

void MeterReader::abortD0Session() {
  if (d0Stage_ == 0) return;
  d0Stage_ = 0;
  d0Length_ = 0;
  serial_.end();
  delay(20);
  openPort(baud_, SERIAL_8N1);
}

bool MeterReader::runD0SessionSection() {
  const uint32_t now = millis();

  // Stage 1: read the identification line, switch baud, ask for the data block.
  if (d0Stage_ == 1) {
    while (serial_.available() > 0 && d0Length_ < sizeof(d0Buffer_) - 1) {
      const int byte = serial_.read();
      if (byte < 0) break;
      ++stats_.bytesSeen;
      d0Buffer_[d0Length_++] = static_cast<char>(byte);
      d0Buffer_[d0Length_] = '\0';
    }
    if (d0Length_ >= 5 && strstr(d0Buffer_, "\r\n") != nullptr) {
      logEvent("meter: D0 identification '%s'", d0Buffer_);
      // Switch to the data baud and request the readout (mode C, 9600, block 1).
      serial_.end();
      delay(20);
      openPort(METER_D0_DATA_BAUD, SERIAL_7E1);
      delay(50);
      serial_.write(0x06);  // ACK
      serial_.write(reinterpret_cast<const uint8_t*>("051\r\n"), 5);
      serial_.flush();
      d0Length_ = 0;
      d0Buffer_[0] = '\0';
      d0Stage_ = 2;
      d0StartedAtMs_ = now;
      return true;
    }
    if (now - d0StartedAtMs_ > 6000) {
      ++stats_.d0Failures;
      logEvent("meter: no D0 identification - wrong protocol or polarity");
      abortD0Session();
    }
    return false;
  }

  // Stage 2: collect the data block, which ends with ETX (0x03).
  if (d0Stage_ == 2) {
    bool done = false;
    while (serial_.available() > 0 && d0Length_ < sizeof(d0Buffer_) - 1) {
      const int byte = serial_.read();
      if (byte < 0) break;
      ++stats_.bytesSeen;
      if (byte == 0x02) continue;  // STX
      if (byte == 0x03) {          // ETX
        done = true;
        break;
      }
      d0Buffer_[d0Length_++] = static_cast<char>(byte);
      d0Buffer_[d0Length_] = '\0';
    }

    if (done) {
      serial_.write(0x06);  // acknowledge the block, then close the session
      serial_.flush();

      // Parse "OBIS(value*unit)" tuples.
      uint8_t found = 0;
      for (const D0Obis& candidate : kD0Obis) {
        const char* at = strstr(d0Buffer_, candidate.token);
        if (!at) continue;
        const char* open = strchr(at, '(');
        if (!open) continue;
        const char* close = strchr(open, ')');
        if (!close) continue;
        int64_t raw = 0;
        int8_t scaler = 0;
        uint8_t unit = core::kUnitNone;
        if (!parseD0Value(open + 1, close, raw, scaler, unit)) continue;
        core::Reading reading;
        reading.obis = candidate.obis;
        reading.raw = raw;
        reading.scaler = scaler;
        reading.unit = unit != core::kUnitNone ? unit : candidate.unit;
        values_.update(reading);
        ++found;
      }

      if (found > 0) {
        values_.valid = true;
        ++values_.telegramCount;
        values_.lastTelegramAtMs = now;
        ++stats_.telegrams;
        stats_.lastTelegramAtMs = now;
        stats_.activeProtocol = MeterProtocol::D0;
        lastTelegramLen_ = d0Length_ < sizeof(lastTelegram_) ? d0Length_ : sizeof(lastTelegram_);
        memcpy(lastTelegram_, d0Buffer_, lastTelegramLen_);
        logEvent("meter: D0 readout with %u values", found);
      } else {
        ++stats_.d0Failures;
        logEvent("meter: D0 data block had no known OBIS value");
      }
      abortD0Session();
      return true;
    }

    if (now - d0StartedAtMs_ > 8000) {
      ++stats_.d0Failures;
      logEvent("meter: D0 data block timed out");
      abortD0Session();
    }
    return false;
  }

  return false;
}

bool MeterReader::fresh(uint32_t nowMs, uint32_t maxAgeMs) const {
  if (!values_.valid) return false;
  if (!stats_.lastTelegramAtMs) return false;
  // Stamps are taken with millis(); `nowMs` was captured earlier in the same
  // loop iteration, before the WiFi and meter work. Subtracting the two can go
  // negative, and unsigned that reads as an ancient telegram - the node would
  // then never report its meter as fresh and never send a reading at all.
  (void)nowMs;
  return !olderThan(millis(), stats_.lastTelegramAtMs, maxAgeMs);
}

void MeterReader::injectTelemetry(const core::MeterTelemetry& telemetry) {
  const uint32_t now = millis();

  // The unit handling sits in the core library, next to the code that packs the
  // frame, so the two can be checked against each other on the host - that pair
  // is where 0.1 Wh and W were mixed up and a 800 W load arrived as 8000 W.
  core::applyTelemetry(telemetry, values_);

  values_.lastTelegramAtMs = now;
  ++values_.telegramCount;
  ++stats_.telegrams;
  stats_.lastTelegramAtMs = now;
  stats_.remoteFeed = true;
}

const char* MeterReader::activeProtocolName() const {
  if (stats_.remoteFeed) return "lora";
  switch (stats_.activeProtocol) {
    case MeterProtocol::Sml:
      return "sml";
    case MeterProtocol::D0:
      return "d0";
    default:
      return "detecting";
  }
}

void MeterReader::dumpTelegram(String& out) const {
  // Raw view first: with a head that is wired but silent, this is what tells
  // noise apart from the start of a telegram.
  out = "raw bytes total=";
  out += String(rawTotal_);
  out += "  last bytes: ";
  out += rawTotal_ ? rawTailHex() : String("(none)");
  out += "\n";

  out += "len=";
  out += String(lastTelegramLen_);
  out += "\n";
  for (size_t i = 0; i < lastTelegramLen_; ++i) {
    char byte[4];
    snprintf(byte, sizeof(byte), "%02X ", lastTelegram_[i]);
    out += byte;
    if ((i + 1) % 24 == 0) out += "\n";
  }
  out += "\n";
  out += "entries=";
  out += String(lastParseStats_.entries);
  out += " unknown=";
  out += String(lastParseStats_.unknownEntries);
  out += " crc=";
  out += lastParseStats_.crcActual == lastParseStats_.crcExpected ? "ok" : "mismatch";
  out += " lengthMode=";
  out += lastParseStats_.lengthIncludesTagByte ? "includes-tag" : "excludes-tag";
  out += "\n";
  if (lastParseStats_.last.hasValue) {
    char line[128];
    snprintf(line, sizeof(line), "last-obis=%02X%02X%02X raw=%lld scaler=%d unit=%u\n",
             lastParseStats_.last.obis[2], lastParseStats_.last.obis[3],
             lastParseStats_.last.obis[4], static_cast<long long>(lastParseStats_.last.raw),
             lastParseStats_.last.scaler, lastParseStats_.last.unit);
    out += line;
  }
}
