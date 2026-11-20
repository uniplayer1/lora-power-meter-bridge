#pragma once
// The meter node's own status, reported to the gateway every few minutes.
//
// The gateway only knows its own side of the link: its airtime, memory and
// how well it hears the node. Without this report Home Assistant showed the
// gateway's "Sendezeit" under a name that read as the meter's, and the node's
// budget, memory and read-head counters were visible only on its own access
// point in the basement. 25 bytes, sealed like a reading.

#include <cstddef>
#include <cstdint>

#include "meter_values.h"

namespace core {

struct NodeStatus {
  enum MeterFlags : uint8_t {
    kMeterResolved = 1u << 0,  // the protocol is known (a telegram decoded)
    kMeterSml = 1u << 1,
    kMeterD0 = 1u << 2,
    kMeterInverted = 1u << 3,  // RX polarity currently inverted
    kMeterSimulating = 1u << 4,
  };
  static constexpr uint8_t kVersion = 1;
  static constexpr uint16_t kAgeNever = 0xFFFF;

  uint16_t airtimeUsedDs = 0;   // airtime used in the rolling hour, 0.1 s
  uint16_t txBlocked = 0;       // sends held back by the budget (saturating)
  uint16_t heapFreeKb = 0;
  uint16_t heapMinKb = 0;       // lowest free heap since boot
  int8_t rssi = 0;              // how the node hears the gateway, dBm
  int8_t snrQuarterDb = 0;      // SNR in 0.25 dB
  uint32_t telegrams = 0;       // telegrams decoded since boot
  uint16_t telegramErrors = 0;  // checksum + parse errors (saturating)
  uint16_t telegramAgeS = kAgeNever;  // age of the last telegram
  uint8_t meterFlags = 0;
  uint8_t rxPin = 0;
  uint32_t bytesSeen = 0;       // bytes received from the read head

  static constexpr size_t kWireSize = 1 + 2 + 2 + 2 + 2 + 1 + 1 + 4 + 2 + 2 + 1 + 1 + 4;  // 25
};

inline uint16_t saturate16(uint32_t value) { return value > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(value); }

inline size_t packNodeStatus(const NodeStatus& s, uint8_t* out, size_t cap) {
  if (out == nullptr || cap < NodeStatus::kWireSize) return 0;
  size_t i = 0;
  out[i++] = NodeStatus::kVersion;
  putLe(out + i, s.airtimeUsedDs, 2); i += 2;
  putLe(out + i, s.txBlocked, 2); i += 2;
  putLe(out + i, s.heapFreeKb, 2); i += 2;
  putLe(out + i, s.heapMinKb, 2); i += 2;
  out[i++] = static_cast<uint8_t>(s.rssi);
  out[i++] = static_cast<uint8_t>(s.snrQuarterDb);
  putLe(out + i, s.telegrams, 4); i += 4;
  putLe(out + i, s.telegramErrors, 2); i += 2;
  putLe(out + i, s.telegramAgeS, 2); i += 2;
  out[i++] = s.meterFlags;
  out[i++] = s.rxPin;
  putLe(out + i, s.bytesSeen, 4); i += 4;
  return i;
}

// Refuses a payload that is too short or from a newer layout version; a newer
// node may append fields, which an older gateway simply does not read.
inline bool unpackNodeStatus(const uint8_t* in, size_t len, NodeStatus& s) {
  if (in == nullptr || len < NodeStatus::kWireSize || in[0] != NodeStatus::kVersion) return false;
  size_t i = 1;
  s.airtimeUsedDs = static_cast<uint16_t>(getLe(in + i, 2)); i += 2;
  s.txBlocked = static_cast<uint16_t>(getLe(in + i, 2)); i += 2;
  s.heapFreeKb = static_cast<uint16_t>(getLe(in + i, 2)); i += 2;
  s.heapMinKb = static_cast<uint16_t>(getLe(in + i, 2)); i += 2;
  s.rssi = static_cast<int8_t>(in[i++]);
  s.snrQuarterDb = static_cast<int8_t>(in[i++]);
  s.telegrams = static_cast<uint32_t>(getLe(in + i, 4)); i += 4;
  s.telegramErrors = static_cast<uint16_t>(getLe(in + i, 2)); i += 2;
  s.telegramAgeS = static_cast<uint16_t>(getLe(in + i, 2)); i += 2;
  s.meterFlags = in[i++];
  s.rxPin = in[i++];
  s.bytesSeen = static_cast<uint32_t>(getLe(in + i, 4)); i += 4;
  return true;
}

}  // namespace core
