#pragma once
// Measurement model shared by the meter reader (basement node) and the gateway.
//
// No Arduino headers here on purpose: this file is compiled by the native test
// environment as well as by the firmware, so the parsing and packing logic can
// be unit tested on the host.

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace core {

// OBIS reduced to its (C,D,E) triple. The A/B prefix bytes and the trailing
// *255 differ between manufacturers, the measurement itself is C.D.E.
enum ObisCode : uint32_t {
  kObisImportTotal = 0x010800,  // 1.8.0  Bezug, total
  kObisImportT1 = 0x010801,     // 1.8.1  Hochtarif
  kObisImportT2 = 0x010802,     // 1.8.2  Niedertarif
  kObisExportTotal = 0x020800,  // 2.8.0  Einspeisung, total
  kObisExportT1 = 0x020801,
  kObisExportT2 = 0x020802,
  kObisPowerTotal = 0x010700,   // 1.7.0  Momentanleistung
  kObisPowerTotal16 = 0x100700, // 16.7.0 Momentanleistung, Summe - the same quantity
                                // under the code most meters actually send
  kObisPowerL1 = 0x240700,      // 36.7.0
  kObisPowerL2 = 0x380700,      // 56.7.0
  kObisPowerL3 = 0x4C0700,      // 76.7.0
  kObisVoltageL1 = 0x200700,    // 32.7.0
  kObisVoltageL2 = 0x340700,
  kObisVoltageL3 = 0x480700,
  kObisCurrentL1 = 0x1F0700,    // 31.7.0
  kObisCurrentL2 = 0x330700,
  kObisCurrentL3 = 0x470700,
};

// SML unit codes (the byte that follows the 0x62 unit tag). Read off the meter
// in the basement, not off a table: it labels 1.8.0 with 30 (Wh), 16.7.0 with 27
// (W), 31.7.0/51.7.0 with 33 (A), 32.7.0 with 35 (V) and 14.7.0 with 44 (Hz).
// Amperes are 33 - the earlier value of 31 was wrong and made a current reading
// look like reactive energy.
enum UnitCode : uint8_t {
  kUnitWh = 30,
  kUnitVarh = 31,
  kUnitVAh = 32,
  kUnitW = 27,
  kUnitVar = 28,
  kUnitVA = 29,
  kUnitA = 33,
  kUnitV = 35,
  kUnitHz = 44,
  kUnitNone = 0xFF,
};

inline bool isEnergyUnit(uint8_t unit) {
  return unit == kUnitWh || unit == kUnitVarh || unit == kUnitVAh;
}
inline bool isPowerUnit(uint8_t unit) { return unit == kUnitW || unit == kUnitVar || unit == kUnitVA; }

// One decoded OBIS value. `raw` is the integer as transmitted; the real value
// is raw * 10^scaler (scaler is usually negative).
struct Reading {
  uint32_t obis = 0;
  int64_t raw = 0;
  int8_t scaler = 0;
  uint8_t unit = kUnitNone;

  double value() const { return static_cast<double>(raw) * std::pow(10.0, scaler); }

  // Energy in Wh regardless of whether the meter reported Wh, kWh or varh.
  double wh() const { return isEnergyUnit(unit) ? value() : 0.0; }
  double watts() const { return isPowerUnit(unit) ? value() : 0.0; }
};

// Latest known value per OBIS, with the health counters the web UI and HA
// diagnostics need.
struct MeterValues {
  static constexpr size_t kSlots = 16;

  Reading slots[kSlots];
  uint8_t count = 0;

  bool valid = false;            // at least one telegram parsed
  bool crcOk = false;            // last telegram's checksum matched
  uint32_t telegramCount = 0;
  uint32_t crcErrors = 0;
  uint32_t parseErrors = 0;
  uint32_t lastTelegramAtMs = 0;

  Reading* find(uint32_t obis) {
    for (uint8_t i = 0; i < count; ++i) {
      if (slots[i].obis == obis) return &slots[i];
    }
    return nullptr;
  }
  const Reading* find(uint32_t obis) const {
    return const_cast<MeterValues*>(this)->find(obis);
  }
  bool has(uint32_t obis) const { return find(obis) != nullptr; }

  // Stores or replaces the value for this OBIS. Returns false when the table is
  // full (a meter with an unexpected OBIS mix - never fatal, just ignored).
  bool update(const Reading& r) {
    Reading* slot = find(r.obis);
    if (slot) {
      *slot = r;
      return true;
    }
    if (count >= kSlots) return false;
    slots[count++] = r;
    return true;
  }

  double energyWh(uint32_t obis) const {
    const Reading* r = find(obis);
    return r ? r->wh() : 0.0;
  }
  double powerW(uint32_t obis) const {
    const Reading* r = find(obis);
    return r ? r->watts() : 0.0;
  }
};

// Compact wire form for the LoRa link. Fixed units, so the receiver needs no
// scaler tables and the payload stays at 23 bytes - airtime is legality
// (1 % duty cycle), so every byte here is paid for in transmit time.
struct MeterTelemetry {
  enum Flags : uint8_t {
    kImport = 1u << 0,
    kExport = 1u << 1,
    kPower = 1u << 2,
    kPhases = 1u << 3,
  };

  uint8_t flags = 0;
  uint64_t importWh10 = 0;  // energy in 0.1 Wh
  uint64_t exportWh10 = 0;
  int32_t powerW = 0;
  int16_t powerL1W = 0;
  int16_t powerL2W = 0;
  int16_t powerL3W = 0;

  static constexpr size_t kWireSize = 1 + 6 + 6 + 4 + 3 * 2;  // 23
};

inline void putLe(uint8_t* p, uint64_t value, int bytes) {
  for (int i = 0; i < bytes; ++i) p[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
}

inline uint64_t getLe(const uint8_t* p, int bytes) {
  uint64_t value = 0;
  for (int i = 0; i < bytes; ++i) value |= static_cast<uint64_t>(p[i]) << (8 * i);
  return value;
}

inline uint64_t saturatingEnergy(double wh) {
  if (!(wh > 0.0)) return 0;
  const double units = wh * 10.0;  // 0.1 Wh
  const double max = 281474976710655.0;  // 2^48 - 1
  if (units >= max) return static_cast<uint64_t>(max);
  // The field resolves 0.1 Wh: round to it rather than dropping the remainder,
  // which would bias every reading downwards.
  return static_cast<uint64_t>(units + 0.5);
}

inline int32_t saturatingI32(double v) {
  if (v > 2147483647.0) return 2147483647;
  if (v < -2147483648.0) return -2147483648;
  return static_cast<int32_t>(v);
}

inline int16_t saturatingI16(double v) {
  if (v > 32767.0) return 32767;
  if (v < -32768.0) return -32768;
  return static_cast<int16_t>(v);
}

inline size_t packTelemetry(const MeterTelemetry& t, uint8_t* out, size_t cap) {
  if (cap < MeterTelemetry::kWireSize) return 0;
  out[0] = t.flags;
  putLe(out + 1, t.importWh10, 6);
  putLe(out + 7, t.exportWh10, 6);
  putLe(out + 13, static_cast<uint32_t>(t.powerW), 4);
  putLe(out + 17, static_cast<uint16_t>(t.powerL1W), 2);
  putLe(out + 19, static_cast<uint16_t>(t.powerL2W), 2);
  putLe(out + 21, static_cast<uint16_t>(t.powerL3W), 2);
  return MeterTelemetry::kWireSize;
}

inline bool unpackTelemetry(const uint8_t* in, size_t len, MeterTelemetry& out) {
  if (len < MeterTelemetry::kWireSize) return false;
  out.flags = in[0];
  out.importWh10 = getLe(in + 1, 6);
  out.exportWh10 = getLe(in + 7, 6);
  out.powerW = static_cast<int32_t>(getLe(in + 13, 4));
  out.powerL1W = static_cast<int16_t>(getLe(in + 17, 2));
  out.powerL2W = static_cast<int16_t>(getLe(in + 19, 2));
  out.powerL3W = static_cast<int16_t>(getLe(in + 21, 2));
  return true;
}

inline MeterTelemetry toTelemetry(const MeterValues& v) {
  MeterTelemetry t;
  if (const Reading* r = v.find(kObisImportTotal)) {
    t.flags |= MeterTelemetry::kImport;
    t.importWh10 = saturatingEnergy(r->wh());
  }
  if (const Reading* r = v.find(kObisExportTotal)) {
    t.flags |= MeterTelemetry::kExport;
    t.exportWh10 = saturatingEnergy(r->wh());
  }
  if (const Reading* r = v.find(kObisPowerTotal)) {
    t.flags |= MeterTelemetry::kPower;
    t.powerW = saturatingI32(r->watts());
  }
  const Reading* l1 = v.find(kObisPowerL1);
  const Reading* l2 = v.find(kObisPowerL2);
  const Reading* l3 = v.find(kObisPowerL3);
  if (l1 || l2 || l3) {
    t.flags |= MeterTelemetry::kPhases;
    if (l1) t.powerL1W = saturatingI16(l1->watts());
    if (l2) t.powerL2W = saturatingI16(l2->watts());
    if (l3) t.powerL3W = saturatingI16(l3->watts());
  }
  return t;
}

  // The inverse of toTelemetry(): fills `out` from a frame that arrived over the
  // link. This is where the units of the two sides meet, which is why it lives
  // here and not in the firmware - the pair can be tested against each other on
  // the host. Energy arrives in 0.1 Wh and is divided by ten, power arrives in W
  // and is taken as it is; everything is stored with scaler -1, so wh() and
  // watts() return exactly what the sender measured.
  inline void applyTelemetry(const MeterTelemetry& t, MeterValues& out) {
    auto store = [&out](uint32_t obis, double value, uint8_t unit) {
      Reading reading;
      reading.obis = obis;
      reading.unit = unit;
      reading.raw = static_cast<int64_t>(value * 10.0);
      reading.scaler = -1;  // stored in 0.1 units
      out.update(reading);
    };

    if (t.flags & MeterTelemetry::kImport) {
      store(kObisImportTotal, static_cast<double>(t.importWh10) / 10.0, kUnitWh);
    }
    if (t.flags & MeterTelemetry::kExport) {
      store(kObisExportTotal, static_cast<double>(t.exportWh10) / 10.0, kUnitWh);
    }
    if (t.flags & MeterTelemetry::kPower) {
      store(kObisPowerTotal, t.powerW, kUnitW);
    }
    if (t.flags & MeterTelemetry::kPhases) {
      store(kObisPowerL1, t.powerL1W, kUnitW);
      store(kObisPowerL2, t.powerL2W, kUnitW);
      store(kObisPowerL3, t.powerL3W, kUnitW);
    }
    out.valid = true;
  }

}  // namespace core
