#ifndef VIDEO_STREAM_H
#define VIDEO_STREAM_H

#include <winsock2.h>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

#include "aes_ctr.h"

// Receives the AirPlay mirroring video stream over the TCP data connection.
//
// Packet framing (per UxPlay/RPiPlay raop_rtp_mirror):
//   128-byte header: [0:3] payload size (LE32), [4] payload type,
//                    [5] flags (0x10 = IDR), [6:7] option, [8:15] NTP ts (LE64),
//                    [16:23] source width/height (LE float32) on type 0x01
//   payload: type 0x00 -> AES-128-CTR encrypted, length-prefixed NAL units
//            type 0x01 -> unencrypted SPS/PPS (prepended to the next IDR)
//            type 0x02 -> keepalive, type 0x05 -> streaming report (ignored)
typedef std::function<void(const uint8_t* data, size_t size)> VideoDataCallback;
typedef std::function<void(uint32_t width, uint32_t height)> VideoSizeCallback;

class MirrorVideoServer {
public:
    MirrorVideoServer();
    ~MirrorVideoServer();

    // Starts a TCP listener on an ephemeral port (0 = pick any free port).
    bool Start(uint16_t port = 0);
    void Stop();
    uint16_t GetPort() const { return m_port; }

    // (Re)initializes the AES-CTR keystream; called on every SETUP of a stream.
    void SetStreamKey(const uint8_t key[16], const uint8_t iv[16]);

    // Called (on the receiver thread) once per access unit, Annex-B formatted.
    void SetVideoDataCallback(VideoDataCallback cb) { m_dataCallback = cb; }
    // Called when the client reports the video dimensions.
    void SetVideoSizeCallback(VideoSizeCallback cb) { m_sizeCallback = cb; }

private:
    void ThreadMain();
    bool RecvAll(SOCKET sock, uint8_t* buf, size_t len);
    void HandlePacket(const uint8_t* header, const std::vector<uint8_t>& payload);
    void ResetStreamState();

    uint16_t m_port = 0;
    SOCKET m_listenSock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::thread m_thread;

    AesCtr m_aes;
    bool m_aesReady = false;

    std::vector<uint8_t> m_spsPps;      // Annex-B formatted, prepended to next IDR
    bool m_prependSpsPps = false;
    uint64_t m_spsPpsTimestamp = 0;

    // Reused per-packet work buffers (avoid allocator churn on weak CPUs)
    std::vector<uint8_t> m_decrypted;
    std::vector<uint8_t> m_annexB;

    // Diagnostics
    uint64_t m_vclPackets = 0;
    uint64_t m_vclBytes = 0;
    uint64_t m_framesToDecoder = 0;

    VideoDataCallback m_dataCallback;
    VideoSizeCallback m_sizeCallback;
};

#endif // VIDEO_STREAM_H
