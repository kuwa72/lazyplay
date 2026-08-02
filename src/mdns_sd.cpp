#include "mdns_sd.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

MDNSService::MDNSService() {}

MDNSService::~MDNSService() {
    Stop();
}

std::string MDNSService::GetLocalIPAddress() {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        struct addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_INET;
        if (getaddrinfo(hostname, nullptr, &hints, &res) == 0 && res) {
            for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
                sockaddr_in* saddr = (sockaddr_in*)p->ai_addr;
                char ipStr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &saddr->sin_addr, ipStr, sizeof(ipStr));
                std::string ip(ipStr);
                if (ip != "127.0.0.1" && ip.find("169.254") != 0) { // Exclude loopback & APIPA
                    freeaddrinfo(res);
                    return ip;
                }
            }
            freeaddrinfo(res);
        }
    }
    return "127.0.0.1";
}

std::string MDNSService::GetLocalMacAddress() {
    ULONG outBufLen = sizeof(IP_ADAPTER_ADDRESSES);
    PIP_ADAPTER_ADDRESSES pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen) == ERROR_BUFFER_OVERFLOW) {
        free(pAddresses);
        pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
    }

    std::string macStr = "A1:B2:C3:D4:E5:F6";
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES pCurrAddresses = pAddresses; pCurrAddresses; pCurrAddresses = pCurrAddresses->Next) {
            if (pCurrAddresses->IfType != IF_TYPE_SOFTWARE_LOOPBACK && pCurrAddresses->PhysicalAddressLength == 6) {
                std::ostringstream oss;
                for (DWORD i = 0; i < pCurrAddresses->PhysicalAddressLength; ++i) {
                    if (i > 0) oss << ":";
                    oss << std::hex << std::setw(2) << std::setfill('0') << (int)pCurrAddresses->PhysicalAddress[i];
                }
                macStr = oss.str();
                break;
            }
        }
    }
    if (pAddresses) free(pAddresses);
    return macStr;
}

void MDNSService::EncodeDomainName(std::vector<uint8_t>& buffer, const std::string& domain) {
    std::stringstream ss(domain);
    std::string label;
    while (std::getline(ss, label, '.')) {
        if (!label.empty()) {
            buffer.push_back(static_cast<uint8_t>(label.length()));
            buffer.insert(buffer.end(), label.begin(), label.end());
        }
    }
    buffer.push_back(0x00);
}

std::vector<uint8_t> MDNSService::BuildMDNSPacket(const std::string& ipAddr, const std::string& macAddr) {
    std::vector<uint8_t> pkt;

    // Remove colons from MAC for RAOP service instance
    std::string cleanMac = macAddr;
    cleanMac.erase(std::remove(cleanMac.begin(), cleanMac.end(), ':'), cleanMac.end());
    std::transform(cleanMac.begin(), cleanMac.end(), cleanMac.begin(), ::toupper);

    // DNS Header (12 bytes)
    pkt.push_back(0x00); pkt.push_back(0x00); // ID
    pkt.push_back(0x84); pkt.push_back(0x00); // Flags: Authoritative Answer
    pkt.push_back(0x00); pkt.push_back(0x00); // QDCOUNT
    pkt.push_back(0x00); pkt.push_back(0x06); // ANCOUNT (6 Answers: AirPlay PTR/SRV/TXT, RAOP PTR/SRV/TXT)
    pkt.push_back(0x00); pkt.push_back(0x00); // NSCOUNT
    pkt.push_back(0x00); pkt.push_back(0x00); // ARCOUNT

    std::string airplayService = "_airplay._tcp.local";
    std::string airplayInstance = m_deviceName + "." + airplayService;
    std::string raopService = "_raop._tcp.local";
    std::string raopInstance = cleanMac + "@" + m_deviceName + "." + raopService;
    std::string hostName = m_deviceName + ".local";

    // 1. AirPlay PTR Record
    EncodeDomainName(pkt, airplayService);
    pkt.push_back(0x00); pkt.push_back(0x0C); // PTR
    pkt.push_back(0x00); pkt.push_back(0x01); // IN
    pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x11); pkt.push_back(0x94);
    std::vector<uint8_t> apPtrData; EncodeDomainName(apPtrData, airplayInstance);
    pkt.push_back(0x00); pkt.push_back(static_cast<uint8_t>(apPtrData.size()));
    pkt.insert(pkt.end(), apPtrData.begin(), apPtrData.end());

    // 2. AirPlay SRV Record
    EncodeDomainName(pkt, airplayInstance);
    pkt.push_back(0x00); pkt.push_back(0x21); // SRV
    pkt.push_back(0x80); pkt.push_back(0x01); // IN + Flush
    pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x78);
    std::vector<uint8_t> apSrvData;
    apSrvData.push_back(0x00); apSrvData.push_back(0x00); // Priority
    apSrvData.push_back(0x00); apSrvData.push_back(0x00); // Weight
    apSrvData.push_back((m_airplayPort >> 8) & 0xFF);
    apSrvData.push_back(m_airplayPort & 0xFF);
    EncodeDomainName(apSrvData, hostName);
    pkt.push_back(0x00); pkt.push_back(static_cast<uint8_t>(apSrvData.size()));
    pkt.insert(pkt.end(), apSrvData.begin(), apSrvData.end());

    // 3. AirPlay TXT Record
    EncodeDomainName(pkt, airplayInstance);
    pkt.push_back(0x00); pkt.push_back(0x10); // TXT
    pkt.push_back(0x80); pkt.push_back(0x01); // IN + Flush
    pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x11); pkt.push_back(0x94);
    std::vector<std::string> apTxtPairs = {
        "deviceid=" + macAddr,
        "features=0x5A7FFFF7,0x1E",
        "model=AppleTV3,1",
        "srcvers=220.68",
        "flags=0x4",
        "vv=2",
        "pk=b08f5a79677468f637d6e4dee19379d7395d6f3c"
    };
    std::vector<uint8_t> apTxtData;
    for (const auto& kv : apTxtPairs) {
        apTxtData.push_back(static_cast<uint8_t>(kv.length()));
        apTxtData.insert(apTxtData.end(), kv.begin(), kv.end());
    }
    pkt.push_back(static_cast<uint8_t>((apTxtData.size() >> 8) & 0xFF));
    pkt.push_back(static_cast<uint8_t>(apTxtData.size() & 0xFF));
    pkt.insert(pkt.end(), apTxtData.begin(), apTxtData.end());

    // 4. RAOP PTR Record
    EncodeDomainName(pkt, raopService);
    pkt.push_back(0x00); pkt.push_back(0x0C);
    pkt.push_back(0x00); pkt.push_back(0x01);
    pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x11); pkt.push_back(0x94);
    std::vector<uint8_t> raopPtrData; EncodeDomainName(raopPtrData, raopInstance);
    pkt.push_back(0x00); pkt.push_back(static_cast<uint8_t>(raopPtrData.size()));
    pkt.insert(pkt.end(), raopPtrData.begin(), raopPtrData.end());

    // 5. RAOP SRV Record
    EncodeDomainName(pkt, raopInstance);
    pkt.push_back(0x00); pkt.push_back(0x21);
    pkt.push_back(0x80); pkt.push_back(0x01);
    pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x78);
    std::vector<uint8_t> raopSrvData;
    raopSrvData.push_back(0x00); raopSrvData.push_back(0x00);
    raopSrvData.push_back(0x00); raopSrvData.push_back(0x00);
    raopSrvData.push_back((m_raopPort >> 8) & 0xFF);
    raopSrvData.push_back(m_raopPort & 0xFF);
    EncodeDomainName(raopSrvData, hostName);
    pkt.push_back(0x00); pkt.push_back(static_cast<uint8_t>(raopSrvData.size()));
    pkt.insert(pkt.end(), raopSrvData.begin(), raopSrvData.end());

    // 6. RAOP TXT Record
    EncodeDomainName(pkt, raopInstance);
    pkt.push_back(0x00); pkt.push_back(0x10);
    pkt.push_back(0x80); pkt.push_back(0x01);
    pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x11); pkt.push_back(0x94);
    std::vector<std::string> raopTxtPairs = {
        "ch=2", "cn=0,1", "et=0,3,5", "md=0,1,2", "sr=44100", "ss=16",
        "da=true", "sv=false", "vn=65537", "tp=UDP", "sf=0x4"
    };
    std::vector<uint8_t> raopTxtData;
    for (const auto& kv : raopTxtPairs) {
        raopTxtData.push_back(static_cast<uint8_t>(kv.length()));
        raopTxtData.insert(raopTxtData.end(), kv.begin(), kv.end());
    }
    pkt.push_back(static_cast<uint8_t>((raopTxtData.size() >> 8) & 0xFF));
    pkt.push_back(static_cast<uint8_t>(raopTxtData.size() & 0xFF));
    pkt.insert(pkt.end(), raopTxtData.begin(), raopTxtData.end());

    return pkt;
}

bool MDNSService::Start(const std::string& deviceName, uint16_t airplayPort, uint16_t raopPort) {
    m_deviceName = deviceName;
    m_airplayPort = airplayPort;
    m_raopPort = raopPort;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[mDNS] WSAStartup failed." << std::endl;
        return false;
    }

    m_running = true;
    m_announceThread = std::thread(&MDNSService::AnnounceLoop, this);
    m_listenThread = std::thread(&MDNSService::ListenLoop, this);

    std::cout << "[mDNS] AirPlay & RAOP Responder started for: \"" << m_deviceName << "\"" << std::endl;
    return true;
}

void MDNSService::Stop() {
    if (m_running) {
        m_running = false;
        if (m_announceThread.joinable()) m_announceThread.join();
        if (m_listenThread.joinable()) m_listenThread.join();
        WSACleanup();
        std::cout << "[mDNS] Responder stopped." << std::endl;
    }
}

void MDNSService::AnnounceLoop() {
    std::string localIP = GetLocalIPAddress();
    std::string localMac = GetLocalMacAddress();
    std::cout << "[mDNS] Local IP: " << localIP << " | MAC: " << localMac << std::endl;

    while (m_running) {
        SendMDNSResponse();
        for (int i = 0; i < 20 && m_running; ++i) { // Announce every 2 seconds
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void MDNSService::ListenLoop() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;

    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(5353);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        closesocket(sock);
        return;
    }

    // Join IPv4 Multicast Group 224.0.0.251
    struct ip_mreq mreq = {};
    inet_pton(AF_INET, "224.0.0.251", &mreq.imr_multiaddr);
    inet_pton(AF_INET, GetLocalIPAddress().c_str(), &mreq.imr_interface);
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq));

    char buffer[2048];
    while (m_running) {
        sockaddr_in fromAddr = {};
        int fromLen = sizeof(fromAddr);
        int bytesRead = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr*)&fromAddr, &fromLen);

        if (bytesRead > 12) { // Received mDNS Query Packet from Mac
            SendMDNSResponse();
        }
    }
    closesocket(sock);
}

void MDNSService::SendMDNSResponse() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;

    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    int ttl = 255;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));

    sockaddr_in destAddr = {};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &destAddr.sin_addr);

    std::string localIP = GetLocalIPAddress();
    std::string localMac = GetLocalMacAddress();
    std::vector<uint8_t> packet = BuildMDNSPacket(localIP, localMac);

    sendto(sock, (const char*)packet.data(), static_cast<int>(packet.size()), 0,
           (struct sockaddr*)&destAddr, sizeof(destAddr));

    closesocket(sock);
}
