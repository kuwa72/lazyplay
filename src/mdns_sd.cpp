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

namespace {

void AppendU16(std::vector<uint8_t>& pkt, uint16_t v) {
    pkt.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    pkt.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendU32(std::vector<uint8_t>& pkt, uint32_t v) {
    pkt.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    pkt.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    pkt.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    pkt.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendTxtPair(std::vector<uint8_t>& txt, const std::string& key, const std::string& value) {
    std::string kv = key + "=" + value;
    if (kv.size() > 255) kv.resize(255);
    txt.push_back(static_cast<uint8_t>(kv.size()));
    txt.insert(txt.end(), kv.begin(), kv.end());
}

} // namespace

std::vector<uint8_t> BuildAirPlayTxt(const AirPlayAdvertiseInfo& info) {
    char features[24];
    snprintf(features, sizeof(features), "0x%X,0x%X", info.features1, info.features2);

    std::vector<uint8_t> txt;
    AppendTxtPair(txt, "deviceid", info.macColon);
    AppendTxtPair(txt, "features", features);
    AppendTxtPair(txt, "pw", "false");
    AppendTxtPair(txt, "flags", "0x4");
    AppendTxtPair(txt, "model", AIRPLAY_MODEL);
    AppendTxtPair(txt, "pk", AIRPLAY_PK_HEX);
    AppendTxtPair(txt, "pi", AIRPLAY_PI);
    AppendTxtPair(txt, "srcvers", AIRPLAY_SRCVERS);
    AppendTxtPair(txt, "vv", AIRPLAY_VV);
    return txt;
}

std::vector<uint8_t> BuildRaopTxt(const AirPlayAdvertiseInfo& info) {
    char features[24];
    snprintf(features, sizeof(features), "0x%X,0x%X", info.features1, info.features2);

    std::vector<uint8_t> txt;
    AppendTxtPair(txt, "ch", "2");
    AppendTxtPair(txt, "cn", "0,1"); // PCM + ALAC only (no AAC: MF can't decode AAC-ELD)
    AppendTxtPair(txt, "da", "true");
    AppendTxtPair(txt, "et", "0,3,5");
    AppendTxtPair(txt, "vv", "2");
    AppendTxtPair(txt, "ft", features);
    AppendTxtPair(txt, "am", AIRPLAY_MODEL);
    AppendTxtPair(txt, "md", "0,1,2");
    AppendTxtPair(txt, "rhd", "5.6.0.0");
    AppendTxtPair(txt, "pw", "false");
    AppendTxtPair(txt, "sr", "44100");
    AppendTxtPair(txt, "ss", "16");
    AppendTxtPair(txt, "sv", "false");
    AppendTxtPair(txt, "tp", "UDP");
    AppendTxtPair(txt, "txtvers", "1");
    AppendTxtPair(txt, "sf", "0x4");
    AppendTxtPair(txt, "vs", AIRPLAY_SRCVERS);
    AppendTxtPair(txt, "vn", "65537");
    AppendTxtPair(txt, "pk", AIRPLAY_PK_HEX);
    return txt;
}

std::string GetLocalIPAddress() {
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

std::string GetLocalMacAddress() {
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
                oss << std::uppercase << std::hex << std::setfill('0');
                for (DWORD i = 0; i < pCurrAddresses->PhysicalAddressLength; ++i) {
                    if (i > 0) oss << ":";
                    oss << std::setw(2) << (int)pCurrAddresses->PhysicalAddress[i];
                }
                macStr = oss.str();
                break;
            }
        }
    }
    if (pAddresses) free(pAddresses);
    return macStr;
}

MDNSService::MDNSService() {}

MDNSService::~MDNSService() {
    Stop();
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

std::vector<uint8_t> MDNSService::BuildMDNSPacket() {
    std::vector<uint8_t> pkt;

    std::string airplayType = "_airplay._tcp.local";
    std::string raopType = "_raop._tcp.local";
    std::string airplayInstance = m_info.deviceName + "." + airplayType;
    std::string raopInstance = m_info.macPlain + "@" + m_info.deviceName + "." + raopType;
    std::string hostName = m_info.hostName;

    // DNS Header (12 bytes)
    AppendU16(pkt, 0x0000); // Transaction ID
    AppendU16(pkt, 0x8400); // Flags: Authoritative Answer
    AppendU16(pkt, 0x0000); // QDCOUNT
    AppendU16(pkt, 7);      // ANCOUNT: PTR x2, SRV x2, TXT x2, A
    AppendU16(pkt, 0x0000); // NSCOUNT
    AppendU16(pkt, 0x0000); // ARCOUNT

    const uint32_t ttlService = 4500;
    const uint32_t ttlHost = 120;

    auto appendPtr = [&](const std::string& type, const std::string& instance) {
        EncodeDomainName(pkt, type);
        AppendU16(pkt, 12);           // Type PTR
        AppendU16(pkt, 1);            // Class IN
        AppendU32(pkt, ttlService);
        std::vector<uint8_t> data;
        EncodeDomainName(data, instance);
        AppendU16(pkt, static_cast<uint16_t>(data.size()));
        pkt.insert(pkt.end(), data.begin(), data.end());
    };

    auto appendSrv = [&](const std::string& instance, uint16_t port) {
        EncodeDomainName(pkt, instance);
        AppendU16(pkt, 33);           // Type SRV
        AppendU16(pkt, 0x8001);       // Class IN + Flush
        AppendU32(pkt, ttlHost);
        std::vector<uint8_t> data;
        AppendU16(data, 0);           // Priority
        AppendU16(data, 0);           // Weight
        AppendU16(data, port);
        EncodeDomainName(data, hostName);
        AppendU16(pkt, static_cast<uint16_t>(data.size()));
        pkt.insert(pkt.end(), data.begin(), data.end());
    };

    auto appendTxt = [&](const std::string& instance, const std::vector<uint8_t>& txt) {
        EncodeDomainName(pkt, instance);
        AppendU16(pkt, 16);           // Type TXT
        AppendU16(pkt, 0x8001);       // Class IN + Flush
        AppendU32(pkt, ttlService);
        AppendU16(pkt, static_cast<uint16_t>(txt.size()));
        pkt.insert(pkt.end(), txt.begin(), txt.end());
    };

    appendPtr(airplayType, airplayInstance);
    appendPtr(raopType, raopInstance);
    appendSrv(airplayInstance, m_info.airplayPort);
    appendSrv(raopInstance, m_info.raopPort);
    appendTxt(airplayInstance, BuildAirPlayTxt(m_info));
    appendTxt(raopInstance, BuildRaopTxt(m_info));

    // A record: hostName -> local IPv4
    EncodeDomainName(pkt, hostName);
    AppendU16(pkt, 1);                // Type A
    AppendU16(pkt, 0x8001);           // Class IN + Flush
    AppendU32(pkt, ttlHost);
    AppendU16(pkt, 4);
    in_addr addr;
    inet_pton(AF_INET, m_localIP.c_str(), &addr);
    const uint8_t* ipBytes = reinterpret_cast<const uint8_t*>(&addr.s_addr);
    pkt.insert(pkt.end(), ipBytes, ipBytes + 4);

    return pkt;
}

bool MDNSService::Start(const AirPlayAdvertiseInfo& info) {
    m_info = info;
    m_localIP = GetLocalIPAddress();

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[mDNS] WSAStartup failed." << std::endl;
        return false;
    }

    // Persistent multicast send socket (reused by announcements and query replies)
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock != INVALID_SOCKET) {
        int ttl = 255;
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
        in_addr interfaceAddr;
        inet_pton(AF_INET, m_localIP.c_str(), &interfaceAddr);
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&interfaceAddr, sizeof(interfaceAddr));
        m_sendSock = sock;
    }

    m_running = true;
    m_announceThread = std::thread(&MDNSService::AnnounceLoop, this);
    m_listenThread = std::thread(&MDNSService::ListenLoop, this);

    std::cout << "[mDNS] Announcing \"" << m_info.deviceName << "\" (AirPlay port " << m_info.airplayPort
              << ", RAOP port " << m_info.raopPort << ")" << std::endl;
    return true;
}

void MDNSService::Stop() {
    if (m_running) {
        m_running = false;
        if (m_announceThread.joinable()) m_announceThread.join();
        if (m_listenThread.joinable()) m_listenThread.join();
        if (m_sendSock != ~0ULL) {
            closesocket(static_cast<SOCKET>(m_sendSock));
            m_sendSock = ~0ULL;
        }
        WSACleanup();
        std::cout << "[mDNS] Responders stopped." << std::endl;
    }
}

void MDNSService::AnnounceLoop() {
    std::cout << "[mDNS] Broadcasting AirPlay service at " << m_localIP
              << " | deviceid: " << m_info.macColon << std::endl;

    // Bonjour-style backoff: burst at startup (1s x5), then settle to every 15s
    // (host record TTL is 120s, so this keeps the cache warm with minimal chatter)
    int announceCount = 0;
    while (m_running) {
        SendMDNSResponse();
        announceCount++;
        int intervalMs = announceCount < 5 ? 1000 : 15000;
        for (int i = 0; i < intervalMs / 100 && m_running; ++i) {
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

    // Periodic timeout so Stop() is noticed while blocked in recvfrom
    DWORD recvTimeout = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeout, sizeof(recvTimeout));

    in_addr interfaceAddr;
    inet_pton(AF_INET, m_localIP.c_str(), &interfaceAddr);

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

            // Ignore our own announcements and any non-query packets (QR bit set = response)
            if (m_localIP == fromIP || (buffer[2] & 0x80)) {
                continue;
            }

            // Extract ASCII labels from the query packet for diagnostic purposes
            std::string qNames;
            for (int i = 12; i < bytesRead - 1; ++i) {
                char c = buffer[i];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
                    qNames += c;
                } else if (c > 0 && c < 63 && !qNames.empty() && qNames.back() != '.') {
                    qNames += '.';
                }
            }

            std::cout << "[mDNS Query] from " << fromIP << " | [" << qNames << "] -> replying service records" << std::endl;
            SendMDNSResponse();
        }
    }
    closesocket(sock);
}

void MDNSService::SendMDNSResponse() {
    if (m_sendSock == ~0ULL) return;

    sockaddr_in destAddr = {};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &destAddr.sin_addr);

    std::vector<uint8_t> packet = BuildMDNSPacket();

    sendto(static_cast<SOCKET>(m_sendSock), (const char*)packet.data(), static_cast<int>(packet.size()), 0,
           (struct sockaddr*)&destAddr, sizeof(destAddr));
}
