#pragma once
// CRC-16/X.25 - the checksum SML appends to every telegram, reused for our own
// LoRa packets. Reflected: poly 0x1021 expressed as 0x8408, init 0xFFFF,
// final XOR 0xFFFF.

#include <cstddef>
#include <cstdint>

namespace core {

inline uint16_t crc16X25(const uint8_t* data, size_t len, uint16_t crc = 0xFFFF) {
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? static_cast<uint16_t>((crc >> 1) ^ 0x8408u)
                       : static_cast<uint16_t>(crc >> 1);
    }
  }
  return crc;
}

inline uint16_t crc16X25Finish(uint16_t crc) {
  return static_cast<uint16_t>(crc ^ 0xFFFFu);
}

}  // namespace core
