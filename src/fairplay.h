#ifndef FAIRPLAY_H
#define FAIRPLAY_H

#include <cstdint>

// FairPlay SAP handshake wrapper (built on vendored playfair).
// Usage per connection:
//   1. fp-setup phase 1: Setup(req[16]) -> res[142]
//   2. fp-setup phase 2: Handshake(req[164]) -> res[32]
//   3. SETUP ekey unwrap: Decrypt(ekey[72]) -> aeskey[16]
class FairPlay {
public:
    int Setup(const uint8_t req[16], uint8_t res[142]);
    int Handshake(const uint8_t req[164], uint8_t res[32]);
    int Decrypt(const uint8_t input[72], uint8_t output[16]);

private:
    uint8_t m_keymsg[164] = {};
    unsigned int m_keymsgLen = 0;
};

#endif // FAIRPLAY_H
