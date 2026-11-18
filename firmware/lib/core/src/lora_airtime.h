#pragma once
// LoRa time-on-air, computed from the modem parameters.
//
// This exists rather than calling RadioLib's getTimeOnAir() because airtime is
// what the duty-cycle budget is spent in: the number is legally relevant, so it
// is computed from the documented Semtech formula, unit tested, and not
// dependent on a library's return unit.

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace core {

// sf 7..12, bwKhz in kHz, cr 5..8 (RadioLib's 4/5..4/8), preamble in symbols.
inline double loraAirtimeMs(uint8_t sf, double bwKhz, uint8_t cr, uint8_t preamble,
                            size_t payloadBytes, bool crc = true, bool implicitHeader = false) {
  if (sf < 6 || sf > 12 || bwKhz <= 0.0 || cr < 5 || cr > 8) return 0.0;

  const double symbolMs = static_cast<double>(1u << sf) / (bwKhz * 1000.0) * 1000.0;
  const double preambleMs = (static_cast<double>(preamble) + 4.25) * symbolMs;

  const int de = (sf >= 11 && bwKhz <= 125.0) ? 1 : 0;
  const double numerator = 8.0 * static_cast<double>(payloadBytes) - 4.0 * sf + 28.0 +
                           (crc ? 16.0 : 0.0) - (implicitHeader ? 20.0 : 0.0);
  const double denominator = 4.0 * (sf - 2.0 * de);
  double symbols = std::ceil(numerator / denominator) * (cr - 4 + 4);
  if (symbols < 0) symbols = 0;
  const double payloadMs = (8.0 + symbols) * symbolMs;

  return preambleMs + payloadMs;
}

// Rounds up: a budget that is short by a fraction of a millisecond is still a
// budget violation.
inline uint32_t loraAirtimeMsCeil(uint8_t sf, double bwKhz, uint8_t cr, uint8_t preamble,
                                  size_t payloadBytes) {
  const double ms = loraAirtimeMs(sf, bwKhz, cr, preamble, payloadBytes);
  return static_cast<uint32_t>(std::ceil(ms));
}

}  // namespace core
