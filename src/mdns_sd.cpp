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
    ULONG outBufLen = sizeof(IP_ADAPTER_ADDRESSES);
    PIP_ADAPTER_ADDRESSES pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen) == ERROR_BUFFER_OVERFLOW) {
        free(pAddresses);
        pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
    }

    std::string selectedIP = "127.0.0.1";
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES pCurrAddresses = pAddresses; pCurrAddresses; pCurrAddresses = pCurrAddresses->Next) {
            if (pCurrAddresses->IfType == IF_TYPE_SOFTWARE_LOOPBACK || pCurrAddresses->OperStatus != IfOperStatusUp) {
                continue;
            }

            std::wstring friendlyName(pCurrAddresses->FriendlyName ? pCurrAddresses->FriendlyName : L"");
            std::wstring description(pCurrAddresses->Description ? pCurrAddresses->Description : L"");
            if (friendlyName.find(L"vEthernet") != std::wstring::npos || 
                description.find(L"Hyper-V") != std::wstring::npos ||
                description.find(L"Virtual") != std::wstring::npos ||
                description.find(L"WSL") != std::wstring::npos) {
                continue;
            }

            for (PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurrAddresses->FirstUnicastAddress; pUnicast; pUnicast = pUnicast->Next) {
                if (pUnicast->Address.lpSockaddr->sa_family == AF_INET) {
                    sockaddr_in* sa_in = (sockaddr_in*)pUnicast->Address.lpSockaddr;
                    char ipBuf[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &(sa_in->sin_addr), ipBuf, sizeof(ipBuf));
                    std::string ip(ipBuf);

                    if (ip != "127.0.0.1" && ip.find("169.254") != 0) {
                        selectedIP = ip;
                        if (ip.find("192.168.") == 0 || ip.find("10.") == 0) {
                            free(pAddresses);
                            return selectedIP;
                        }
                    }
                }
            }
        }
    }
    if (pAddresses) free(pAddresses);
    return selectedIP;
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

    // DNS Header (12 bytes)
    pkt.push_back(0x00); pkt.push_back(0x00); // Transaction ID
    pkt.push_back(0x84); pkt.push_back(0x00); // Flags: Authoritative Answer
    pkt.push_back(0x00); pkt.push_back(0x00); // QDCOUNT (0)
    pkt.push_back(0x00); pkt.push_back(0x04); // ANCOUNT (4 Records: PTR, SRV, TXT, A)
    pkt.push_back(0x00); pkt.push_back(0x00); // NSCOUNT
    pkt.push_back(0x00); pkt.push_back(0x00); // ARCOUNT

    std::string serviceType = "_airplay._tcp.local";
    std::string instanceName = m_deviceName + "." + serviceType;
    std::string hostName = m_deviceName + ".local";

    // 1. PTR Record: _airplay._tcp.local -> <m_deviceName>._airplay._tcp.local
    EncodeDomainName(pkt, serviceType);
    pkt.push_back(0x00); pkt.push_back(0x0C); // Type PTR (12)
    pkt.push_back(0x00); pkt.push_back(0x01); // Class IN (1)
    pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x11); pkt.push_back(0x94); // TTL 4500s

    std::vector<uint8_t> ptrData;
    EncodeDomainName(ptrData, instanceName);
    pkt.push_back(0x00); pkt.push_back(static_cast<uint8_t>(ptrData.size()));
    pkt.insert(pkt.end(), ptrData.begin(), ptrData.end());

    // 2. SRV Record: <instanceName> -> <hostName>:7000
    EncodeDomainName(pkt, instanceName);
    pkt.push_back(0x00); pkt.push_back(0x21); // Type SRV (33)
    pkt.push_back(0x80); pkt.push_back(0x01); // Class IN + Flush bit
    pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x78); // TTL 120s

    std::vector<uint8_t> srvData;
    srvData.push_back(0x00); srvData.push_back(0x00); // Priority 0
    srvData.push_back(0x00); srvData.push_back(0x00); // Weight 0
    srvData.push_back((m_airplayPort >> 8) & 0xFF);   // Port High
    srvData.push_back(m_airplayPort & 0xFF);          // Port Low
    EncodeDomainName(srvData, hostName);

    pkt.push_back(0x00); pkt.push_back(static_cast<uint8_t>(srvData.size()));
    pkt.insert(pkt.end(), srvData.begin(), srvData.end());

    // 3. TXT Record: AirPlay Feature Capabilities
    EncodeDomainName(pkt, instanceName);
    pkt.push_back(0x00); pkt.push_back(0x10); // Type TXT (16)
    pkt.push_back(0x80); pkt.push_back(0x01); // Class IN + Flush bit
    pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x11); pkt.push_back(0x94); // TTL 4500s

    std::vector<std::string> txtPairs = {
        "deviceid=" + macAddr,
        "features=0x200", // Open AirPlay Screen Mirroring (No SRP/Pairing required)
        "model=AppleTV3,1",
        "srcvers=220.68",
        "flags=0x4",
        "vv=2"
    };

    std::vector<uint8_t> txtData;
    for (const auto& kv : txtPairs) {
        txtData.push_back(static_cast<uint8_t>(kv.length()));
        txtData.insert(txtData.end(), kv.begin(), kv.end());
    }

    pkt.push_back(static_cast<uint8_t>((txtData.size() >> 8) & 0xFF));
    pkt.push_back(static_cast<uint8_t>(txtData.size() & 0xFF));
    pkt.insert(pkt.end(), txtData.begin(), txtData.end());

    // 4. A Record: <hostName> -> Correct Byte-Ordered IPv4 Address
    EncodeDomainName(pkt, hostName);
    pkt.push_back(0x00); pkt.push_back(0x01); // Type A (1)
    pkt.push_back(0x80); pkt.push_back(0x01); // Class IN + Flush bit
    pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x78); // TTL 120s
    pkt.push_back(0x00); pkt.push_back(0x04); // Data length (4 bytes IPv4)

    in_addr addr;
    inet_pton(AF_INET, ipAddr.c_str(), &addr);
    const uint8_t* ipBytes = reinterpret_cast<const uint8_t*>(&addr.s_addr);
    pkt.push_back(ipBytes[0]);
    pkt.push_back(ipBytes[1]);
    pkt.push_back(ipBytes[2]);
    pkt.push_back(ipBytes[3]);

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

    std::cout << "[mDNS] AirPlay Announcer & Listener started for: \"" << m_deviceName << "\"" << std::endl;
    return true;
}

void MDNSService::Stop() {
    if (m_running) {
        m_running = false;
        if (m_announceThread.joinable()) m_announceThread.join();
        if (m_listenThread.joinable()) m_listenThread.join();
        WSACleanup();
        std::cout << "[mDNS] Responders stopped." << std::endl;
    }
}

void MDNSService::AnnounceLoop() {
    std::string localIP = GetLocalIPAddress();
    std::string localMac = GetLocalMacAddress();
    std::cout << "[mDNS] Broadcasting AirPlay A-Record IP: " << localIP << " | MAC: " << localMac << std::endl;

    while (m_running) {
        SendMDNSResponse();
        for (int i = 0; i < 20 && m_running; ++i) { // Broadcast every 2 seconds
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

    std::string localIP = GetLocalIPAddress();
    in_addr interfaceAddr;
    inet_pton(AF_INET, localIP.c_str(), &interfaceAddr);

    struct ip_mreq mreq = {};
    inet_pton(AF_INET, "224.0.0.251", &mreq.imr_multiaddr);
    mreq.imr_interface = interfaceAddr;
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq));

    char buffer[2048];
    while (m_running) {
        sockaddr_in fromAddr = {};
        int fromLen = sizeof(fromAddr);
        int bytesRead = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr*)&fromAddr, &fromLen);

        if (bytesRead > 12) {
            char fromIP[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &fromAddr.sin_addr, fromIP, sizeof(fromIP));
            std::cout << "[mDNS Query Received] from " << fromIP << " -> Replying AirPlay A-Record (" << localIP << ":7000)" << std::endl;
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

    std::string localIP = GetLocalIPAddress();
    in_addr interfaceAddr;
    inet_pton(AF_INET, localIP.c_str(), &interfaceAddr);
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&interfaceAddr, sizeof(interfaceAddr));

    sockaddr_in destAddr = {};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &destAddr.sin_addr);

    std::string localMac = GetLocalMacAddress();
    std::vector<uint8_t> packet = BuildMDNSPacket(localIP, localMac);

    sendto(sock, (const char*)packet.data(), static_cast<int>(packet.size()), 0,
           (struct sockaddr*)&destAddr, sizeof(destAddr));

    closesocket(sock);
}
