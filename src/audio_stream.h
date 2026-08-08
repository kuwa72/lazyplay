#ifndef AUDIO_STREAM_H
#define AUDIO_STREAM_H

#include <winsock2.h>
#include <cstdint>
#include <thread>
#include <atomic>
#include <vector>

#include "aes_cbc.h"
#include "audio_decoder.h"
#include "audio_wasapi.h"

// AirPlay mirror audio receiver (stream type 96): RTP/UDP, AES-128-CBC
// (per-packet IV reset, partial tail plaintext), AAC-ELD frames decoded via
// FFmpeg libavcodec and played through WASAPI.
class AudioReceiver {
public:
    AudioReceiver();
    ~AudioReceiver();

    bool Start(uint16_t port, const uint8_t aesKey[16], const uint8_t aesIv[16]);
    void Stop();
    uint16_t GetPort() const { return m_port; }

    void SetVolumeDb(float db) { m_player.SetVolumeDb(db); }

private:
    void ThreadMain();

    uint16_t m_port = 0;
    SOCKET m_sock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::thread m_thread;

    AesCbc m_aes;
    AacEldDecoder m_decoder;
    WasapiPlayer m_player;

    // RTP dedup: AAC-ELD packets are retransmitted in a sliding pattern
    std::vector<uint16_t> m_seenSeq;
    size_t m_seenPos = 0;
    size_t m_seenFilled = 0;
    uint64_t m_validFrames = 0;
    uint64_t m_decodedFrames = 0;

    std::vector<uint8_t> m_decryptBuf;
};

#endif // AUDIO_STREAM_H
