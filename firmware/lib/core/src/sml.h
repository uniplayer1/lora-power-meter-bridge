#pragma once
// SML (Smart Message Language, BSI TR-03109-1) framing and parsing.
//
// Wire format, as seen on real meters (byte-exact example from an EasyMeter
// Q3B, 1.8.0 = 2216.7611 kWh):
//
//   77 07 01 00 01 08 00 ff 65 00 00 80 01 62 1e 52 fc 59 00 00 00 05 29 4a f8 c9 01
//   |  |  |                 |              |     |     |                       |
//   |  |  |                 |              |     |     |                       +- signature
//   |  |  |                 |              |     |     +- value   (0x59 -> 8 bytes, big endian)
//   |  |  |                 |              |     +- scaler 0x52 -> 1 byte, 0xfc = -4
//   |  |  |                 |              +- unit 0x62 -> 1 byte, 0x1e = 30 = Wh
//   |  |  |                 +- status
//   |  |  +- OBIS 01 00 01 08 00 ff  (1-0:1.8.0*255)
//   |  +- item count (7 = OBIS + 6 tagged items)
//   +- list-entry marker
//
// The length nibble of a tagged item COUNTS THE TAG BYTE ITSELF: 0x62 carries
// one data byte, 0x59 carries eight. Internal consistency of the sample above
// (7 items, every length byte correct, value 22167615689 * 10^-4) is what fixed
// this reading - see test/test_sml for the assertion. A second convention
// (length excluding the tag byte) is tried as a fallback for meters whose
// encoding differs, and parseSml() reports which one fitted.
//
// A CRC mismatch is reported but never discards values: a meter we mis-frame
// should still produce numbers on the dashboard, flagged as unverified.

#include <cstddef>
#include <cstdint>

#include "meter_values.h"

namespace core {

// Telegram framing: start marker 1B 1B 1B 1B 01 01 01 01, end marker
// 1B 1B 1B 1B 1A, then the 2-byte CRC-16/X.25.
class SmlFramer {
 public:
  // A real telegram is far longer than the SML books suggest: the meter in the
  // basement sends 636 bytes (620 of payload, the 5-byte end marker, one filler
  // byte and the CRC), once a second. At 512 the framer overflowed on every
  // single telegram and reported zero readings while the wire was perfectly
  // fine - which is exactly what it looked like from the outside.
  static constexpr size_t kMaxTelegram = 1024;
  enum class Result { None, Telegram, Overflow };

  Result feed(uint8_t byte);

  const uint8_t* data() const { return buf_; }
  size_t size() const { return len_; }
  uint32_t telegrams() const { return telegrams_; }
  uint32_t resyncs() const { return resyncs_; }
  void reset();

 private:
  static constexpr uint8_t kStart[8] = {0x1B, 0x1B, 0x1B, 0x1B, 0x01, 0x01, 0x01, 0x01};
  static constexpr uint8_t kEnd[5] = {0x1B, 0x1B, 0x1B, 0x1B, 0x1A};

  uint8_t buf_[kMaxTelegram] = {0};
  size_t len_ = 0;
  uint8_t window_[sizeof(kStart)] = {0};
  size_t windowLen_ = 0;
  // The trailer after the end marker is the CRC plus at most one filler byte
  // that some meters insert - and cover with the checksum. The framer validates
  // the CRC itself, so it emits a telegram the moment the checksum fits instead
  // of waiting for a byte that may not arrive for another second.
  bool crcValidAt(size_t shift) const;

  bool inTelegram_ = false;
  bool endSeen_ = false;
  size_t endIndex_ = 0;     // where the end marker started
  uint8_t trailerLen_ = 0;  // bytes seen since, CRC included
  uint32_t telegrams_ = 0;
  uint32_t resyncs_ = 0;
};

// Debug view of the last entry the parser looked at. Surfaced through the
// `meter dump` CLI command so the real meter can be decoded in one round trip
// instead of guessing at the grammar remotely.
struct SmlEntryDebug {
  uint8_t obis[6] = {0};
  uint8_t itemCount = 0;
  int64_t raw = 0;
  int8_t scaler = 0;
  uint8_t unit = UnitCode::kUnitNone;
  bool hasValue = false;
};

struct SmlParseStats {
  uint16_t crcExpected = 0;
  uint16_t crcActual = 0;
  bool crcByteSwapped = false;
  uint8_t crcOffset = 0;              // bytes between the end marker and the CRC
  bool lengthIncludesTagByte = true;  // which convention fitted
  uint8_t entries = 0;
  uint8_t unknownEntries = 0;
  SmlEntryDebug last;
};

// Parses one complete telegram (start marker .. CRC inclusive). Returns true
// when at least one OBIS value was extracted.

// Checks the framing alone, without decoding a single entry: the start marker and
// the end marker are where they belong, and the CRC over everything up to it
// matches (with or without the filler byte some meters insert). A telegram whose
// entries no longer decode but whose framing is intact is a parser gap; one whose
// framing is broken is a wire, timing or buffer-size problem. Telling those two
// apart is the whole point of having this as its own function.
bool smlFramingOk(const uint8_t* telegram, size_t len);
bool parseSml(const uint8_t* telegram, size_t len, MeterValues& out,
              SmlParseStats* stats = nullptr);

// Synthesises a telegram in the wire format above. Used by the tests and by the
// firmware's meter simulator, so the whole chain can be exercised without the
// IR read head attached.
struct SmlBuildEntry {
  uint32_t obis;
  int64_t raw;
  int8_t scaler;
  uint8_t unit;
};

size_t buildSmlTelegram(const SmlBuildEntry* entries, size_t count, uint8_t* out,
                        size_t cap);

}  // namespace core
