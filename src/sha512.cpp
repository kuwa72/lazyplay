#include "sha512.h"

#include <cstring>

namespace {

const uint64_t K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

inline uint64_t RotR(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
inline uint64_t Ch(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (~x & z); }
inline uint64_t Maj(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (x & z) ^ (y & z); }
inline uint64_t BS0(uint64_t x) { return RotR(x, 28) ^ RotR(x, 34) ^ RotR(x, 39); }
inline uint64_t BS1(uint64_t x) { return RotR(x, 14) ^ RotR(x, 18) ^ RotR(x, 41); }
inline uint64_t SS0(uint64_t x) { return RotR(x, 1) ^ RotR(x, 8) ^ (x >> 7); }
inline uint64_t SS1(uint64_t x) { return RotR(x, 19) ^ RotR(x, 61) ^ (x >> 6); }

inline uint64_t LoadBE64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

inline void StoreBE64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) { p[i] = static_cast<uint8_t>(v & 0xFF); v >>= 8; }
}

} // namespace

SHA512::SHA512() {
    m_h[0] = 0x6a09e667f3bcc908ULL; m_h[1] = 0xbb67ae8584caa73bULL;
    m_h[2] = 0x3c6ef372fe94f82bULL; m_h[3] = 0xa54ff53a5f1d36f1ULL;
    m_h[4] = 0x510e527fade682d1ULL; m_h[5] = 0x9b05688c2b3e6c1fULL;
    m_h[6] = 0x1f83d9abfb41bd6bULL; m_h[7] = 0x5be0cd19137e2179ULL;
    m_bufLen = 0;
    m_totalLen = 0;
}

void SHA512::ProcessBlock(const uint8_t* block) {
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = LoadBE64(block + i * 8);
    for (int i = 16; i < 80; ++i) w[i] = SS1(w[i - 2]) + w[i - 7] + SS0(w[i - 15]) + w[i - 16];

    uint64_t a = m_h[0], b = m_h[1], c = m_h[2], d = m_h[3];
    uint64_t e = m_h[4], f = m_h[5], g = m_h[6], h = m_h[7];

    for (int i = 0; i < 80; ++i) {
        uint64_t t1 = h + BS1(e) + Ch(e, f, g) + K[i] + w[i];
        uint64_t t2 = BS0(a) + Maj(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    m_h[0] += a; m_h[1] += b; m_h[2] += c; m_h[3] += d;
    m_h[4] += e; m_h[5] += f; m_h[6] += g; m_h[7] += h;
}

void SHA512::Update(const uint8_t* data, size_t len) {
    m_totalLen += len;
    while (len > 0) {
        size_t take = 128 - m_bufLen;
        if (take > len) take = len;
        memcpy(m_buf + m_bufLen, data, take);
        m_bufLen += take;
        data += take;
        len -= take;
        if (m_bufLen == 128) {
            ProcessBlock(m_buf);
            m_bufLen = 0;
        }
    }
}

void SHA512::Final(uint8_t out[64]) {
    uint64_t bitLen = m_totalLen * 8;
    uint8_t pad = 0x80;
    Update(&pad, 1);
    uint8_t zero = 0;
    while (m_bufLen != 112) Update(&zero, 1);
    uint8_t lenBytes[16] = {};
    StoreBE64(lenBytes + 8, bitLen);
    Update(lenBytes, 16);
    for (int i = 0; i < 8; ++i) StoreBE64(out + i * 8, m_h[i]);
}

void SHA512Hash(const uint8_t* data, size_t len, uint8_t out[64]) {
    SHA512 ctx;
    ctx.Update(data, len);
    ctx.Final(out);
}
