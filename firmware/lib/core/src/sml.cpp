#include "sml.h"

#include <cstring>

#include "crc16.h"

namespace core {
namespace {

const uint8_t kStartMarker[8] = {0x1B, 0x1B, 0x1B, 0x1B, 0x01, 0x01, 0x01, 0x01};
const uint8_t kEndMarker[5] = {0x1B, 0x1B, 0x1B, 0x1B, 0x1A};
constexpr uint8_t kListEntry = 0x77;

// Bytes the framer consumes after the end marker before it hands a telegram on:
// the 2-byte CRC plus one byte of slack. Meters differ in whether anything sits
// between the end marker and the CRC - EasyMeter puts a filler byte there and
// covers it with the checksum - so the framer always takes three and gives the
// last one back to the start-marker window. See SmlFramer::feed().
constexpr int kTrailerBytes = 3;

bool isKnownUnit(uint8_t unit) {
  switch (unit) {
    case UnitCode::kUnitWh:
    case UnitCode::kUnitVarh:
    case UnitCode::kUnitVAh:
    case UnitCode::kUnitW:
    case UnitCode::kUnitVar:
    case UnitCode::kUnitVA:
    case UnitCode::kUnitA:
    case UnitCode::kUnitV:
    case UnitCode::kUnitHz:
      return true;
    default:
      return false;
  }
}

size_t findEndMarker(const uint8_t* buf, size_t len, size_t from) {
  if (len < sizeof(kEndMarker)) return len;
  for (size_t i = from; i + sizeof(kEndMarker) <= len; ++i) {
    if (memcmp(buf + i, kEndMarker, sizeof(kEndMarker)) == 0) return i;
  }
  return len;
}

int64_t decodeBigEndian(const uint8_t* data, int bytes, bool isSigned) {
  uint64_t value = 0;
  for (int i = 0; i < bytes; ++i) value = (value << 8) | data[i];
  if (isSigned && bytes > 0 && bytes < 8 && (data[0] & 0x80)) {
    value |= (~static_cast<uint64_t>(0)) << (bytes * 8);
  }
  return static_cast<int64_t>(value);
}

// Walks the entries of one telegram under a given length convention. Returns
// true when at least one entry was structurally sound; parseSml() retries with
// the other convention when nothing came out of this walk.
//
// A byte that only LOOKS like an entry marker is expected, not exceptional: the
// server-id string of the meter in the basement contains a 0x77, and aborting
// the whole walk on it hid a perfectly good telegram. A malformed entry is
// therefore skipped - the walk resyncs on the next marker - instead of ending
// the telegram.
bool walkEntries(const uint8_t* buf, size_t endIndex, bool lengthIncludesTagByte,
                 MeterValues& out, SmlParseStats& stats) {
  size_t i = sizeof(kStartMarker);
  uint8_t entries = 0;
  uint8_t unknown = 0;

  while (i < endIndex) {
    if (buf[i] != kListEntry) {
      ++i;  // resync on the next marker; meters repeat the entry marker
      continue;
    }
    if (i + 2 + 6 > endIndex) break;  // no room for another OBIS: end of data
    const size_t entryStart = i;
    const uint8_t itemCount = buf[i + 1];
    const uint8_t* obisBytes = buf + i + 2;
    i += 8;

    // 01 00 <C> <D> <E> <F> - the measurement is identified by C.D.E.
    const uint32_t obis = (static_cast<uint32_t>(obisBytes[2]) << 16) |
                          (static_cast<uint32_t>(obisBytes[3]) << 8) |
                          static_cast<uint32_t>(obisBytes[4]);

    Reading reading;
    // 16.7.0 and 1.7.0 are the same quantity - the meter in the basement sends
    // 16.7.0. Reporting both under 1.7.0 keeps every consumer (OLED, HTTP, MQTT,
    // Home Assistant) from having to know which code a given meter picked.
    reading.obis = (obis == kObisPowerTotal16) ? kObisPowerTotal : obis;
    bool hasValue = false;
    bool hasScaleReference = false;
    bool complete = true;
    uint8_t items = 1;  // the OBIS itself counts as an item

    // The entry ends where the next marker starts; the declared item count is
    // recorded for the debug view but not trusted for walking.
    while (i < endIndex && buf[i] != kListEntry) {
      const uint8_t tag = buf[i];
      const uint8_t type = tag >> 4;
      int dataLen = tag & 0x0F;
      dataLen = lengthIncludesTagByte ? dataLen - 1 : dataLen;
      if (dataLen < 0 || i + 1 + static_cast<size_t>(dataLen) > endIndex) {
        complete = false;
        break;
      }
      const uint8_t* data = buf + i + 1;
      i += 1 + static_cast<size_t>(dataLen);
      ++items;

      if (type == 6 && dataLen == 1 && isKnownUnit(data[0])) {
        reading.unit = data[0];
        hasScaleReference = true;
      } else if (type == 5 && dataLen == 1 && data[0] >= 0xF7  /* -9 */) {
        reading.scaler = static_cast<int8_t>(data[0]);
        hasScaleReference = true;
      } else if ((type == 5 || type == 6) && dataLen >= 2 && dataLen <= 8) {
        // Only accept a value once this entry has shown a unit or a scaler:
        // the status word is also a 4-byte unsigned and must not be mistaken
        // for a reading.
        if (hasScaleReference) {
          reading.raw = decodeBigEndian(data, dataLen, type == 5);
          hasValue = true;
        }
      }
      // Everything else (status, valTime, signature, nested lists) is skipped
      // by its length - that is what keeps the walk robust.
    }

    if (!complete) {
      i = entryStart + 1;  // not an entry after all: resync on the next marker
      continue;
    }

    ++entries;
    if (hasValue) {
      if (!out.update(reading)) ++unknown;  // table full
      stats.last.obis[0] = obisBytes[0];
      stats.last.obis[1] = obisBytes[1];
      stats.last.obis[2] = obisBytes[2];
      stats.last.obis[3] = obisBytes[3];
      stats.last.obis[4] = obisBytes[4];
      stats.last.obis[5] = obisBytes[5];
      stats.last.itemCount = itemCount;
      stats.last.raw = reading.raw;
      stats.last.scaler = reading.scaler;
      stats.last.unit = reading.unit;
      stats.last.hasValue = true;
    } else {
      ++unknown;
    }
  }

  stats.entries = entries;
  stats.unknownEntries = unknown;
  return entries > 0;
}

}  // namespace

bool SmlFramer::crcValidAt(size_t shift) const {
  const size_t covered = endIndex_ + sizeof(kEndMarker) + shift;
  if (covered + 2 > len_) return false;
  const uint16_t expected = crc16X25Finish(crc16X25(buf_, covered));
  const uint16_t littleEndian =
      static_cast<uint16_t>(buf_[covered] | (buf_[covered + 1] << 8));
  const uint16_t bigEndian =
      static_cast<uint16_t>((buf_[covered] << 8) | buf_[covered + 1]);
  return littleEndian == expected || bigEndian == expected;
}

SmlFramer::Result SmlFramer::feed(uint8_t byte) {
  if (!inTelegram_) {
    if (windowLen_ < sizeof(kStart)) {
      window_[windowLen_++] = byte;
    } else {
      memmove(window_, window_ + 1, sizeof(kStart) - 1);
      window_[sizeof(kStart) - 1] = byte;
    }
    if (windowLen_ == sizeof(kStart) && memcmp(window_, kStartMarker, sizeof(kStart)) == 0) {
      inTelegram_ = true;
      endSeen_ = false;
      trailerLen_ = 0;
      windowLen_ = 0;
      len_ = 0;
      memcpy(buf_, kStartMarker, sizeof(kStartMarker));
      len_ = sizeof(kStartMarker);
    }
    return Result::None;
  }

  if (len_ >= kMaxTelegram) {
    inTelegram_ = false;
    endSeen_ = false;
    trailerLen_ = 0;
    len_ = 0;
    windowLen_ = 0;
    ++resyncs_;
    return Result::Overflow;
  }

  buf_[len_++] = byte;

  if (!endSeen_ && len_ >= sizeof(kEndMarker) &&
      memcmp(buf_ + len_ - sizeof(kEndMarker), kEndMarker, sizeof(kEndMarker)) == 0) {
    endSeen_ = true;
    endIndex_ = len_ - sizeof(kEndMarker);
    trailerLen_ = 0;
    return Result::None;
  }

  if (endSeen_) {
    ++trailerLen_;
    // A CRC that fits straight after the end marker is the standard layout and
    // is handed on at once. Otherwise the extra byte the other layout implies
    // is waited for - and when it turns out not to belong to this telegram it
    // goes back into the start-marker window instead of being swallowed.
    const bool emit = (trailerLen_ == 2) ? crcValidAt(0) : (trailerLen_ >= kTrailerBytes);
    if (!emit) return Result::None;
    if (trailerLen_ >= kTrailerBytes && !crcValidAt(1)) {
      --len_;
      window_[0] = byte;
      windowLen_ = 1;
    }
    inTelegram_ = false;
    endSeen_ = false;
    trailerLen_ = 0;
    ++telegrams_;
    return Result::Telegram;
  }
  return Result::None;
}

void SmlFramer::reset() {
  len_ = 0;
  windowLen_ = 0;
  inTelegram_ = false;
  endSeen_ = false;
  endIndex_ = 0;
  trailerLen_ = 0;
}

bool parseSml(const uint8_t* telegram, size_t len, MeterValues& out, SmlParseStats* stats) {
  SmlParseStats local;
  SmlParseStats& st = stats ? *stats : local;

  if (telegram == nullptr || len < sizeof(kStartMarker) + sizeof(kEndMarker) + 2 ||
      memcmp(telegram, kStartMarker, sizeof(kStartMarker)) != 0) {
    ++out.parseErrors;
    return false;
  }

  const size_t endIndex = findEndMarker(telegram, len, sizeof(kStartMarker));
  if (endIndex >= len) {
    ++out.parseErrors;
    return false;
  }

  // Checksum over start marker .. end marker inclusive, CRC-16/X.25, low byte
  // first. Meters differ in what sits between the end marker and the CRC: the
  // standard has nothing, EasyMeter inserts one filler byte and covers it. Both
  // offsets are tried, and the one that matched is reported in the stats. A
  // mismatch never discards values - see the note in sml.h.
  const size_t crcIndex = endIndex + sizeof(kEndMarker);
  st.crcOffset = 0;
  st.crcExpected = 0;
  st.crcActual = 0;
  st.crcByteSwapped = false;
  out.crcOk = false;
  for (size_t shift = 0; shift <= 1; ++shift) {
    const size_t covered = crcIndex + shift;
    if (covered + 2 > len) break;
    const uint16_t expected = crc16X25Finish(crc16X25(telegram, covered));
    const uint16_t littleEndian =
        static_cast<uint16_t>(telegram[covered] | (telegram[covered + 1] << 8));
    const uint16_t bigEndian =
        static_cast<uint16_t>((telegram[covered] << 8) | telegram[covered + 1]);
    const bool swapped = (bigEndian == expected) && (littleEndian != expected);
    if (shift == 0 || littleEndian == expected || bigEndian == expected) {
      st.crcExpected = expected;
      st.crcActual = swapped ? bigEndian : littleEndian;
      st.crcByteSwapped = swapped;
    }
    if (littleEndian == expected || bigEndian == expected) {
      st.crcOffset = static_cast<uint8_t>(shift);
      out.crcOk = true;
      break;
    }
  }

  MeterValues parsed;
  parsed.valid = true;
  st.lengthIncludesTagByte = true;
  bool ok = walkEntries(telegram, endIndex, true, parsed, st);
  if (!ok || parsed.count == 0) {
    MeterValues fallback;
    fallback.valid = true;
    SmlParseStats fbStats = st;
    fbStats.lengthIncludesTagByte = false;
    if (walkEntries(telegram, endIndex, false, fallback, fbStats) && fallback.count > 0) {
      parsed = fallback;
      st = fbStats;
      ok = true;
    }
  }

  if (!ok || parsed.count == 0) {
    ++out.parseErrors;
    return false;
  }

  for (uint8_t i = 0; i < parsed.count; ++i) out.update(parsed.slots[i]);
  out.valid = true;
  ++out.telegramCount;
  if (!out.crcOk) ++out.crcErrors;
  return true;
}

size_t buildSmlTelegram(const SmlBuildEntry* entries, size_t count, uint8_t* out, size_t cap) {
  size_t i = 0;
  auto put = [&](uint8_t value) {
    if (i < cap) out[i] = value;
    ++i;
  };

  for (uint8_t b : kStartMarker) put(b);

  // Minimal message envelope: the parser only keys on the list-entry markers,
  // so this carries the transaction/group bytes a real meter would send.
  const uint8_t envelope[] = {0x76, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01};
  for (uint8_t b : envelope) put(b);

  for (size_t e = 0; e < count; ++e) {
    const SmlBuildEntry& entry = entries[e];
    put(kListEntry);
    put(7);  // OBIS + status + unit + scaler + value + signature = 6 -> 7 items
    put(0x01);
    put(0x00);
    put(static_cast<uint8_t>((entry.obis >> 16) & 0xFF));
    put(static_cast<uint8_t>((entry.obis >> 8) & 0xFF));
    put(static_cast<uint8_t>(entry.obis & 0xFF));
    put(0xFF);
    // status: 4 data bytes, tag length includes the tag byte
    const uint8_t status[] = {0x65, 0x00, 0x00, 0x80, 0x01};
    for (uint8_t b : status) put(b);
    put(0x62);
    put(entry.unit);
    put(0x52);
    put(static_cast<uint8_t>(entry.scaler));  // len byte, 8 data bytes
    put(0x59);
    for (int shift = 56; shift >= 0; shift -= 8) {
      put(static_cast<uint8_t>((static_cast<uint64_t>(entry.raw) >> shift) & 0xFF));
    }
    put(0x01);  // signature
  }

  for (uint8_t b : kEndMarker) put(b);

  const size_t crcIndex = i;  // CRC covers start marker .. end marker
  if (i + 2 > cap) return 0;
  const uint16_t crc = crc16X25Finish(crc16X25(out, i));
  out[i++] = static_cast<uint8_t>(crc & 0xFF);
  out[i++] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  (void)crcIndex;
  return i;
}

}  // namespace core
