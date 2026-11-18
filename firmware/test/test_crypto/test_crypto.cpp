#include "../unity_case.h"
// Link crypto tests.
//
// Every expected value below was produced by an independent implementation -
// Python's hashlib/hmac plus the `cryptography` package, which is OpenSSL
// underneath - so these tests compare the C++ against something that was not
// written from the same understanding of the algorithms. The published
// constants (SHA-256's K/IV) are generated from the primes in crypto.cpp rather
// than transcribed, for the same reason.

#include <unity.h>

#include <cstdio>
#include <cstring>

#include "crypto.h"

using namespace core;

namespace {

uint8_t nibble(char c) {
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
  return static_cast<uint8_t>(c - 'A' + 10);
}

void fromHex(const char* hex, uint8_t* out, size_t outLen) {
  for (size_t i = 0; i < outLen; ++i) {
    out[i] = static_cast<uint8_t>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
  }
}

bool equalsHex(const uint8_t* data, size_t len, const char* hex) {
  for (size_t i = 0; i < len; ++i) {
    const uint8_t want = static_cast<uint8_t>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
    if (data[i] != want) return false;
  }
  return true;
}

// The frame used throughout: the real one from this installation, Keller ->
// Wohnung, 13600.5433 kWh and 902 W.
const char* kHeaderHex = "a50204001faf141e3845d2044523010017";
const char* kPayloadHex = "0f39471b0800000000000000008603000055001e011202";
const char* kNonceHex = "141e384545230100d2040400";
const char* kSecretHex = "0011223344556677";
const char* kMasterHex =
    "2930e7fd5435d292992a979112d51f48227011b94e46cb80b2ec28c6bbcd0be1";
const char* kSealedPayloadHex = "6e0a39ec214e6f00e1668ff34d50a0121fa2d8a6489438";
const char* kTagHex = "2a812516c802";

}  // namespace

void setUp() {}
void tearDown() {}

TEST_CASE("sha256 matches the reference implementation") {
  uint8_t out[32];

  sha256(reinterpret_cast<const uint8_t*>(""), 0, out);
  TEST_ASSERT_TRUE(equalsHex(out, 32,
                             "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

  sha256(reinterpret_cast<const uint8_t*>("abc"), 3, out);
  TEST_ASSERT_TRUE(equalsHex(out, 32,
                             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

  uint8_t thousand[1000];
  memset(thousand, 'a', sizeof(thousand));
  sha256(thousand, sizeof(thousand), out);
  TEST_ASSERT_TRUE(equalsHex(out, 32,
                             "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));

  // 41 bytes: crosses the padding boundary, where an implementation that gets
  // the length encoding wrong fails and short inputs still pass.
  uint8_t longInput[41];
  fromHex("6173646667686a6b796943486f6e6b6a66000000000000000000000000000000000000000000000000",
          longInput, sizeof(longInput));
  sha256(longInput, sizeof(longInput), out);
  TEST_ASSERT_TRUE(equalsHex(out, 32,
                             "e849b2f19b7af058e05dc84b9a3f4d6b7fe804008a8820ec5610cf2ffcc1feb7"));
}

TEST_CASE("hmac-sha256 matches the reference implementation") {
  uint8_t out[32];
  hmacSha256(reinterpret_cast<const uint8_t*>("x"), 1, reinterpret_cast<const uint8_t*>("y"), 1,
             out);
  TEST_ASSERT_TRUE(equalsHex(out, 32,
                             "1b56188fbdc65a885923886c8b7271332149050589d91803364521080cd0792d"));
}

TEST_CASE("aes-128 produces the keystream the reference implementation produces") {
  uint8_t key[16];
  uint8_t iv[16];
  uint8_t data[16];
  fromHex("000102030405060708090a0b0c0d0e0f", key, sizeof(key));
  memset(iv, 0, sizeof(iv));
  memset(data, 0, sizeof(data));

  aes128CtrXor(key, iv, data, sizeof(data));
  TEST_ASSERT_TRUE(equalsHex(data, sizeof(data), "c6a13b37878f5b826f4f8162a1c8d879"));
}

TEST_CASE("the link key both ends derive from the same secret") {
  uint8_t secret[kLinkSecretSize];
  uint8_t key[kLinkKeySize];
  fromHex(kSecretHex, secret, sizeof(secret));

  deriveLinkKey(secret, 0xAF1F, key);
  TEST_ASSERT_TRUE(equalsHex(key, kLinkKeySize, kMasterHex));

  // A different net id is a different key: two installations sharing a secret by
  // accident must not share a keystream.
  uint8_t other[kLinkKeySize];
  deriveLinkKey(secret, 0xAF20, other);
  TEST_ASSERT_FALSE(memcmp(key, other, kLinkKeySize) == 0);
}

TEST_CASE("the sealed frame matches byte for byte") {
  uint8_t key[kLinkKeySize];
  uint8_t nonce[kLinkNonceSize];
  uint8_t header[17];
  uint8_t payload[23];
  uint8_t tag[kLinkTagSize];
  fromHex(kMasterHex, key, sizeof(key));
  fromHex(kNonceHex, nonce, sizeof(nonce));
  fromHex(kHeaderHex, header, sizeof(header));
  fromHex(kPayloadHex, payload, sizeof(payload));

  linkSeal(key, nonce, header, sizeof(header), payload, sizeof(payload), tag);

  TEST_ASSERT_TRUE(equalsHex(payload, sizeof(payload), kSealedPayloadHex));
  TEST_ASSERT_TRUE(equalsHex(tag, sizeof(tag), kTagHex));

  // And it opens again with the same key, header and nonce.
  TEST_ASSERT_TRUE(linkOpen(key, nonce, header, sizeof(header), payload, sizeof(payload), tag));
  TEST_ASSERT_TRUE(equalsHex(payload, sizeof(payload), kPayloadHex));
}

TEST_CASE("a frame that was touched is refused") {
  uint8_t key[kLinkKeySize];
  uint8_t nonce[kLinkNonceSize];
  uint8_t header[17];
  uint8_t payload[23];
  uint8_t tag[kLinkTagSize];
  fromHex(kMasterHex, key, sizeof(key));
  fromHex(kNonceHex, nonce, sizeof(nonce));
  fromHex(kHeaderHex, header, sizeof(header));
  fromHex(kPayloadHex, payload, sizeof(payload));
  linkSeal(key, nonce, header, sizeof(header), payload, sizeof(payload), tag);

  uint8_t sealedPayload[23];
  memcpy(sealedPayload, payload, sizeof(sealedPayload));

  // A different secret: the wrong end of the link, or a neighbour guessing.
  uint8_t wrong[kLinkKeySize];
  memcpy(wrong, key, sizeof(wrong));
  wrong[0] ^= 0x01;
  TEST_ASSERT_FALSE(linkOpen(wrong, nonce, header, sizeof(header), payload, sizeof(payload), tag));

  // A changed byte of ciphertext, tag left as it was.
  memcpy(payload, sealedPayload, sizeof(payload));
  payload[7] ^= 0x40;
  TEST_ASSERT_FALSE(linkOpen(key, nonce, header, sizeof(header), payload, sizeof(payload), tag));

  // A replayed frame with a different destination in the clear header.
  memcpy(payload, sealedPayload, sizeof(payload));
  header[8] ^= 0x02;
  TEST_ASSERT_FALSE(linkOpen(key, nonce, header, sizeof(header), payload, sizeof(payload), tag));

  // A truncated or flipped tag.
  header[8] ^= 0x02;
  uint8_t badTag[kLinkTagSize];
  memcpy(badTag, tag, sizeof(badTag));
  badTag[5] ^= 0x80;
  TEST_ASSERT_FALSE(linkOpen(key, nonce, header, sizeof(header), payload, sizeof(payload), badTag));

  // A refused frame leaves the payload alone, so nothing downstream can read
  // half-decrypted numbers.
  TEST_ASSERT_TRUE(equalsHex(payload, sizeof(payload), kSealedPayloadHex));
}
