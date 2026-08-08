#ifndef AES_CBC_H
#define AES_CBC_H

#include <cstdint>
#include <cstddef>

// AES-128-CBC decryption in the AirPlay audio shape: each packet is
// independent (the IV resets per packet), only complete 16-byte blocks are
// encrypted, and any trailing partial block is plaintext.
class AesCbc {
public:
    void InitDecrypt(const uint8_t key[16], const uint8_t iv[16]);
    void InitEncrypt(const uint8_t key[16], const uint8_t iv[16]); // tests/tools
    void DecryptPacket(const uint8_t* in, uint8_t* out, size_t len);
    void EncryptPacket(const uint8_t* in, uint8_t* out, size_t len);

private:
    void DecryptBlock(const uint8_t in[16], uint8_t out[16]);
    void EncryptBlockFwd(const uint8_t in[16], uint8_t out[16]);

    uint32_t m_roundKey[44] = {};
    uint8_t m_iv[16] = {};
    bool m_initialized = false;
};

#endif // AES_CBC_H
