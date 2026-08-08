#include "audio_stream.h"

#include <ws2tcpip.h>
#include <iostream>
#include <algorithm>

namespace {

// AAC-ELD frame markers after decryption (first byte)
bool IsEldFrameStart(uint8_t b) {
    return b == 0x8c || b == 0x8d || b == 0x8e ||   // modern clients
           b == 0x80 || b == 0x81 || b == 0x82;     // older iOS
}

} // namespace

AudioReceiver::AudioReceiver() {}
AudioReceiver::~AudioReceiver() { Stop(); }

bool AudioReceiver::Start(uint16_t port, const uint8_t aesKey[16], const uint8_t aesIv[16]) {
    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) return false;

    BOOL reuse = TRUE;
    setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(m_sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
        return false;
    }

    sockaddr_in bound = {};
    int boundLen = sizeof(bound);
    getsockname(m_sock, (struct sockaddr*)&bound, &boundLen);
    m_port = ntohs(bound.sin_port);

    int rcvBuf = 1024 * 1024;
    setsockopt(m_sock, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvBuf, sizeof(rcvBuf));
    DWORD recvTimeout = 500;
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeout, sizeof(recvTimeout));

    m_aes.InitDecrypt(aesKey, aesIv);
    if (!m_decoder.Init()) {
        std::cerr << "[Audio] AAC-ELD decoder init failed." << std::endl;
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
        return false;
    }
    if (!m_player.Start()) {
        std::cerr << "[Audio] WASAPI player failed to start." << std::endl;
    }

    m_seenSeq.assign(128, 0);
    m_seenPos = 0;
    m_seenFilled = 0;

    m_running = true;
    m_thread = std::thread(&AudioReceiver::ThreadMain, this);
    std::cout << "[Audio] AAC-ELD receiver on UDP port " << m_port << std::endl;
    return true;
}

void AudioReceiver::Stop() {
    if (m_running) {
        m_running = false;
        if (m_sock != INVALID_SOCKET) {
            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
        }
        if (m_thread.joinable()) m_thread.join();
    }
    m_decoder.Shutdown();
    m_player.Stop();
}

void AudioReceiver::ThreadMain() {
    uint8_t packet[2048];
    while (m_running) {
        int len = recvfrom(m_sock, (char*)packet, sizeof(packet), 0, nullptr, nullptr);
        if (len <= 0) continue;
        if (len < 12) continue;

        uint16_t seq = (static_cast<uint16_t>(packet[2]) << 8) | packet[3];
        const uint8_t* payload = packet + 12;
        size_t payloadLen = static_cast<size_t>(len) - 12;

        // Initial no-data marker packets carry a 4-byte marker payload
        if (payloadLen == 4 && payload[0] == 0x00 && payload[1] == 0x68 &&
            payload[2] == 0x34 && payload[3] == 0x00) {
            continue;
        }

        // Dedup retransmitted sequence numbers
        size_t valid = (m_seenFilled < m_seenSeq.size()) ? m_seenPos : m_seenSeq.size();
        bool seen = std::find(m_seenSeq.begin(), m_seenSeq.begin() + valid, seq) != m_seenSeq.begin() + valid;
        if (seen) continue;
        m_seenSeq[m_seenPos] = seq;
        m_seenPos = (m_seenPos + 1) % m_seenSeq.size();
        if (m_seenFilled < m_seenSeq.size()) m_seenFilled++;

        m_decryptBuf.resize(payloadLen);
        m_aes.DecryptPacket(payload, m_decryptBuf.data(), payloadLen);

        if (!IsEldFrameStart(m_decryptBuf[0])) {
            continue; // unknown/corrupt frame
        }

        m_validFrames++;

        std::vector<int16_t> pcm;
        if (m_decoder.Decode(m_decryptBuf.data(), payloadLen, pcm)) {
            // frameSize = samples per channel; stereo interleaved
            m_player.PushPcm(pcm.data(), pcm.size() / 2);
            m_decodedFrames++;
            int16_t maxS = 0;
            for (int16_t s : pcm) if (s > maxS || s < -maxS) maxS = s > 0 ? s : -s;
            if (m_decodedFrames <= 3 || m_decodedFrames % 100 == 0) {
                std::cout << "[Audio] decoded frame " << m_decodedFrames
                          << " size=" << pcm.size() << " maxS=" << maxS << std::endl;
            }
        } else {
            std::cout << "[Audio] frame decode FAILED at valid=" << m_validFrames << std::endl;
        }
        if (m_validFrames == 3 || m_validFrames % 100 == 0) {
            std::cout << "[Audio] frames: valid=" << m_validFrames << " decoded=" << m_decodedFrames
                      << " (queue ok)" << std::endl;
        }
    }
}
