#include "../unity_case.h"
// SML parser tests. The parser is the highest-risk piece of the firmware: it
// has to decode whatever the meter in the friend's basement emits, and a wrong
// reading is worse than no reading. Everything here runs on the host, so the
// grammar is pinned down before the IR read head is even wired.

#include <unity.h>

#include <cmath>
#include <cstring>

#include "crc16.h"
#include "meter_values.h"
#include "sml.h"

#include "../fixtures/sml_easymeter_q3.h"

using namespace core;

namespace {

const uint8_t kStartMarker[8] = {0x1B, 0x1B, 0x1B, 0x1B, 0x01, 0x01, 0x01, 0x01};
const uint8_t kEndMarker[5] = {0x1B, 0x1B, 0x1B, 0x1B, 0x1A};

// Byte-exact 1.8.0 entry from an EasyMeter Q3B read over the optical
// interface: 22167615689 * 10^-4 Wh = 2216.7615689 kWh.
const uint8_t kRealEasymeterEntry[] = {
    0x77, 0x07, 0x01, 0x00, 0x01, 0x08, 0x00, 0xff, 0x64, 0x00,
    0x00, 0x80, 0x01, 0x62, 0x1e, 0x52, 0xfc, 0x59, 0x00, 0x00,
    0x00, 0x05, 0x29, 0x4a, 0xf8, 0xc9, 0x01};

// Wraps raw entry bytes into a valid telegram (markers + CRC).
size_t wrapTelegram(const uint8_t* entry, size_t entryLen, uint8_t* out, size_t cap) {
  const size_t body = sizeof(kStartMarker) + entryLen + sizeof(kEndMarker) + 2;
  if (cap < body) return 0;
  size_t i = 0;
  memcpy(out + i, kStartMarker, sizeof(kStartMarker));
  i += sizeof(kStartMarker);
  memcpy(out + i, entry, entryLen);
  i += entryLen;
  memcpy(out + i, kEndMarker, sizeof(kEndMarker));
  i += sizeof(kEndMarker);
  const uint16_t crc = crc16X25Finish(crc16X25(out, i));
  out[i++] = static_cast<uint8_t>(crc & 0xFF);
  out[i++] = static_cast<uint8_t>(crc >> 8);
  return i;
}

}  // namespace

void setUp() {}
void tearDown() {}

TEST_CASE("parses a real EasyMeter 1.8.0 entry byte for byte") {
  uint8_t telegram[64];
  const size_t len = wrapTelegram(kRealEasymeterEntry, sizeof(kRealEasymeterEntry), telegram,
                                 sizeof(telegram));
  TEST_ASSERT_GREATER_THAN(0, len);

  MeterValues values;
  SmlParseStats stats;
  TEST_ASSERT_TRUE(parseSml(telegram, len, values, &stats));
  TEST_ASSERT_TRUE(values.crcOk);
  TEST_ASSERT_EQUAL_UINT8(1, stats.entries);
  TEST_ASSERT_TRUE(stats.lengthIncludesTagByte);

  const Reading* import = values.find(kObisImportTotal);
  TEST_ASSERT_NOT_NULL(import);
  TEST_ASSERT_EQUAL_INT64(22167615689LL, import->raw);
  TEST_ASSERT_EQUAL_INT8(-4, import->scaler);
  TEST_ASSERT_EQUAL_UINT8(kUnitWh, import->unit);
  TEST_ASSERT_DOUBLE_WITHIN(0.001, 2216761.5689, import->wh());
}

TEST_CASE("builder and parser agree on a full telegram") {
  const SmlBuildEntry entries[] = {
      {kObisImportTotal, 123456789LL, -4, kUnitWh},
      {kObisExportTotal, 98765432LL, -4, kUnitWh},
      {kObisPowerTotal, 1234LL, -2, kUnitW},
      {kObisPowerL1, -250LL, -2, kUnitW},
  };

  uint8_t telegram[256];
  const size_t len = buildSmlTelegram(entries, 4, telegram, sizeof(telegram));
  TEST_ASSERT_GREATER_THAN(0, len);

  MeterValues values;
  SmlParseStats stats;
  TEST_ASSERT_TRUE(parseSml(telegram, len, values, &stats));
  TEST_ASSERT_TRUE(values.crcOk);
  TEST_ASSERT_EQUAL_UINT8(4, stats.entries);

  TEST_ASSERT_DOUBLE_WITHIN(0.01, 12345.6789, values.energyWh(kObisImportTotal));
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 9876.5432, values.energyWh(kObisExportTotal));
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 12.34, values.powerW(kObisPowerTotal));
  TEST_ASSERT_DOUBLE_WITHIN(0.01, -2.50, values.powerW(kObisPowerL1));
}

TEST_CASE("a bad checksum keeps the values but flags them") {
  const SmlBuildEntry entry[] = {{kObisImportTotal, 4200LL, -1, kUnitWh}};
  uint8_t telegram[128];
  size_t len = buildSmlTelegram(entry, 1, telegram, sizeof(telegram));
  telegram[len - 1] ^= 0xFF;  // corrupt the CRC

  MeterValues values;
  TEST_ASSERT_TRUE(parseSml(telegram, len, values));
  TEST_ASSERT_FALSE(values.crcOk);
  TEST_ASSERT_EQUAL_UINT32(1, values.crcErrors);
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 420.0, values.energyWh(kObisImportTotal));
}

TEST_CASE("simulated meter values survive the telemetry wire format") {
  MeterValues values;
  values.update(Reading{kObisImportTotal, 22167615689LL, -4, kUnitWh});
  values.update(Reading{kObisExportTotal, 1000LL, -3, kUnitWh});
  values.update(Reading{kObisPowerTotal, 1234LL, -2, kUnitW});
  values.update(Reading{kObisPowerL1, -250LL, -2, kUnitW});

  const MeterTelemetry telemetry = toTelemetry(values);
  TEST_ASSERT_EQUAL_UINT8(MeterTelemetry::kImport | MeterTelemetry::kExport |
                              MeterTelemetry::kPower | MeterTelemetry::kPhases,
                          telemetry.flags);

  uint8_t wire[MeterTelemetry::kWireSize];
  TEST_ASSERT_EQUAL_UINT32(MeterTelemetry::kWireSize,
                           packTelemetry(telemetry, wire, sizeof(wire)));

  MeterTelemetry decoded;
  TEST_ASSERT_TRUE(unpackTelemetry(wire, sizeof(wire), decoded));
  TEST_ASSERT_EQUAL_UINT64(telemetry.importWh10, decoded.importWh10);
  TEST_ASSERT_EQUAL_UINT64(telemetry.exportWh10, decoded.exportWh10);
  TEST_ASSERT_EQUAL_INT32(telemetry.powerW, decoded.powerW);
  TEST_ASSERT_EQUAL_INT16(telemetry.powerL1W, decoded.powerL1W);
}

// The two sides of the link, end to end on the host: what the sender measured
// has to be what the receiver shows. This pair is where a load of 800 W arrived
// as 8000 W and the energy was inflated by ten, because power was passed through
// a conversion meant for the 0.1 Wh the energy field uses.
TEST_CASE("a received telemetry frame lands on the numbers the sender saw") {
  MeterValues sender;
  sender.update(Reading{kObisImportTotal, 22167615LL, -4, kUnitWh});  // 2216.7615 kWh
  sender.update(Reading{kObisExportTotal, 1234LL, -2, kUnitWh});
  sender.update(Reading{kObisPowerTotal, 80000LL, -2, kUnitW});       // 800 W
  sender.update(Reading{kObisPowerL1, 8500LL, -2, kUnitW});

  MeterValues received;
  applyTelemetry(toTelemetry(sender), received);

  // The wire resolves energy to 0.1 Wh, so that is the tolerance here - the
  // point of the test is the factor of ten, not the last digit.
  TEST_ASSERT_DOUBLE_WITHIN(0.05, sender.energyWh(kObisImportTotal),
                            received.energyWh(kObisImportTotal));
  TEST_ASSERT_DOUBLE_WITHIN(0.05, sender.energyWh(kObisExportTotal),
                            received.energyWh(kObisExportTotal));
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 800.0, received.powerW(kObisPowerTotal));
  TEST_ASSERT_DOUBLE_WITHIN(0.01, sender.powerW(kObisPowerL1), received.powerW(kObisPowerL1));
  TEST_ASSERT_TRUE(received.valid);
}

TEST_CASE("framer finds two telegrams in a stream of noise") {
  const SmlBuildEntry entry[] = {{kObisImportTotal, 100LL, -1, kUnitWh}};
  uint8_t telegram[128];
  const size_t len = buildSmlTelegram(entry, 1, telegram, sizeof(telegram));

  uint8_t stream[512];
  size_t n = 0;
  const uint8_t noise[] = {0x00, 0x1B, 0x1B, 0xFF, 0x42, 0x1B, 0x1B, 0x1B};
  memcpy(stream + n, noise, sizeof(noise));
  n += sizeof(noise);
  memcpy(stream + n, telegram, len);
  n += len;
  const uint8_t junk[] = {0x1B, 0x1B, 0x1B, 0x1B, 0x00, 0x1B, 0x1B, 0x1B, 0x1B};
  memcpy(stream + n, junk, sizeof(junk));
  n += sizeof(junk);
  memcpy(stream + n, telegram, len);
  n += len;

  SmlFramer framer;
  int complete = 0;
  int parsed = 0;
  for (size_t i = 0; i < n; ++i) {
    if (framer.feed(stream[i]) == SmlFramer::Result::Telegram) {
      ++complete;
      MeterValues values;
      if (parseSml(framer.data(), framer.size(), values)) ++parsed;
    }
  }
  TEST_ASSERT_EQUAL_INT(2, complete);
  TEST_ASSERT_EQUAL_INT(2, parsed);
}

TEST_CASE("a truncated telegram cannot be parsed and does not overrun") {
  const SmlBuildEntry entry[] = {{kObisImportTotal, 100LL, -1, kUnitWh}};
  uint8_t telegram[128];
  const size_t len = buildSmlTelegram(entry, 1, telegram, sizeof(telegram));

  MeterValues values;
  TEST_ASSERT_FALSE(parseSml(telegram, len - 6, values));
  TEST_ASSERT_EQUAL_UINT32(1, values.parseErrors);

  // Headers only, empty payload area.
  TEST_ASSERT_FALSE(parseSml(telegram, 12, values));
  TEST_ASSERT_FALSE(parseSml(nullptr, 0, values));
}

// The meter in the basement, byte for byte. Everything here was learned from a
// capture through the read head on a PC (tools/ir_head_test.py --dump) after the
// device reported no readings at all: the telegram is 636 bytes, not the ~200
// the parser was sized for, and EasyMeter puts one filler byte between the end
// marker and the CRC - which the checksum covers.
TEST_CASE("parses the real 636-byte telegram from the basement meter") {
  MeterValues values;
  SmlParseStats stats;
  TEST_ASSERT_TRUE(parseSml(fixtures::kEasymeterQ3Telegram,
                            fixtures::kEasymeterQ3TelegramLen, values, &stats));
  TEST_ASSERT_TRUE(values.crcOk);
  TEST_ASSERT_EQUAL_UINT8(1, stats.crcOffset);
  TEST_ASSERT_EQUAL_UINT8(0, values.crcErrors);
  TEST_ASSERT_TRUE(stats.lengthIncludesTagByte);

  // The meter's own numbers, as read off the wire: 13598.3825 kWh consumed,
  // nothing exported, and 902 W right now.
  const Reading* import = values.find(kObisImportTotal);
  TEST_ASSERT_NOT_NULL(import);
  TEST_ASSERT_EQUAL_UINT8(kUnitWh, import->unit);
  TEST_ASSERT_DOUBLE_WITHIN(0.001, 13598.3825, import->wh() / 1000.0);

  // It calls the instantaneous power 16.7.0; the firmware reports it as 1.7.0.
  const Reading* power = values.find(kObisPowerTotal);
  TEST_ASSERT_NOT_NULL(power);
  TEST_ASSERT_EQUAL_UINT8(kUnitW, power->unit);
  TEST_ASSERT_DOUBLE_WITHIN(0.1, 902.0, power->watts());

  // The three phases have to add up to that total - 85 + 286 + 530 = 901 W.
  TEST_ASSERT_DOUBLE_WITHIN(0.1, 901.0, values.powerW(kObisPowerL1) + values.powerW(kObisPowerL2) +
                                         values.powerW(kObisPowerL3));

  const Reading* volts = values.find(kObisVoltageL1);
  TEST_ASSERT_NOT_NULL(volts);
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 234.1, volts->value());

  // Amperes are unit 33. Reading them as anything else is how a current ends up
  // on the dashboard with the wrong unit next to it.
  const Reading* amps = values.find(kObisCurrentL3);
  TEST_ASSERT_NOT_NULL(amps);
  TEST_ASSERT_EQUAL_UINT8(kUnitA, amps->unit);
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 2.82, amps->value());
}

TEST_CASE("the framer delivers a telegram this long instead of overflowing") {
  SmlFramer framer;
  bool complete = false;
  for (size_t i = 0; i < fixtures::kEasymeterQ3TelegramLen; ++i) {
    if (framer.feed(fixtures::kEasymeterQ3Telegram[i]) == SmlFramer::Result::Telegram) {
      complete = true;
    }
  }
  TEST_ASSERT_TRUE(complete);
  TEST_ASSERT_EQUAL_UINT32(0, framer.resyncs());
  TEST_ASSERT_EQUAL_UINT32(1, framer.telegrams());

  MeterValues values;
  TEST_ASSERT_TRUE(parseSml(framer.data(), framer.size(), values, nullptr));
  TEST_ASSERT_TRUE(values.crcOk);
}

TEST_CASE("a standard-layout telegram still frames and verifies") {
  // The layout the builder writes: CRC straight after the end marker, no filler
  // byte. The framer now takes one byte more than that needs, so this pins down
  // that the extra byte is given back and no telegram is lost in a stream.
  const SmlBuildEntry entry[] = {{kObisImportTotal, 4200LL, -1, kUnitWh}};
  uint8_t telegram[128];
  const size_t len = buildSmlTelegram(entry, 1, telegram, sizeof(telegram));

  uint8_t stream[256];
  memcpy(stream, telegram, len);
  memcpy(stream + len, telegram, len);

  SmlFramer framer;
  int complete = 0;
  int parsed = 0;
  for (size_t i = 0; i < 2 * len; ++i) {
    if (framer.feed(stream[i]) == SmlFramer::Result::Telegram) {
      ++complete;
      MeterValues values;
      SmlParseStats stats;
      if (parseSml(framer.data(), framer.size(), values, &stats) && values.crcOk) {
        TEST_ASSERT_EQUAL_UINT8(0, stats.crcOffset);
        ++parsed;
      }
    }
  }
  TEST_ASSERT_EQUAL_INT(2, complete);
  TEST_ASSERT_EQUAL_INT(2, parsed);
}
