#ifndef MDNS_SD_H
#define MDNS_SD_H

#include <string>
#include <thread>
#include <atomic>
#include <cstdint>
#include <vector>

class MDNSService {
public:
    MDNSService();
    ~MDNSService();

    bool Start(const std::string& deviceName, uint16_t airplayPort = 7000, uint16_t raopPort = 5000);
    void Stop();

private:
    void AnnounceLoop();
    void ListenLoop();
    void SendMDNSResponse();

    std::string GetLocalIPAddress();
    std::string GetLocalMacAddress();

    std::vector<uint8_t> BuildMDNSPacket(const std::string& ipAddr, const std::string& macAddr);
    void EncodeDomainName(std::vector<uint8_t>& buffer, const std::string& domain);

    std::string m_deviceName;
    uint16_t m_airplayPort = 7000;
    uint16_t m_raopPort = 5000;

    std::atomic<bool> m_running{false};
    std::thread m_announceThread;
    std::thread m_listenThread;
};

#endif // MDNS_SD_H
