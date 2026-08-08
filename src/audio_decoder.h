#ifndef AUDIO_DECODER_H
#define AUDIO_DECODER_H

#include <cstdint>
#include <cstddef>
#include <vector>

// AAC-ELD decoder (AirPlay mirroring audio: ct=8, 44100 Hz stereo, spf=480),
// backed by the vendored Fraunhofer FDK decoder (src/fdk-aac).
class AacEldDecoder {
public:
    AacEldDecoder();
    ~AacEldDecoder();

    bool Init();
    void Shutdown();

    // Decode one raw AAC-ELD frame -> interleaved s16 PCM (44.1 kHz stereo).
    // Returns false when the frame did not decode.
    bool Decode(const uint8_t* frame, size_t len, std::vector<int16_t>& pcm);

private:
    void* m_dec = nullptr; // HANDLE_AACDECODER
};

#endif // AUDIO_DECODER_H
