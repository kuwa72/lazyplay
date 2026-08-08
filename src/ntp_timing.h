#ifndef NTP_TIMING_H
#define NTP_TIMING_H

#include <winsock2.h>
#include <cstdint>
#include <string>
#include <thread>
#include <atomic>

// AirPlay "timingProtocol=NTP" exchange: we act as the requester, sending a
// 32-byte timing packet to the client's timing port every 3 seconds and
// tracking the clock offset from its replies (UxPlay raop_ntp semantics).
class NtpTimingClient {
public:
    NtpTimingClient();
    ~NtpTimingClient();

    bool Start(const std::string& clientIp, uint16_t clientPort);
    void Stop();

    uint16_t GetLocalPort() const { return m_localPort; }
    int64_t GetOffsetNs() const { return m_offsetNs.load(); }

private:
    void ThreadMain();

    uint16_t m_localPort = 0;
    std::string m_clientIp;
    uint16_t m_clientPort = 0;

    SOCKET m_sock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    std::atomic<int64_t> m_offsetNs{0};
};

#endif // NTP_TIMING_H
