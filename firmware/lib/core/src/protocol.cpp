#include "protocol.h"

#include <cstring>

#include "crc16.h"

namespace core {
namespace {

void putU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void putU32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

uint16_t getU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

void packetNonce(const Packet& packet, uint8_t out[kLinkNonceSize]) {
  size_t i = 0;
  putU16(out + i, packet.src);
  i += 2;
  putU16(out + i, packet.dst);
  i += 2;
  putU32(out + i, packet.uptimeS);
  i += 4;
  putU16(out + i, packet.seq);
  i += 2;
  out[i++] = static_cast<uint8_t>(packet.type);
  out[i++] = 0;
}

size_t encodePacket(const Packet& packet, uint8_t* out, size_t cap, const uint8_t* key) {
  if (out == nullptr) return 0;
  const size_t payloadLen = packet.payloadLen > kMaxPayload ? kMaxPayload : packet.payloadLen;
  const bool sealed = key != nullptr && payloadLen > 0;
  const size_t total = kHeaderSize + payloadLen + (sealed ? kLinkTagSize : kCrcSize);
  if (cap < total) return 0;

  out[0] = kPacketMagic;
  out[1] = kProtocolVersion;
  out[2] = static_cast<uint8_t>(packet.type);
  out[3] = sealed ? static_cast<uint8_t>(packet.flags | kFlagEncrypted) : packet.flags;
  putU16(out + 4, packet.netId);
  putU16(out + 6, packet.src);
  putU16(out + 8, packet.dst);
  putU16(out + 10, packet.seq);
  putU32(out + 12, packet.uptimeS);
  out[16] = static_cast<uint8_t>(payloadLen);
  if (payloadLen > 0) memcpy(out + kHeaderSize, packet.payload, payloadLen);

  if (sealed) {
    uint8_t nonce[kLinkNonceSize];
    packetNonce(packet, nonce);
    // The header is written first and passed as associated data, so the tag
    // covers the routing fields as well as the payload it just encrypted.
    linkSeal(key, nonce, out, kHeaderSize, out + kHeaderSize, payloadLen,
             out + kHeaderSize + payloadLen);
    return total;
  }

  const uint16_t crc = crc16X25Finish(crc16X25(out, kHeaderSize + payloadLen));
  out[kHeaderSize + payloadLen] = static_cast<uint8_t>(crc & 0xFF);
  out[kHeaderSize + payloadLen + 1] = static_cast<uint8_t>(crc >> 8);
  return total;
}

bool decodePacket(const uint8_t* in, size_t len, Packet& out, const uint8_t* key) {
  if (in == nullptr || len < kHeaderSize + kCrcSize) return false;
  if (in[0] != kPacketMagic || in[1] != kProtocolVersion) return false;

  const size_t payloadLen = in[16];
  if (payloadLen > kMaxPayload) return false;

  const bool sealed = (in[3] & kFlagEncrypted) != 0;
  const size_t total = kHeaderSize + payloadLen + (sealed ? kLinkTagSize : kCrcSize);
  if (len < total) return false;

  out.type = static_cast<PacketType>(in[2]);
  out.flags = in[3];
  out.netId = getU16(in + 4);
  out.src = getU16(in + 6);
  out.dst = getU16(in + 8);
  out.seq = getU16(in + 10);
  out.uptimeS = getU32(in + 12);
  out.payloadLen = static_cast<uint8_t>(payloadLen);

  if (sealed) {
    // Nothing about a sealed frame is trustworthy without the key: the tag has
    // to be checked before a single payload byte is looked at.
    if (key == nullptr) return false;
    if (payloadLen > 0) memcpy(out.payload, in + kHeaderSize, payloadLen);
    uint8_t nonce[kLinkNonceSize];
    packetNonce(out, nonce);
    return linkOpen(key, nonce, in, kHeaderSize, out.payload, payloadLen,
                    in + kHeaderSize + payloadLen);
  }

  const uint16_t expected = crc16X25Finish(crc16X25(in, kHeaderSize + payloadLen));
  const uint16_t actual = static_cast<uint16_t>(in[kHeaderSize + payloadLen] |
                                               (in[kHeaderSize + payloadLen + 1] << 8));
  if (expected != actual) return false;

  if (payloadLen > 0) memcpy(out.payload, in + kHeaderSize, payloadLen);
  return true;
}

}  // namespace core
