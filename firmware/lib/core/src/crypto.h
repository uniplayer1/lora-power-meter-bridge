#pragma once
// Link crypto: AES-128-CTR for the payload, HMAC-SHA256 for integrity, both
// derived from one shared secret that never travels over the air. The secret is
// agreed during pairing with an X25519 key exchange: each side sends only its
// public key, and both compute the same secret from their own private key and
// the other's public key.
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

constexpr size_t kLinkSecretSize = 16;  // agreed by X25519 during pairing
constexpr size_t kX25519Size = 32;      // private key, public key, shared value
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

// X25519 (RFC 7748): out = scalar * point on Curve25519. The scalar is clamped
// here, so any 32 random bytes are a valid private key.
void x25519(uint8_t out[kX25519Size], const uint8_t scalar[kX25519Size],
            const uint8_t point[kX25519Size]);
// The public key for a private key: scalar * base point (u = 9).
void x25519Public(uint8_t out[kX25519Size], const uint8_t privateKey[kX25519Size]);

// The link secret both ends of a pairing arrive at: HMAC-SHA256 keyed with the
// X25519 shared value over both public keys, initiator first, truncated to
// kLinkSecretSize. Binding the public keys means each side derives the secret
// for exactly this exchange. Returns false for an all-zero shared value (the
// other side sent a degenerate public key).
bool derivePairingSecret(const uint8_t shared[kX25519Size], const uint8_t initiatorPub[kX25519Size],
                         const uint8_t responderPub[kX25519Size], uint8_t out[kLinkSecretSize]);

// A four-digit number derived from the link secret, shown on both devices after
// pairing. Equal numbers mean both ended up with the same secret, and nobody
// sat in the middle of the exchange.
uint16_t pairingCheckCode(const uint8_t secret[kLinkSecretSize]);

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
