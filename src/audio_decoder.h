#ifndef AUDIO_DECODER_H
#define AUDIO_DECODER_H

#include <cstdint>
#include <cstddef>
#include <vector>

// AAC-ELD decoder (AirPlay mirroring audio: ct=8, 44100 Hz stereo, spf=480),
// backed by FFmpeg libavcodec (GPL-2.0-or-later / LGPL-2.1-or-later).
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
    void* m_codec = nullptr; // const AVCodec*
    void* m_ctx = nullptr;   // AVCodecContext*
    void* m_frame = nullptr; // AVFrame*
    void* m_pkt = nullptr;   // AVPacket*
};

#endif // AUDIO_DECODER_H
