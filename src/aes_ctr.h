#ifndef AES_CTR_H
#define AES_CTR_H

#include <cstdint>
#include <cstddef>

// AES-128-CTR with a continuous keystream across Process() calls.
// Counter semantics match OpenSSL EVP_aes_128_ctr: the 128-bit IV is
// treated as a big-endian counter incremented once per 16-byte block.
class AesCtr {
public:
    void Init(const uint8_t key[16], const uint8_t iv[16]);
    // Symmetric (CTR encryption == decryption)
    void Process(const uint8_t* in, uint8_t* out, size_t len);

private:
    void EncryptBlock(const uint8_t in[16], uint8_t out[16]);
    void NextKeystreamBlock();

    uint32_t m_roundKey[44] = {};
    uint8_t m_counter[16] = {};
    uint8_t m_keystream[16] = {};
    size_t m_ksOffset = 16; // 16 = need new block
    bool m_initialized = false;
};

#endif // AES_CTR_H
