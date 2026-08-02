#include "rtsp_server.h"
#include <iostream>
#include <sstream>
#include <algorithm>

RTSPServer::RTSPServer() {}

RTSPServer::~RTSPServer() {
    Stop();
}

bool RTSPServer::Start(uint16_t port) {
    m_port = port;
    m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSock == INVALID_SOCKET) {
        std::cerr << "[RTSP] Failed to create socket." << std::endl;
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_port);

    if (bind(m_listenSock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "[RTSP] Bind failed on port " << m_port << std::endl;
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        return false;
    }

    if (listen(m_listenSock, 5) == SOCKET_ERROR) {
        std::cerr << "[RTSP] Listen failed." << std::endl;
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        return false;
    }

    m_running = true;
    m_serverThread = std::thread(&RTSPServer::ServerLoop, this);
    std::cout << "[RTSP] AirPlay RTSP/HTTP Server active on port " << m_port << std::endl;
    return true;
}

void RTSPServer::Stop() {
    if (m_running) {
        m_running = false;
        if (m_listenSock != INVALID_SOCKET) {
            closesocket(m_listenSock);
            m_listenSock = INVALID_SOCKET;
        }
        if (m_serverThread.joinable()) {
            m_serverThread.join();
        }
        std::cout << "[RTSP] Server stopped." << std::endl;
    }
}

void RTSPServer::ServerLoop() {
    while (m_running) {
        sockaddr_in clientAddr = {};
        int addrLen = sizeof(clientAddr);
        SOCKET clientSock = accept(m_listenSock, (struct sockaddr*)&clientAddr, &addrLen);
        if (clientSock == INVALID_SOCKET) {
            if (!m_running) break;
            continue;
        }

        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, sizeof(clientIP));
        std::cout << "[RTSP] Incoming AirPlay connection from " << clientIP << std::endl;

        std::thread(&RTSPServer::HandleClient, this, clientSock).detach();
    }
}

void RTSPServer::HandleClient(SOCKET clientSock) {
    char buffer[8192];
    while (m_running) {
        int bytesRead = recv(clientSock, buffer, sizeof(buffer) - 1, 0);
        if (bytesRead <= 0) break;

        buffer[bytesRead] = '\0';
        std::string request(buffer, bytesRead);

        ProcessRTSPRequest(clientSock, request);
    }
    closesocket(clientSock);
}

void RTSPServer::ProcessRTSPRequest(SOCKET clientSock, const std::string& request) {
    std::istringstream iss(request);
    std::string method, url, protocol;
    iss >> method >> url >> protocol;

    std::cout << "[RTSP Request] " << method << " " << url << " (" << protocol << ")" << std::endl;

    std::string cseq = "1";
    std::string activeRemote = "";
    std::string line;

    while (std::getline(iss, line)) {
        if (line.find("CSeq:") == 0 || line.find("Cseq:") == 0) {
            cseq = line.substr(line.find(":") + 1);
            cseq.erase(0, cseq.find_first_not_of(" \r\n"));
            cseq.erase(cseq.find_last_not_of(" \r\n") + 1);
        }
    }

    std::ostringstream response;

    // Handle GET /info (AirPlay 2 capabilities plist)
    if (method == "GET" && (url == "/info" || url.find("/info") != std::string::npos)) {
        std::string plistBody = 
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n"
            "<dict>\n"
            "  <key>features</key><integer>1518338039</integer>\n"
            "  <key>macAddress</key><string>A1:B2:C3:D4:E5:F6</string>\n"
            "  <key>model</key><string>AppleTV3,1</string>\n"
            "  <key>name</key><string>lazyplay-display</string>\n"
            "  <key>protovers</key><string>1.1</string>\n"
            "  <key>srcvers</key><string>220.68</string>\n"
            "</dict>\n"
            "</plist>\n";

        response << "RTSP/1.0 200 OK\r\n"
                 << "CSeq: " << cseq << "\r\n"
                 << "Content-Type: application/x-apple-binary-plist\r\n"
                 << "Content-Length: " << plistBody.length() << "\r\n"
                 << "\r\n"
                 << plistBody;
    }
    // Handle AirPlay Pair Setup & Pair Verify (Authentication bypass / stub 200 OK)
    else if (method == "POST" && (url.find("/pair-setup") != std::string::npos || 
                                 url.find("/pair-verify") != std::string::npos ||
                                 url.find("/fp-setup") != std::string::npos)) {
        // Return 32-byte stub challenge handshake response for AirPlay pair setup
        std::string dummyPayload(32, '\0');
        response << "RTSP/1.0 200 OK\r\n"
                 << "CSeq: " << cseq << "\r\n"
                 << "Content-Type: application/octet-stream\r\n"
                 << "Content-Length: " << dummyPayload.length() << "\r\n"
                 << "\r\n"
                 << dummyPayload;
        std::cout << "[RTSP] AirPlay Pair handshake (" << url << ") completed." << std::endl;
    }
    // Standard RTSP Methods
    else {
        response << "RTSP/1.0 200 OK\r\n"
                 << "CSeq: " << cseq << "\r\n"
                 << "Server: AirPlay/220.68\r\n";

        if (method == "OPTIONS") {
            response << "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER, POST, GET\r\n";
        } else if (method == "ANNOUNCE") {
            std::cout << "[RTSP] ANNOUNCE stream requested." << std::endl;
        } else if (method == "SETUP") {
            response << "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                     << "Session: 12345678\r\n";
            std::cout << "[RTSP] SETUP stream established." << std::endl;
        } else if (method == "RECORD") {
            std::cout << "[RTSP] RECORD streaming in progress..." << std::endl;
        } else if (method == "TEARDOWN") {
            std::cout << "[RTSP] TEARDOWN stream closed." << std::endl;
        }

        response << "\r\n";
    }

    std::string respStr = response.str();
    send(clientSock, respStr.c_str(), static_cast<int>(respStr.length()), 0);
}
