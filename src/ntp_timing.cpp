#include "ntp_timing.h"

#include <ws2tcpip.h>
#include <iostream>
#include <chrono>
#include <cstring>

namespace {

const uint64_t SECONDS_FROM_1900_TO_1970 = 2208988800ULL;
const uint64_t SECOND_IN_NSECS = 1000000000ULL;

uint64_t LocalTimeNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

uint64_t GetBE64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

void PutBE64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) { p[i] = static_cast<uint8_t>(v & 0xFF); v >>= 8; }
}

// ns since Unix epoch -> NTP timestamp (8 bytes, big-endian)
void PutNtpTimestamp(uint8_t* p, uint64_t nsSince1970) {
    uint64_t seconds = nsSince1970 / SECOND_IN_NSECS + SECONDS_FROM_1900_TO_1970;
    uint64_t fraction = ((nsSince1970 % SECOND_IN_NSECS) << 32) / SECOND_IN_NSECS;
    PutBE64(p, (seconds << 32) | fraction);
}

// NTP timestamp (8 bytes, big-endian) -> ns since Unix epoch
uint64_t GetNtpTimestamp(const uint8_t* p) {
    uint64_t seconds = GetBE64(p) >> 32;
    uint64_t fraction = GetBE64(p) & 0xFFFFFFFFULL;
    return (seconds - SECONDS_FROM_1900_TO_1970) * SECOND_IN_NSECS + ((fraction * SECOND_IN_NSECS) >> 32);
}

} // namespace

NtpTimingClient::NtpTimingClient() {}

NtpTimingClient::~NtpTimingClient() {
    Stop();
}

bool NtpTimingClient::Start(const std::string& clientIp, uint16_t clientPort) {
    Stop();

    m_clientIp = clientIp;
    m_clientPort = clientPort;

    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) return false;

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0; // ephemeral

    if (bind(m_sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
        return false;
    }

    sockaddr_in bound = {};
    int boundLen = sizeof(bound);
    getsockname(m_sock, (struct sockaddr*)&bound, &boundLen);
    m_localPort = ntohs(bound.sin_port);

    DWORD recvTimeout = 500;
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeout, sizeof(recvTimeout));

    m_running = true;
    m_thread = std::thread(&NtpTimingClient::ThreadMain, this);
    std::cout << "[Timing] NTP timing client on UDP port " << m_localPort
              << " -> client port " << clientPort << std::endl;
    return true;
}

void NtpTimingClient::Stop() {
    if (m_running) {
        m_running = false;
        if (m_sock != INVALID_SOCKET) {
            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
        }
        if (m_thread.joinable()) m_thread.join();
    }
}

void NtpTimingClient::ThreadMain() {
    sockaddr_in clientAddr = {};
    clientAddr.sin_family = AF_INET;
    clientAddr.sin_port = htons(m_clientPort);
    inet_pton(AF_INET, m_clientIp.c_str(), &clientAddr.sin_addr);

    uint64_t clientRefTime = 0; // last client transmit timestamp (raw BE64 in response)
    uint64_t recvTime = 0;      // local ns when the last response arrived

    while (m_running) {
        uint8_t request[32] = { 0x80, 0xd2, 0x00, 0x07 };
        uint64_t sendTime = LocalTimeNs();
        PutNtpTimestamp(request + 24, sendTime);
        if (recvTime) {
            PutBE64(request + 8, clientRefTime);
            PutNtpTimestamp(request + 16, recvTime);
        }

        int sent = sendto(m_sock, (const char*)request, sizeof(request), 0,
                          (struct sockaddr*)&clientAddr, sizeof(clientAddr));
        if (sent == SOCKET_ERROR) {
            if (!m_running) break;
        } else {
            uint8_t response[128];
            int len = recvfrom(m_sock, (char*)response, sizeof(response), 0, nullptr, nullptr);
            if (len >= 32) {
                recvTime = LocalTimeNs();
                clientRefTime = GetBE64(response + 24);

                // t0: our transmit time (echoed back, NTP format), t1/t2: client clock (raw ns)
                int64_t t0 = static_cast<int64_t>(GetNtpTimestamp(response + 8));
                int64_t t1 = static_cast<int64_t>(GetBE64(response + 16));
                int64_t t2 = static_cast<int64_t>(GetBE64(response + 24));
                int64_t t3 = static_cast<int64_t>(recvTime);
                m_offsetNs.store(((t1 - t0) + (t2 - t3)) / 2);
            }
        }

        // 3 second interval, wakeable on shutdown
        for (int i = 0; i < 30 && m_running; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
