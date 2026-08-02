#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <cstdint>

typedef std::function<void(const uint8_t* data, size_t size)> VideoFrameCallback;

class RTSPServer {
public:
    RTSPServer();
    ~RTSPServer();

    bool Start(uint16_t port = 7000);
    void Stop();

    void SetVideoFrameCallback(VideoFrameCallback callback) {
        m_videoCallback = callback;
    }

private:
    void ServerLoop();
    void HandleClient(SOCKET clientSock);
    void ProcessRTSPRequest(SOCKET clientSock, const std::string& request);

    uint16_t m_port = 7000;
    SOCKET m_listenSock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::thread m_serverThread;

    VideoFrameCallback m_videoCallback;
};

#endif // RTSP_SERVER_H
