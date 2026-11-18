#include "crypto.h"

#include <cstdlib>
#include <cstring>

namespace core {
namespace {

// ── SHA-256 ──────────────────────────────────────────────────────────────────
// The constants are the fractional parts of the cube roots (K) and square roots
// (IV) of the first primes, which is how they are defined in FIPS 180-4.
constexpr uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr uint32_t kSha256Iv[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

inline uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }
inline uint32_t rotl8(uint8_t x, unsigned n) {
  return static_cast<uint8_t>((x << n) | (x >> (8u - n)));
}

inline uint32_t loadBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline void storeBe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

inline uint32_t loadLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline void storeLe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

void sha256Block(uint32_t h[8], const uint8_t* block) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) w[i] = loadBe32(block + 4 * i);
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = hh + s1 + ch + kSha256K[i] + w[i];
    const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

// ── AES-128 ──────────────────────────────────────────────────────────────────
// GF(2^8) with the AES polynomial x^8 + x^4 + x^3 + x + 1 (0x11B).
inline uint8_t gmul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  for (int i = 0; i < 8; ++i) {
    if (b & 1) r ^= a;
    const bool carry = (a & 0x80) != 0;
    a = static_cast<uint8_t>(a << 1);
    if (carry) a ^= 0x1B;
    b = static_cast<uint8_t>(b >> 1);
  }
  return r;
}

// The S-box as defined, not as a table: the multiplicative inverse in GF(2^8)
// followed by the affine transform. Generating it removes a 256-byte constant
// that could be transcribed incorrectly and never noticed.
const uint8_t* sbox() {
  static uint8_t table[256];
  static bool ready = false;
  if (!ready) {
    for (int x = 0; x < 256; ++x) {
      uint8_t inv = 0;
      if (x != 0) {
        for (int y = 1; y < 256; ++y) {
          if (gmul(static_cast<uint8_t>(x), static_cast<uint8_t>(y)) == 1) {
            inv = static_cast<uint8_t>(y);
            break;
          }
        }
      }
      table[x] = static_cast<uint8_t>(inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^ rotl8(inv, 3) ^
                                      rotl8(inv, 4) ^ 0x63);
    }
    ready = true;
  }
  return table;
}

struct Aes128 {
  uint8_t rk[11][16];
};

void aesExpandKey(Aes128& a, const uint8_t key[16]) {
  memcpy(a.rk[0], key, 16);
  const uint8_t* sb = sbox();
  uint8_t rcon = 1;
  for (int round = 1; round <= 10; ++round) {
    uint8_t t[4] = {a.rk[round - 1][12], a.rk[round - 1][13], a.rk[round - 1][14],
                    a.rk[round - 1][15]};
    const uint8_t first = t[0];
    t[0] = t[1];
    t[1] = t[2];
    t[2] = t[3];
    t[3] = first;
    for (int i = 0; i < 4; ++i) t[i] = sb[t[i]];
    t[0] ^= rcon;
    rcon = gmul(rcon, 2);
    for (int i = 0; i < 4; ++i) a.rk[round][i] = a.rk[round - 1][i] ^ t[i];
    for (int i = 4; i < 16; ++i) {
      a.rk[round][i] = static_cast<uint8_t>(a.rk[round - 1][i] ^ a.rk[round][i - 4]);
    }
  }
}

void aesEncryptBlock(const Aes128& a, const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16];
  for (int i = 0; i < 16; ++i) s[i] = static_cast<uint8_t>(in[i] ^ a.rk[0][i]);
  const uint8_t* sb = sbox();

  for (int round = 1; round <= 10; ++round) {
    for (int i = 0; i < 16; ++i) s[i] = sb[s[i]];

    // State is column-major: s[4 * column + row]. ShiftRows shifts row r left by r.
    uint8_t t[16];
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) t[4 * c + r] = s[4 * ((c + r) % 4) + r];
    }
    memcpy(s, t, 16);

    if (round != 10) {
      for (int c = 0; c < 4; ++c) {
        const uint8_t a0 = s[4 * c + 0], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
        const uint8_t x0 = gmul(a0, 2), x1 = gmul(a1, 2), x2 = gmul(a2, 2), x3 = gmul(a3, 2);
        s[4 * c + 0] = static_cast<uint8_t>(x0 ^ (x1 ^ a1) ^ a2 ^ a3);
        s[4 * c + 1] = static_cast<uint8_t>(a0 ^ x1 ^ (x2 ^ a2) ^ a3);
        s[4 * c + 2] = static_cast<uint8_t>(a0 ^ a1 ^ x2 ^ (x3 ^ a3));
        s[4 * c + 3] = static_cast<uint8_t>((x0 ^ a0) ^ a1 ^ a2 ^ x3);
      }
    }

    for (int i = 0; i < 16; ++i) s[i] = static_cast<uint8_t>(s[i] ^ a.rk[round][i]);
  }
  memcpy(out, s, 16);
}

// The encryption subkey and the authentication subkey are derived apart rather
// than taken as two halves of one hash output. Both come from the same secret,
// so a wrong secret changes both either way - but separating them means no
// single byte of key material can leave a frame that authenticates and still
// decrypts to rubbish.
void splitLinkKey(const uint8_t key[kLinkKeySize], uint8_t encKey[16], uint8_t macKey[32]) {
  memcpy(encKey, key, 16);
  uint8_t buf[3 + kLinkKeySize];
  memcpy(buf, "mac", 3);
  memcpy(buf + 3, key, kLinkKeySize);
  sha256(buf, sizeof(buf), macKey);
}

// XORs the tag comparison result so the timing does not leak how far it matched.
bool tagEquals(const uint8_t* a, const uint8_t* b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; ++i) diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
  return diff == 0;
}

}  // namespace

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  uint32_t h[8];
  memcpy(h, kSha256Iv, sizeof(h));

  uint8_t block[64];
  size_t offset = 0;
  while (len - offset >= 64) {
    sha256Block(h, data + offset);
    offset += 64;
  }

  const size_t rest = len - offset;
  memset(block, 0, sizeof(block));
  if (rest) memcpy(block, data + offset, rest);
  block[rest] = 0x80;
  if (rest >= 56) {
    sha256Block(h, block);
    memset(block, 0, sizeof(block));
  }
  const uint64_t bits = static_cast<uint64_t>(len) * 8u;
  for (int i = 0; i < 8; ++i) block[63 - i] = static_cast<uint8_t>(bits >> (8 * i));
  sha256Block(h, block);

  for (int i = 0; i < 8; ++i) storeBe32(out + 4 * i, h[i]);
}

void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen,
                uint8_t out[32]) {
  uint8_t k[64];
  memset(k, 0, sizeof(k));
  if (keyLen > sizeof(k)) {
    sha256(key, keyLen, k);
  } else {
    memcpy(k, key, keyLen);
  }

  uint8_t ipad[64];
  uint8_t opad[64];
  for (int i = 0; i < 64; ++i) {
    ipad[i] = static_cast<uint8_t>(k[i] ^ 0x36);
    opad[i] = static_cast<uint8_t>(k[i] ^ 0x5c);
  }

  // inner = SHA256(ipad || msg)
  uint8_t* inner = static_cast<uint8_t*>(malloc(64 + msgLen));
  if (inner == nullptr) return;
  memcpy(inner, ipad, 64);
  if (msgLen) memcpy(inner + 64, msg, msgLen);
  uint8_t innerHash[32];
  sha256(inner, 64 + msgLen, innerHash);
  free(inner);

  uint8_t outer[96];
  memcpy(outer, opad, 64);
  memcpy(outer + 64, innerHash, 32);
  sha256(outer, sizeof(outer), out);
}

void aes128CtrXor(const uint8_t key[16], const uint8_t iv[16], uint8_t* data, size_t len) {
  Aes128 a;
  aesExpandKey(a, key);

  uint8_t counter[16];
  memcpy(counter, iv, 16);
  uint8_t stream[16];
  for (size_t off = 0; off < len; off += 16) {
    aesEncryptBlock(a, counter, stream);
    const size_t n = (len - off < 16) ? (len - off) : 16;
    for (size_t i = 0; i < n; ++i) data[off + i] = static_cast<uint8_t>(data[off + i] ^ stream[i]);
    for (int i = 15; i >= 0; --i) {
      if (++counter[i] != 0) break;
    }
  }
}

void deriveLinkKey(const uint8_t secret[kLinkSecretSize], uint16_t netId,
                   uint8_t out[kLinkKeySize]) {
  static const char kSalt[] = "lilygopower-link-v1";
  uint8_t buf[sizeof(kSalt) - 1 + kLinkSecretSize + 2];
  size_t i = 0;
  memcpy(buf, kSalt, sizeof(kSalt) - 1);
  i += sizeof(kSalt) - 1;
  memcpy(buf + i, secret, kLinkSecretSize);
  i += kLinkSecretSize;
  buf[i++] = static_cast<uint8_t>(netId & 0xFF);
  buf[i++] = static_cast<uint8_t>(netId >> 8);
  sha256(buf, i, out);
}

void linkSeal(const uint8_t key[kLinkKeySize], const uint8_t nonce[kLinkNonceSize],
              const uint8_t* aad, size_t aadLen, uint8_t* payload, size_t payloadLen,
              uint8_t tag[kLinkTagSize]) {
  uint8_t encKey[16];
  uint8_t macKey[32];
  splitLinkKey(key, encKey, macKey);

  uint8_t iv[16];
  memset(iv, 0, sizeof(iv));
  memcpy(iv, nonce, kLinkNonceSize);
  aes128CtrXor(encKey, iv, payload, payloadLen);

  // Tag over header and ciphertext together: the header travels in the clear, so
  // without this its fields could be edited in flight.
  uint8_t mac[32];
  uint8_t* buf = static_cast<uint8_t*>(malloc(aadLen + payloadLen));
  if (buf != nullptr) {
    if (aadLen) memcpy(buf, aad, aadLen);
    if (payloadLen) memcpy(buf + aadLen, payload, payloadLen);
    hmacSha256(macKey, sizeof(macKey), buf, aadLen + payloadLen, mac);
    free(buf);
  } else {
    // Without a buffer there is no tag; callers see a mismatch and drop the
    // frame rather than trusting it.
    memset(mac, 0, sizeof(mac));
  }
  memcpy(tag, mac, kLinkTagSize);
}

bool linkOpen(const uint8_t key[kLinkKeySize], const uint8_t nonce[kLinkNonceSize],
              const uint8_t* aad, size_t aadLen, uint8_t* payload, size_t payloadLen,
              const uint8_t tag[kLinkTagSize]) {
  uint8_t encKey[16];
  uint8_t macKey[32];
  splitLinkKey(key, encKey, macKey);

  uint8_t mac[32];
  uint8_t* buf = static_cast<uint8_t*>(malloc(aadLen + payloadLen));
  if (buf == nullptr) return false;
  if (aadLen) memcpy(buf, aad, aadLen);
  if (payloadLen) memcpy(buf + aadLen, payload, payloadLen);
  hmacSha256(macKey, sizeof(macKey), buf, aadLen + payloadLen, mac);
  free(buf);

  if (!tagEquals(mac, tag, kLinkTagSize)) return false;

  uint8_t iv[16];
  memset(iv, 0, sizeof(iv));
  memcpy(iv, nonce, kLinkNonceSize);
  aes128CtrXor(encKey, iv, payload, payloadLen);
  return true;
}

}  // namespace core
