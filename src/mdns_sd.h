#ifndef MDNS_SD_H
#define MDNS_SD_H

#include <string>
#include <thread>
#include <atomic>
#include <cstdint>
#include <vector>

#include "airplay_advertise.h"

class MDNSService {
public:
    MDNSService();
    ~MDNSService();

    bool Start(const AirPlayAdvertiseInfo& info);
    void Stop();

private:
    void AnnounceLoop();
    void ListenLoop();
    void SendMDNSResponse();

    std::vector<uint8_t> BuildMDNSPacket();
    void EncodeDomainName(std::vector<uint8_t>& buffer, const std::string& domain);

    AirPlayAdvertiseInfo m_info;
    std::string m_localIP;
    uintptr_t m_sendSock = ~0ULL; // persistent UDP socket for announcements (SOCKET)

    std::atomic<bool> m_running{false};
    std::thread m_announceThread;
    std::thread m_listenThread;
};

// Network identity helpers (used to fill AirPlayAdvertiseInfo)
std::string GetLocalIPAddress();
std::string GetLocalMacAddress(); // uppercase, colon separated

#endif // MDNS_SD_H
