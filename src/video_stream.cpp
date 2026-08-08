#include "video_stream.h"

#include <ws2tcpip.h>
#include <iostream>
#include <cstring>

namespace {

uint32_t GetLE32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v; // x86/x64 is little-endian
}

uint64_t GetLE64(const uint8_t* p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

uint16_t GetBE16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t GetBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

float GetLEFloat(const uint8_t* p) {
    float f;
    memcpy(&f, p, 4);
    return f;
}

const uint8_t NAL_START_CODE[4] = { 0x00, 0x00, 0x00, 0x01 };

} // namespace

MirrorVideoServer::MirrorVideoServer() {}

MirrorVideoServer::~MirrorVideoServer() {
    Stop();
}

bool MirrorVideoServer::Start(uint16_t port) {
    m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSock == INVALID_SOCKET) return false;

    BOOL reuse = TRUE;
    setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(m_listenSock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(m_listenSock, 1) == SOCKET_ERROR) {
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        return false;
    }

    sockaddr_in bound = {};
    int boundLen = sizeof(bound);
    getsockname(m_listenSock, (struct sockaddr*)&bound, &boundLen);
    m_port = ntohs(bound.sin_port);

    m_running = true;
    m_thread = std::thread(&MirrorVideoServer::ThreadMain, this);
    std::cout << "[Mirror] Video data channel listening on TCP port " << m_port << std::endl;
    return true;
}

void MirrorVideoServer::Stop() {
    if (m_running) {
        m_running = false;
        if (m_listenSock != INVALID_SOCKET) {
            closesocket(m_listenSock);
            m_listenSock = INVALID_SOCKET;
        }
        if (m_thread.joinable()) m_thread.join();
        ResetStreamState();
        std::cout << "[Mirror] Video data channel stopped." << std::endl;
    }
}

void MirrorVideoServer::SetStreamKey(const uint8_t key[16], const uint8_t iv[16]) {
    m_aes.Init(key, iv);
    m_aesReady = true;
    ResetStreamState();
}

void MirrorVideoServer::ResetStreamState() {
    m_spsPps.clear();
    m_prependSpsPps = false;
    m_vclPackets = 0;
    m_vclBytes = 0;
    m_framesToDecoder = 0;
}

void MirrorVideoServer::ThreadMain() {
    while (m_running) {
        sockaddr_in clientAddr = {};
        int addrLen = sizeof(clientAddr);
        SOCKET stream = accept(m_listenSock, (struct sockaddr*)&clientAddr, &addrLen);
        if (stream == INVALID_SOCKET) {
            if (!m_running) break;
            continue;
        }

        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, sizeof(clientIP));
        std::cout << "[Mirror] Video stream connected from " << clientIP << std::endl;

        // Periodic timeout so we can notice shutdown while blocked in recv
        DWORD recvTimeout = 1000;
        setsockopt(stream, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeout, sizeof(recvTimeout));

        // Large receive buffer to absorb IDR bursts on big scene changes
        // (weak CPU + Wi-Fi can otherwise back-pressure the TCP stream)
        int rcvBuf = 8 * 1024 * 1024;
        setsockopt(stream, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvBuf, sizeof(rcvBuf));
        int nodelay = 1;
        setsockopt(stream, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

        ResetStreamState();

        while (m_running) {
            uint8_t header[128];
            if (!RecvAll(stream, header, sizeof(header))) break;

            uint32_t payloadSize = GetLE32(header);
            if (payloadSize > 4 * 1024 * 1024) { // sanity bound (aborted/garbled stream)
                std::cerr << "[Mirror] Implausible payload size " << payloadSize << ", dropping connection." << std::endl;
                break;
            }

            m_payloadBuf.resize(payloadSize);
            if (payloadSize > 0 && !RecvAll(stream, m_payloadBuf.data(), payloadSize)) break;

            HandlePacket(header, m_payloadBuf);
        }

        if (m_running) {
            int err = WSAGetLastError();
            std::cout << "[Mirror] Video data channel closed (wsa=" << err
                      << ", packets=" << m_vclPackets << ", bytes=" << m_vclBytes
                      << ", toDecoder=" << m_framesToDecoder << "); waiting for reconnect." << std::endl;
        }
        closesocket(stream);
        ResetStreamState();
    }
}

bool MirrorVideoServer::RecvAll(SOCKET sock, uint8_t* buf, size_t len) {
    size_t read = 0;
    while (read < len) {
        int ret = recv(sock, (char*)buf + read, static_cast<int>(len - read), 0);
        if (ret > 0) {
            read += ret;
            continue;
        }
        if (ret == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT) {
            if (!m_running) return false;
            continue;
        }
        return false; // closed or hard error
    }
    return true;
}

void MirrorVideoServer::HandlePacket(const uint8_t* header, const std::vector<uint8_t>& payload) {
    uint32_t payloadSize = static_cast<uint32_t>(payload.size());
    uint8_t type = header[4];
    uint64_t timestamp = GetLE64(header + 8);

    switch (type) {
    case 0x00: {
        // Encrypted VCL NAL unit(s)
        if (!m_aesReady) {
            std::cerr << "[Mirror] Encrypted video arrived before SETUP; dropping." << std::endl;
            return;
        }

        m_vclPackets++;
        m_vclBytes += payloadSize;
        if (m_vclPackets <= 3 || m_vclPackets % 300 == 0) {
            std::cout << "[Mirror] VCL packet #" << m_vclPackets << " size=" << payloadSize
                      << " flags=0x" << std::hex << (int)header[5] << std::dec
                      << " ts=" << timestamp << std::endl;
        }

        m_decrypted.resize(payloadSize);
        m_aes.Process(payload.data(), m_decrypted.data(), payloadSize);

        m_annexB.clear();
        if (m_prependSpsPps) {
            if (timestamp != m_spsPpsTimestamp) {
                std::cerr << "[Mirror] SPS/PPS timestamp mismatch (cached ts=" << m_spsPpsTimestamp
                          << " vs packet ts=" << timestamp << "); discarding cached headers." << std::endl;
            } else {
                m_annexB.insert(m_annexB.end(), m_spsPps.begin(), m_spsPps.end());
                std::cout << "[Mirror] Prepended SPS/PPS to IDR packet #" << m_vclPackets << std::endl;
            }
            m_prependSpsPps = false;
        }
        // Decrypted region starts here; the SPS/PPS prefix is already Annex-B
        size_t pos = m_annexB.size();
        m_annexB.insert(m_annexB.end(), m_decrypted.begin(), m_decrypted.end());

        // Replace 4-byte big-endian NAL lengths with Annex-B start codes
        bool valid = true;
        while (pos < m_annexB.size()) {
            if (pos + 4 > m_annexB.size()) { valid = false; break; }
            uint32_t nalLen = GetBE32(m_annexB.data() + pos);
            memcpy(m_annexB.data() + pos, NAL_START_CODE, 4);
            pos += 4;
            if (pos + nalLen > m_annexB.size() || (m_annexB[pos] & 0x80)) { valid = false; break; }
            pos += nalLen;
        }
        if (pos != m_annexB.size()) valid = false;

        if (!valid) {
            std::cerr << "[Mirror] Malformed NAL data (decryption desync?); dropping packet." << std::endl;
            return;
        }

        if (m_dataCallback) {
            m_framesToDecoder++;
            m_dataCallback(m_annexB.data(), m_annexB.size());
        } else if (m_vclPackets <= 3 || m_vclPackets % 300 == 0) {
            std::cout << "[Mirror] WARNING: video data callback is empty; frame dropped." << std::endl;
        }
        break;
    }

    case 0x01: {
        // Unencrypted SPS/PPS parameter packet (also carries video dimensions)
        if (payloadSize < 12) return;

        float width = GetLEFloat(header + 56);
        float height = GetLEFloat(header + 60);
        if (width >= 16 && height >= 16 && m_sizeCallback) {
            m_sizeCallback(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        }

        uint16_t spsSize = GetBE16(payload.data() + 6);
        if (static_cast<size_t>(8) + spsSize + 3 > payloadSize) return;
        const uint8_t* sps = payload.data() + 8;
        uint16_t ppsSize = GetBE16(payload.data() + spsSize + 9);
        if (static_cast<size_t>(spsSize) + 11 + ppsSize > payloadSize) return;
        const uint8_t* pps = payload.data() + spsSize + 11;

        m_spsPps.clear();
        m_spsPps.insert(m_spsPps.end(), NAL_START_CODE, NAL_START_CODE + 4);
        m_spsPps.insert(m_spsPps.end(), sps, sps + spsSize);
        m_spsPps.insert(m_spsPps.end(), NAL_START_CODE, NAL_START_CODE + 4);
        m_spsPps.insert(m_spsPps.end(), pps, pps + ppsSize);
        m_prependSpsPps = true;
        m_spsPpsTimestamp = timestamp;

        std::cout << "[Mirror] SPS/PPS received (video " << static_cast<uint32_t>(width) << "x"
                  << static_cast<uint32_t>(height) << ", option=0x" << std::hex
                  << ((int)header[6] << 8 | header[7]) << std::dec
                  << ", sps=" << spsSize << "B pps=" << ppsSize << "B)" << std::endl;
        if (header[6] == 0x56 || header[6] == 0x5e) {
            std::cout << "[Mirror] NOTE: client flagged the video stream as suspended/stopping." << std::endl;
        }
        break;
    }

    case 0x02: // old-protocol keepalive
    case 0x05: // per-second streaming report (binary plist)
        break;

    default:
        std::cout << "[Mirror] Ignoring packet type 0x" << std::hex << (int)type << std::dec << std::endl;
        break;
    }
}
