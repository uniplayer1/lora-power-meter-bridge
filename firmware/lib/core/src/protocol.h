#pragma once
// LoRa link packet format, version 2.
//
// The chat firmware sent pipe-delimited ASCII. This link carries meter readings
// on a 1 % duty cycle, and airtime IS the legal budget, so the packet is a
// fixed-width binary frame with a CRC instead. A full telemetry frame is 42
// bytes on the wire - about 75 ms of airtime at SF7/BW125.

#include <cstddef>
#include <cstdint>

#include "crypto.h"

namespace core {

constexpr uint8_t kPacketMagic = 0xA5;
constexpr uint8_t kProtocolVersion = 2;
constexpr size_t kMaxPayload = 32;
constexpr size_t kHeaderSize = 17;  // magic .. payloadLen
constexpr size_t kCrcSize = 2;

// Set in Packet::flags when the payload is sealed. Such a frame ends in the
// authentication tag instead of the CRC, and the tag covers the cleartext header
// as well, so a changed destination or a replayed sequence number is refused.
//
// The top bit, not the bottom one: the low bits of this byte are used by tests
// and by future flags, and a frame that merely carries a low bit set must not
// look like a sealed one.
constexpr uint8_t kFlagEncrypted = 1u << 7;

enum class PacketType : uint8_t {
  Hello = 1,        // announce on the pairing channel
  PairRequest = 2,  // "anyone there?" broadcast carrying a nonce
  PairAck = 3,      // reply with the agreed net id
  Data = 4,         // meter telemetry payload (MeterTelemetry)
  Ack = 5,          // acknowledges a Data/Status sequence number
  Status = 6,       // link diagnostics, no meter payload
  Cmd = 7,          // gateway -> node command (set interval, resync clock)
  TimeSync = 8,     // gateway -> node wall clock
  // Sent back after PairAck. Without it only the side that *answered* learns
  // the other's id, and the initiator stays "unpaired" forever while packets
  // still flow - which looks exactly like a link that cannot be established.
  PairConfirm = 9,
};

struct Packet {
  PacketType type = PacketType::Status;
  uint8_t flags = 0;
  uint16_t netId = 0;
  uint16_t src = 0;
  uint16_t dst = 0;  // 0 = broadcast
  uint16_t seq = 0;
  uint32_t uptimeS = 0;
  uint8_t payloadLen = 0;
  uint8_t payload[kMaxPayload] = {0};

  size_t wireSize() const {
    return kHeaderSize + payloadLen + ((flags & kFlagEncrypted) ? kLinkTagSize : kCrcSize);
  }
};

// The per-frame nonce for the link cipher, built from header fields the receiver
// already has - so no nonce bytes travel. The sender randomises its sequence base
// at every boot, which is what keeps the nonce from repeating after a restart.
void packetNonce(const Packet& packet, uint8_t out[kLinkNonceSize]);

// Serialises into out. Returns the number of bytes written, or 0 when the
// buffer is too small (nothing partial is ever written).
//
// With a key the payload is sealed and the frame carries the tag instead of the
// CRC; without one it is the plaintext frame with a CRC that the pairing traffic
// and diagnostics still use.
size_t encodePacket(const Packet& packet, uint8_t* out, size_t cap,
                    const uint8_t* key = nullptr);

// Returns false on a short frame, a bad magic/version, a length overrun, a CRC
// mismatch - or a frame that claims to be sealed but has no key to check it
// against. Never reads past len.
bool decodePacket(const uint8_t* in, size_t len, Packet& out,
                  const uint8_t* key = nullptr);

// Short id derived from the MAC, so a packet fits two bytes per endpoint.
inline uint16_t shortIdFromMac(const uint8_t mac[6]) {
  return static_cast<uint16_t>((static_cast<uint16_t>(mac[4]) << 8) | mac[5]);
}

}  // namespace core
