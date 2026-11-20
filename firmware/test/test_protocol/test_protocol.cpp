#include "../unity_case.h"
// Packet codec tests: an over-the-air length field must never be trusted, and a
// corrupted frame must never be acted on.

#include <unity.h>

#include <cstring>

#include "meter_values.h"
#include "node_status.h"
#include "protocol.h"

using namespace core;

void setUp() {}
void tearDown() {}

namespace {

Packet makeDataPacket() {
  Packet packet;
  packet.type = PacketType::Data;
  packet.flags = 0x03;
  packet.netId = 0xBEEF;
  packet.src = 0x1234;
  packet.dst = 0x5678;
  packet.seq = 42;
  packet.uptimeS = 98765;

  MeterTelemetry telemetry;
  telemetry.flags = MeterTelemetry::kImport | MeterTelemetry::kPower;
  telemetry.importWh10 = 22167615;
  telemetry.powerW = -1234;
  packet.payloadLen = static_cast<uint8_t>(packTelemetry(telemetry, packet.payload, kMaxPayload));
  return packet;
}

}  // namespace

TEST_CASE("data packet round trips with its telemetry payload") {
  const Packet sent = makeDataPacket();
  uint8_t wire[64];
  const size_t len = encodePacket(sent, wire, sizeof(wire));
  TEST_ASSERT_EQUAL_UINT32(sent.wireSize(), len);
  TEST_ASSERT_EQUAL_UINT32(17 + MeterTelemetry::kWireSize + 2, len);

  Packet received;
  TEST_ASSERT_TRUE(decodePacket(wire, len, received));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketType::Data),
                          static_cast<uint8_t>(received.type));
  TEST_ASSERT_EQUAL_UINT16(0xBEEF, received.netId);
  TEST_ASSERT_EQUAL_UINT16(0x1234, received.src);
  TEST_ASSERT_EQUAL_UINT16(0x5678, received.dst);
  TEST_ASSERT_EQUAL_UINT16(42, received.seq);
  TEST_ASSERT_EQUAL_UINT32(98765, received.uptimeS);

  MeterTelemetry telemetry;
  TEST_ASSERT_TRUE(unpackTelemetry(received.payload, received.payloadLen, telemetry));
  TEST_ASSERT_EQUAL_UINT64(22167615, telemetry.importWh10);
  TEST_ASSERT_EQUAL_INT32(-1234, telemetry.powerW);
}

TEST_CASE("every packet type survives the codec") {
  for (uint8_t raw = 1; raw <= 8; ++raw) {
    Packet sent;
    sent.type = static_cast<PacketType>(raw);
    sent.seq = raw;
    uint8_t wire[64];
    const size_t len = encodePacket(sent, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_UINT32(17 + 2, len);

    Packet received;
    TEST_ASSERT_TRUE(decodePacket(wire, len, received));
    TEST_ASSERT_EQUAL_UINT8(raw, static_cast<uint8_t>(received.type));
  }
}

TEST_CASE("corrupted, truncated and foreign frames are rejected") {
  const Packet sent = makeDataPacket();
  uint8_t wire[64];
  const size_t len = encodePacket(sent, wire, sizeof(wire));

  Packet received;
  uint8_t copy[64];
  memcpy(copy, wire, len);
  copy[len - 1] ^= 0x40;  // CRC
  TEST_ASSERT_FALSE(decodePacket(copy, len, received));

  memcpy(copy, wire, len);
  copy[0] = 0x00;  // magic
  TEST_ASSERT_FALSE(decodePacket(copy, len, received));

  memcpy(copy, wire, len);
  copy[1] = kProtocolVersion + 1;
  TEST_ASSERT_FALSE(decodePacket(copy, len, received));

  memcpy(copy, wire, len);
  copy[16] = 0xFF;  // payload length beyond the buffer
  TEST_ASSERT_FALSE(decodePacket(copy, len, received));

  TEST_ASSERT_FALSE(decodePacket(wire, len - 3, received));
  TEST_ASSERT_FALSE(decodePacket(nullptr, 0, received));
}

TEST_CASE("encoding refuses a buffer that is too small") {
  const Packet sent = makeDataPacket();
  uint8_t wire[8];
  TEST_ASSERT_EQUAL_UINT32(0, encodePacket(sent, wire, sizeof(wire)));
}

TEST_CASE("short ids derive from the MAC") {
  const uint8_t mac[6] = {0x24, 0x0A, 0xC4, 0x12, 0xAB, 0xCD};
  TEST_ASSERT_EQUAL_UINT16(0xABCD, shortIdFromMac(mac));
}

TEST_CASE("a sealed frame round trips with the key and only with it") {
  const Packet sent = makeDataPacket();
  uint8_t key[kLinkKeySize];
  memset(key, 0x5A, sizeof(key));  // the real vectors live in test_crypto

  uint8_t plainWire[64];
  uint8_t wire[64];
  const size_t plainLen = encodePacket(sent, plainWire, sizeof(plainWire));
  const size_t sealedLen = encodePacket(sent, wire, sizeof(wire), key);

  // The 6-byte tag replaces the 2-byte CRC: four bytes more on the wire, and the
  // frame says in its flags that it is sealed.
  TEST_ASSERT_EQUAL_UINT32(plainLen + 4, sealedLen);
  TEST_ASSERT_TRUE((wire[3] & kFlagEncrypted) != 0);

  // The payload left the device as ciphertext, not as the numbers themselves.
  TEST_ASSERT_TRUE(memcmp(plainWire + kHeaderSize, wire + kHeaderSize, sent.payloadLen) != 0);

  Packet received;
  TEST_ASSERT_TRUE(decodePacket(wire, sealedLen, received, key));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketType::Data),
                          static_cast<uint8_t>(received.type));
  TEST_ASSERT_EQUAL_UINT16(sent.seq, received.seq);
  TEST_ASSERT_EQUAL_UINT32(sent.uptimeS, received.uptimeS);
  TEST_ASSERT_EQUAL_UINT8(sent.payloadLen, received.payloadLen);
  TEST_ASSERT_EQUAL_MEMORY(sent.payload, received.payload, sent.payloadLen);

  // Without a key nothing about a sealed frame can be verified.
  TEST_ASSERT_FALSE(decodePacket(wire, sealedLen, received, nullptr));

  // A different secret is refused by the tag, not tolerated as noise.
  uint8_t wrong[kLinkKeySize];
  memcpy(wrong, key, sizeof(wrong));
  wrong[7] ^= 0x11;
  TEST_ASSERT_FALSE(decodePacket(wire, sealedLen, received, wrong));

  uint8_t copy[64];
  memcpy(copy, wire, sealedLen);
  copy[kHeaderSize + 3] ^= 0x08;  // a flipped byte of ciphertext
  TEST_ASSERT_FALSE(decodePacket(copy, sealedLen, received, key));

  memcpy(copy, wire, sealedLen);
  copy[8] ^= 0x02;  // a changed destination in the clear header
  TEST_ASSERT_FALSE(decodePacket(copy, sealedLen, received, key));

  memcpy(copy, wire, sealedLen);
  copy[10] ^= 0x01;  // a replayed-looking sequence number
  TEST_ASSERT_FALSE(decodePacket(copy, sealedLen, received, key));

  TEST_ASSERT_FALSE(decodePacket(wire, sealedLen - 1, received, key));  // tag cut short
}

TEST_CASE("a plaintext frame still decodes when a key is configured") {
  // Pairing and diagnostics travel in the clear. That a *reading* must be sealed
  // is the application's rule, and radio_link enforces it.
  const Packet sent = makeDataPacket();
  uint8_t key[kLinkKeySize];
  memset(key, 0x5A, sizeof(key));

  uint8_t wire[64];
  const size_t len = encodePacket(sent, wire, sizeof(wire));

  Packet received;
  TEST_ASSERT_TRUE(decodePacket(wire, len, received, key));
  TEST_ASSERT_FALSE((received.flags & kFlagEncrypted) != 0);
  TEST_ASSERT_EQUAL_MEMORY(sent.payload, received.payload, sent.payloadLen);
}

TEST_CASE("the node status report survives the wire") {
  NodeStatus in;
  in.airtimeUsedDs = 213;  // 21.3 s
  in.txBlocked = 2;
  in.heapFreeKb = 141;
  in.heapMinKb = 118;
  in.rssi = -97;
  in.snrQuarterDb = -14;  // -3.5 dB
  in.telegrams = 123456;
  in.telegramErrors = 7;
  in.telegramAgeS = 1;
  in.meterFlags = NodeStatus::kMeterResolved | NodeStatus::kMeterSml;
  in.rxPin = 39;
  in.bytesSeen = 78901234;

  uint8_t wire[NodeStatus::kWireSize + 4];
  TEST_ASSERT_EQUAL_UINT32(25, NodeStatus::kWireSize);
  TEST_ASSERT_EQUAL_UINT32(NodeStatus::kWireSize, packNodeStatus(in, wire, sizeof(wire)));
  // Fits a sealed frame with room to spare.
  TEST_ASSERT_TRUE(NodeStatus::kWireSize <= kMaxPayload);

  NodeStatus out;
  TEST_ASSERT_TRUE(unpackNodeStatus(wire, NodeStatus::kWireSize, out));
  TEST_ASSERT_EQUAL_UINT16(213, out.airtimeUsedDs);
  TEST_ASSERT_EQUAL_UINT16(2, out.txBlocked);
  TEST_ASSERT_EQUAL_UINT16(141, out.heapFreeKb);
  TEST_ASSERT_EQUAL_UINT16(118, out.heapMinKb);
  TEST_ASSERT_EQUAL_INT8(-97, out.rssi);
  TEST_ASSERT_EQUAL_INT8(-14, out.snrQuarterDb);
  TEST_ASSERT_EQUAL_UINT32(123456, out.telegrams);
  TEST_ASSERT_EQUAL_UINT16(7, out.telegramErrors);
  TEST_ASSERT_EQUAL_UINT16(1, out.telegramAgeS);
  TEST_ASSERT_EQUAL_UINT8(NodeStatus::kMeterResolved | NodeStatus::kMeterSml, out.meterFlags);
  TEST_ASSERT_EQUAL_UINT8(39, out.rxPin);
  TEST_ASSERT_EQUAL_UINT32(78901234, out.bytesSeen);

  // Too short, or a layout version this build does not know: refused.
  TEST_ASSERT_FALSE(unpackNodeStatus(wire, NodeStatus::kWireSize - 1, out));
  wire[0] = NodeStatus::kVersion + 1;
  TEST_ASSERT_FALSE(unpackNodeStatus(wire, NodeStatus::kWireSize, out));
  // Counters that do not fit two bytes saturate instead of wrapping.
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, saturate16(70000));
}
