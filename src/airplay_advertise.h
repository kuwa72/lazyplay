#ifndef AIRPLAY_ADVERTISE_H
#define AIRPLAY_ADVERTISE_H

#include <cstdint>
#include <string>
#include <vector>

// AirPlay service identity shared by the mDNS announcer and the RTSP server.
// Features follow UxPlay defaults (0x5A7FFEE6) with bit 27 ("supports legacy
// pairing") cleared, so clients skip the SRP pair-setup/pair-verify handshake.
struct AirPlayAdvertiseInfo {
    std::string deviceName;            // e.g. "lazyplay-display"
    std::string macColon;              // "AA:BB:CC:DD:EE:FF" (uppercase)
    std::string macPlain;              // "AABBCCDDEEFF" (uppercase, no separators)
    std::string hostName;              // sanitized DNS-SD host, "<deviceName>.local"
    uint16_t airplayPort = 7000;
    uint16_t raopPort = 5000;
    uint32_t features1 = 0x527FFEE6;   // low 32 bits, bit 27 = 0 (no legacy pairing)
    uint32_t features2 = 0x0;          // high 32 bits (no H.265, no AirPlay 2 extras)

    uint64_t Features() const { return (static_cast<uint64_t>(features2) << 32) | features1; }
};

// Persistent public key placeholder (pairing disabled; kept for TXT/“pk” compatibility)
#define AIRPLAY_PK_HEX "b07727d6f6cd6e08b58ede525ec3cdeaa252ad9f683feb212ef8a205246554e7"
#define AIRPLAY_MODEL "AppleTV3,2"
#define AIRPLAY_SRCVERS "220.68"
#define AIRPLAY_PI "2e388006-13ba-4041-9a67-25dd4a43d536"
#define AIRPLAY_VV "2"

// DNS-SD TXT record byte encoding: length-prefixed "key=value" strings.
std::vector<uint8_t> BuildAirPlayTxt(const AirPlayAdvertiseInfo& info);
std::vector<uint8_t> BuildRaopTxt(const AirPlayAdvertiseInfo& info);

#endif // AIRPLAY_ADVERTISE_H
