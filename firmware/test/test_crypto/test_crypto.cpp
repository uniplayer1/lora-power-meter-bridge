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
const char* kSecretHex = "00112233445566778899aabbccddeeff";
// sha256("lilygopower-link-v1" || secret || net id 0xAF1F little-endian).
const char* kDerivedHex =
    "7118553cdd1db7ab4166b474f632d1853d1a33c26d8b3579f5d19cfeef89fbab";

// X25519: RFC 7748 section 6.1 (Alice = initiator, Bob = responder). The
// pairing secret and check code were computed with Python's hmac/hashlib over
// the shared value from the `cryptography` package.
const char* kAlicePrivHex = "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
const char* kAlicePubHex = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
const char* kBobPrivHex = "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
const char* kBobPubHex = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
const char* kSharedHex = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";
const char* kPairSecretHex = "739f0fceab8947130123401ae2de26fd";
const uint16_t kCheckCode = 5642;
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
  TEST_ASSERT_TRUE(equalsHex(key, kLinkKeySize, kDerivedHex));

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

TEST_CASE("x25519 matches RFC 7748") {
  uint8_t scalar[kX25519Size];
  uint8_t point[kX25519Size];
  uint8_t out[kX25519Size];
  // Section 5.2, first vector: an arbitrary point, not the base point.
  fromHex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, sizeof(scalar));
  fromHex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, sizeof(point));
  x25519(out, scalar, point);
  TEST_ASSERT_TRUE(equalsHex(out, sizeof(out), "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"));

  // Section 6.1: both public keys and the shared value.
  uint8_t alice[kX25519Size], bob[kX25519Size], alicePub[kX25519Size], bobPub[kX25519Size];
  fromHex(kAlicePrivHex, alice, sizeof(alice));
  fromHex(kBobPrivHex, bob, sizeof(bob));
  x25519Public(alicePub, alice);
  x25519Public(bobPub, bob);
  TEST_ASSERT_TRUE(equalsHex(alicePub, sizeof(alicePub), kAlicePubHex));
  TEST_ASSERT_TRUE(equalsHex(bobPub, sizeof(bobPub), kBobPubHex));

  uint8_t sharedA[kX25519Size], sharedB[kX25519Size];
  x25519(sharedA, alice, bobPub);
  x25519(sharedB, bob, alicePub);
  TEST_ASSERT_TRUE(equalsHex(sharedA, sizeof(sharedA), kSharedHex));
  TEST_ASSERT_EQUAL_MEMORY(sharedA, sharedB, kX25519Size);
}

TEST_CASE("both ends of a pairing derive the same link secret") {
  uint8_t shared[kX25519Size], alicePub[kX25519Size], bobPub[kX25519Size];
  fromHex(kSharedHex, shared, sizeof(shared));
  fromHex(kAlicePubHex, alicePub, sizeof(alicePub));
  fromHex(kBobPubHex, bobPub, sizeof(bobPub));

  uint8_t secret[kLinkSecretSize];
  TEST_ASSERT_TRUE(derivePairingSecret(shared, alicePub, bobPub, secret));
  TEST_ASSERT_TRUE(equalsHex(secret, sizeof(secret), kPairSecretHex));
  TEST_ASSERT_EQUAL_UINT16(kCheckCode, pairingCheckCode(secret));

  // The order of the public keys is part of the secret: swapped roles would
  // give a different one, so each side must know which role it played.
  uint8_t swapped[kLinkSecretSize];
  TEST_ASSERT_TRUE(derivePairingSecret(shared, bobPub, alicePub, swapped));
  TEST_ASSERT_FALSE(memcmp(secret, swapped, kLinkSecretSize) == 0);

  // A degenerate public key yields an all-zero shared value, which is refused.
  uint8_t zero[kX25519Size] = {0};
  TEST_ASSERT_FALSE(derivePairingSecret(zero, alicePub, bobPub, secret));
}
