#ifndef SHA512_H
#define SHA512_H

#include <cstdint>
#include <cstddef>

// Minimal SHA-512 (FIPS 180-4)
class SHA512 {
public:
    SHA512();
    void Update(const uint8_t* data, size_t len);
    void Final(uint8_t out[64]);

private:
    void ProcessBlock(const uint8_t* block);

    uint64_t m_h[8];
    uint8_t m_buf[128];
    size_t m_bufLen = 0;
    uint64_t m_totalLen = 0; // bytes
};

// One-shot helper
void SHA512Hash(const uint8_t* data, size_t len, uint8_t out[64]);

#endif // SHA512_H
