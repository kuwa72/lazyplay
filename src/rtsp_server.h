#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <memory>
#include <cstdint>

#include "airplay_advertise.h"
#include "fairplay.h"
#include "video_stream.h"
#include "ntp_timing.h"

struct AirPlayServerConfig {
    AirPlayAdvertiseInfo advertise;
    uint16_t displayWidth = 1280;
    uint16_t displayHeight = 720;
    uint8_t maxFps = 30;
    uint8_t refreshRate = 60;
    std::vector<uint8_t> airplayTxt; // raw DNS-SD TXT record (served via GET /info)
    std::vector<uint8_t> raopTxt;
};

class RTSPServer {
public:
    RTSPServer();
    ~RTSPServer();

    bool Start(uint16_t port = 7000);
    void Stop();

    void SetConfig(const AirPlayServerConfig& config) { m_config = config; }

    // Decoded H.264 access units (Annex-B), invoked on the mirror data thread
    void SetVideoDataCallback(VideoDataCallback cb) { m_videoDataCallback = cb; }
    void SetVideoSizeCallback(VideoSizeCallback cb) { m_videoSizeCallback = cb; }

private:
    struct Request {
        std::string method;
        std::string url;
        std::string protocol;
        std::vector<std::pair<std::string, std::string>> headers;
        std::vector<uint8_t> body;

        const std::string* Header(const std::string& name) const;
    };

    // Per-connection AirPlay session state
    struct Session {
        FairPlay fairplay;
        bool fpReady = false;
        uint8_t audioAesKey[16] = {};
        bool hasAudioKey = false;
        std::unique_ptr<MirrorVideoServer> video;
        std::unique_ptr<NtpTimingClient> ntp;
        SOCKET audioSink = INVALID_SOCKET; // bound UDP port that discards audio
    };

    void ServerLoop();
    void HandleClient(SOCKET clientSock, std::string clientIp);
    bool ReadRequest(SOCKET sock, std::vector<uint8_t>& buffer, Request& req);
    void ProcessRequest(SOCKET clientSock, const Request& req, const std::string& clientIp,
                        Session& session, bool& closeAfter);

    void SendResponse(SOCKET sock, const Request& req, const std::string& contentType,
                      const std::vector<uint8_t>& body,
                      const std::vector<std::pair<std::string, std::string>>& extraHeaders = {});
    void SendEmptyOk(SOCKET sock, const Request& req,
                     const std::vector<std::pair<std::string, std::string>>& extraHeaders = {});

    // AirPlay method handlers
    void HandleInfo(SOCKET sock, const Request& req);
    void HandleFpSetup(SOCKET sock, const Request& req, Session& session);
    void HandleSetup(SOCKET sock, const Request& req, const std::string& clientIp, Session& session);
    void HandleTeardown(SOCKET sock, const Request& req, Session& session, bool& closeAfter);

    uint16_t m_port = 7000;
    SOCKET m_listenSock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::thread m_serverThread;

    AirPlayServerConfig m_config;
    VideoDataCallback m_videoDataCallback;
    VideoSizeCallback m_videoSizeCallback;
};

#endif // RTSP_SERVER_H
