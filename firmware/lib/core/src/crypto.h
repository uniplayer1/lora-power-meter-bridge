#pragma once
// Link crypto: AES-128-CTR for the payload, HMAC-SHA256 for integrity, both
// derived from one shared secret that never travels over the air.
//
// Deliberately self-contained and portable. The core library is compiled for the
// host tests as well as for the device, and a C++ implementation validated
// against vectors from an independent implementation is worth more here than a
// dependency that exists on only one of the two. The S-box is generated from
// GF(2^8) arithmetic rather than pasted in as 256 bytes, so there is nothing to
// transcribe wrongly.
//
// Design notes:
//  - Encrypt-then-MAC. The ciphertext is exactly as long as the plaintext; the
//    only bytes added are the tag.
//  - The packet header is authenticated but NOT encrypted (associated data): it
//    has to be read to route the packet, but a changed dst or a replayed seq
//    breaks the tag.
//  - The tag is truncated to 6 bytes: 2^-48 forgery per attempt, and it replaces
//    the CRC (which never proved authorship) so the frame grows by 4 bytes only.
//  - The per-frame nonce is built from fields in the header plus a randomised
//    sequence base per boot, so no keystream is ever reused.

#include <cstddef>
#include <cstdint>

namespace core {

constexpr size_t kLinkSecretSize = 8;   // generated on the node, typed in once
constexpr size_t kLinkKeySize = 32;     // 16 bytes AES + 16 bytes HMAC
constexpr size_t kLinkNonceSize = 12;   // src, dst, uptime, seq, type, spare
constexpr size_t kLinkTagSize = 6;      // truncated HMAC-SHA256

void sha256(const uint8_t* data, size_t len, uint8_t out[32]);
void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen,
                uint8_t out[32]);

// XORs `len` bytes of `data` with the AES-128-CTR keystream for (key, iv).
void aes128CtrXor(const uint8_t key[16], const uint8_t iv[16], uint8_t* data, size_t len);

// The key both ends derive from the shared secret and the net id. Deriving is
// all that happens: no key material is ever transmitted.
void deriveLinkKey(const uint8_t secret[kLinkSecretSize], uint16_t netId,
                   uint8_t out[kLinkKeySize]);

// Seals `payload` in place (same length) and writes the tag next to it.
void linkSeal(const uint8_t key[kLinkKeySize], const uint8_t nonce[kLinkNonceSize],
              const uint8_t* aad, size_t aadLen, uint8_t* payload, size_t payloadLen,
              uint8_t tag[kLinkTagSize]);

// Verifies the tag first and only then decrypts, so a forged frame is rejected
// rather than turned into numbers. Returns false when the tag does not match;
// `payload` is left untouched in that case.
bool linkOpen(const uint8_t key[kLinkKeySize], const uint8_t nonce[kLinkNonceSize],
              const uint8_t* aad, size_t aadLen, uint8_t* payload, size_t payloadLen,
              const uint8_t tag[kLinkTagSize]);

}  // namespace core
