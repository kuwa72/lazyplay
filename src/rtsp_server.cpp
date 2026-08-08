#include "rtsp_server.h"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "bplist.h"
#include "sha512.h"

namespace {

std::string ToLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<uint8_t> HexToBytes(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

// Derives the video AES-128 key/iv from the audio AES key and the
// client-supplied streamConnectionID (UxPlay mirror_buffer_init_aes).
void DeriveVideoKeyIv(uint64_t streamConnectionID, const uint8_t audioKey[16],
                      uint8_t outKey[16], uint8_t outIv[16]) {
    uint8_t digest[64];
    {
        std::string s = "AirPlayStreamKey" + std::to_string(streamConnectionID);
        SHA512 ctx;
        ctx.Update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        ctx.Update(audioKey, 16);
        ctx.Final(digest);
        memcpy(outKey, digest, 16);
    }
    {
        std::string s = "AirPlayStreamIV" + std::to_string(streamConnectionID);
        SHA512 ctx;
        ctx.Update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        ctx.Update(audioKey, 16);
        ctx.Final(digest);
        memcpy(outIv, digest, 16);
    }
}

} // namespace

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
    std::cout << "[RTSP] AirPlay RTSP server active on port " << m_port << std::endl;
    return true;
}

void RTSPServer::Stop() {
    if (m_running) {
        m_running = false;
        if (m_listenSock != INVALID_SOCKET) {
            closesocket(m_listenSock);
            m_listenSock = INVALID_SOCKET;
        }
        if (m_serverThread.joinable()) m_serverThread.join();
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
        std::cout << "[RTSP] Incoming AirPlay connection from " << clientIP
                  << " (port " << m_port << ")" << std::endl;

        std::thread(&RTSPServer::HandleClient, this, clientSock, std::string(clientIP)).detach();
    }
}

const std::string* RTSPServer::Request::Header(const std::string& name) const {
    std::string lower = ToLower(name);
    for (const auto& h : headers) {
        if (ToLower(h.first) == lower) return &h.second;
    }
    return nullptr;
}

bool RTSPServer::ReadRequest(SOCKET sock, std::vector<uint8_t>& buffer, Request& req) {
    auto findHeaderEnd = [&]() -> size_t {
        for (size_t i = 0; i + 3 < buffer.size(); ++i) {
            if (buffer[i] == '\r' && buffer[i + 1] == '\n' && buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
                return i;
            }
        }
        return std::string::npos;
    };

    uint8_t temp[16384];
    size_t headerEnd = findHeaderEnd();
    while (headerEnd == std::string::npos) {
        int ret = recv(sock, (char*)temp, sizeof(temp), 0);
        if (ret <= 0) return false;
        buffer.insert(buffer.end(), temp, temp + ret);
        if (buffer.size() > 256 * 1024) return false; // header too large, bail out
        headerEnd = findHeaderEnd();
    }

    std::string headerBlock(reinterpret_cast<char*>(buffer.data()), headerEnd);
    std::istringstream iss(headerBlock);
    std::string line;
    if (!std::getline(iss, line)) return false;
    {
        std::istringstream firstLine(line);
        if (!(firstLine >> req.method >> req.url >> req.protocol)) return false;
    }

    req.headers.clear();
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        req.headers.emplace_back(Trim(line.substr(0, colon)), Trim(line.substr(colon + 1)));
    }

    size_t contentLength = 0;
    if (const std::string* cl = req.Header("Content-Length")) {
        contentLength = static_cast<size_t>(strtoull(cl->c_str(), nullptr, 10));
    }

    size_t bodyStart = headerEnd + 4;
    while (buffer.size() < bodyStart + contentLength) {
        int ret = recv(sock, (char*)temp, sizeof(temp), 0);
        if (ret <= 0) return false;
        buffer.insert(buffer.end(), temp, temp + ret);
    }

    req.body.assign(buffer.begin() + bodyStart, buffer.begin() + bodyStart + contentLength);
    buffer.erase(buffer.begin(), buffer.begin() + bodyStart + contentLength);
    return true;
}

void RTSPServer::HandleClient(SOCKET clientSock, std::string clientIp) {
    Session session;

    std::vector<uint8_t> buffer;
    while (m_running) {
        Request req;
        if (!ReadRequest(clientSock, buffer, req)) break;

        bool closeAfter = false;
        ProcessRequest(clientSock, req, clientIp, session, closeAfter);
        if (closeAfter) break;
    }

    // Connection closed without (full) TEARDOWN: release session resources
    if (session.video) session.video->Stop();
    if (session.ntp) session.ntp->Stop();
    if (session.audio) session.audio->Stop();
    closesocket(clientSock);
    std::cout << "[RTSP] Connection from " << clientIp << " closed." << std::endl;
}

void RTSPServer::SendResponse(SOCKET sock, const Request& req, const std::string& contentType,
                              const std::vector<uint8_t>& body,
                              const std::vector<std::pair<std::string, std::string>>& extraHeaders) {
    std::ostringstream response;
    response << "RTSP/1.0 200 OK\r\n";
    if (const std::string* cseq = req.Header("CSeq")) {
        response << "CSeq: " << *cseq << "\r\n";
    }
    response << "Server: AirPlay/" << AIRPLAY_SRCVERS << "\r\n";
    for (const auto& h : extraHeaders) {
        response << h.first << ": " << h.second << "\r\n";
    }
    if (!contentType.empty()) {
        response << "Content-Type: " << contentType << "\r\n";
    }
    response << "Content-Length: " << body.size() << "\r\n\r\n";

    std::string head = response.str();
    send(sock, head.c_str(), static_cast<int>(head.size()), 0);
    if (!body.empty()) {
        send(sock, (const char*)body.data(), static_cast<int>(body.size()), 0);
    }
}

void RTSPServer::SendEmptyOk(SOCKET sock, const Request& req,
                             const std::vector<std::pair<std::string, std::string>>& extraHeaders) {
    SendResponse(sock, req, "", {}, extraHeaders);
}

void RTSPServer::HandleInfo(SOCKET sock, const Request& req) {
    BPNode root = BPNode::MakeDict();

    const std::string* contentType = req.Header("Content-Type");
    bool wantsTxtAirPlay = false;
    bool wantsTxtRaop = false;
    if (contentType && contentType->find("application/x-apple-binary-plist") != std::string::npos && !req.body.empty()) {
        BPNode reqRoot;
        if (BPlistParse(req.body.data(), req.body.size(), reqRoot)) {
            if (const BPNode* qualifier = reqRoot.Find("qualifier")) {
                if (qualifier->type == BPNode::Type::Array) {
                    for (const auto& q : qualifier->array) {
                        if (q.type == BPNode::Type::String && q.str == "txtAirPlay") wantsTxtAirPlay = true;
                        if (q.type == BPNode::Type::String && q.str == "txtRAOP") wantsTxtRaop = true;
                    }
                }
            }
        }
    }
    if (wantsTxtAirPlay) root.Set("txtAirPlay", BPNode::MakeData(m_config.airplayTxt));
    if (wantsTxtRaop) root.Set("txtRAOP", BPNode::MakeData(m_config.raopTxt));
    if (contentType) {
        // TXT qualifier request: nothing else needed
        SendResponse(sock, req, "application/x-apple-binary-plist", BPlistWrite(root));
        return;
    }

    const AirPlayAdvertiseInfo& adv = m_config.advertise;
    root.Set("deviceID", BPNode::MakeString(adv.macColon));
    root.Set("macAddress", BPNode::MakeString(adv.macColon));
    root.Set("pk", BPNode::MakeData(HexToBytes(AIRPLAY_PK_HEX)));
    root.Set("features", BPNode::MakeInt(adv.Features()));
    root.Set("name", BPNode::MakeString(adv.deviceName));
    root.Set("pi", BPNode::MakeString(AIRPLAY_PI));
    root.Set("vv", BPNode::MakeInt(2));
    root.Set("statusFlags", BPNode::MakeInt(68));
    root.Set("keepAliveLowPower", BPNode::MakeInt(1));
    root.Set("sourceVersion", BPNode::MakeString(AIRPLAY_SRCVERS));
    root.Set("keepAliveSendStatsAsBody", BPNode::MakeBool(true));
    root.Set("model", BPNode::MakeString(AIRPLAY_MODEL));

    BPNode audioLatencies = BPNode::MakeArray();
    for (uint64_t type : {100, 101}) {
        BPNode lat = BPNode::MakeDict();
        lat.Set("type", BPNode::MakeInt(type));
        lat.Set("inputLatencyMicros", BPNode::MakeInt(0));
        lat.Set("audioType", BPNode::MakeString("default"));
        lat.Set("outputLatencyMicros", BPNode::MakeInt(0));
        audioLatencies.Append(lat);
    }
    root.Set("audioLatencies", audioLatencies);

    BPNode audioFormats = BPNode::MakeArray();
    for (uint64_t type : {100, 101}) {
        BPNode fmt = BPNode::MakeDict();
        fmt.Set("type", BPNode::MakeInt(type));
        // Advertise ALAC only (0x40000): macOS mirror audio defaults to AAC-ELD,
        // which Windows Media Foundation cannot decode
        fmt.Set("audioInputFormats", BPNode::MakeInt(0x40000));
        fmt.Set("audioOutputFormats", BPNode::MakeInt(0x40000));
        audioFormats.Append(fmt);
    }
    root.Set("audioFormats", audioFormats);

    BPNode displays = BPNode::MakeArray();
    BPNode display = BPNode::MakeDict();
    display.Set("uuid", BPNode::MakeString("e0ff8a27-6738-3d56-8a16-cc53aacee925"));
    display.Set("widthPhysical", BPNode::MakeInt(0));
    display.Set("heightPhysical", BPNode::MakeInt(0));
    display.Set("width", BPNode::MakeInt(m_config.displayWidth));
    display.Set("height", BPNode::MakeInt(m_config.displayHeight));
    display.Set("widthPixels", BPNode::MakeInt(m_config.displayWidth));
    display.Set("heightPixels", BPNode::MakeInt(m_config.displayHeight));
    display.Set("rotation", BPNode::MakeBool(false));
    display.Set("refreshRate", BPNode::MakeReal(1.0 / m_config.refreshRate));
    display.Set("maxFPS", BPNode::MakeInt(m_config.maxFps));
    display.Set("overscanned", BPNode::MakeBool(false));
    display.Set("features", BPNode::MakeInt(14));
    displays.Append(display);
    root.Set("displays", displays);

    SendResponse(sock, req, "application/x-apple-binary-plist", BPlistWrite(root));
}

void RTSPServer::HandleFpSetup(SOCKET sock, const Request& req, Session& session) {
    if (req.body.size() == 16) {
        std::vector<uint8_t> res(142);
        if (session.fairplay.Setup(req.body.data(), res.data()) == 0) {
            SendResponse(sock, req, "application/octet-stream", res);
            return;
        }
    } else if (req.body.size() == 164) {
        std::vector<uint8_t> res(32);
        if (session.fairplay.Handshake(req.body.data(), res.data()) == 0) {
            session.fpReady = true;
            SendResponse(sock, req, "application/octet-stream", res);
            std::cout << "[RTSP] FairPlay handshake completed." << std::endl;
            return;
        }
    }
    std::cerr << "[RTSP] Invalid fp-setup request (len=" << req.body.size() << ")" << std::endl;
    SendEmptyOk(sock, req);
}

void RTSPServer::HandleSetup(SOCKET sock, const Request& req, const std::string& clientIp, Session& session) {
    BPNode reqRoot;
    if (!BPlistParse(req.body.data(), req.body.size(), reqRoot)) {
        std::cerr << "[RTSP] SETUP with unparsable plist body." << std::endl;
        SendEmptyOk(sock, req);
        return;
    }

    BPNode resRoot = BPNode::MakeDict();

    const BPNode* ekey = reqRoot.FindData("ekey");
    const BPNode* eiv = reqRoot.FindData("eiv");
    if (ekey && eiv && ekey->data.size() >= 72 && eiv->data.size() >= 16) {
        // SETUP phase 1: key exchange + timing
        if (!session.fpReady) {
            std::cerr << "[RTSP] SETUP arrived before fp-setup; FairPlay session missing." << std::endl;
        }
        if (session.fairplay.Decrypt(ekey->data.data(), session.audioAesKey) == 0) {
            memcpy(session.audioAesIv, eiv->data.data(), 16);
            session.hasAudioKey = true;
            std::cout << "[RTSP] FairPlay ekey decrypted (audio AES key obtained)." << std::endl;
        } else {
            std::cerr << "[RTSP] fairplay_decrypt failed." << std::endl;
        }

        std::string deviceId = reqRoot.FindString("deviceID");
        std::string model = reqRoot.FindString("model");
        std::string name = reqRoot.FindString("name");
        std::cout << "[RTSP] Client: " << name << " (" << model << ", " << deviceId << ")" << std::endl;

        std::string timingProtocol = reqRoot.FindString("timingProtocol");
        uint64_t timingPort = reqRoot.FindInt("timingPort", 0);
        if (timingProtocol == "NTP" && timingPort != 0) {
            session.ntp.reset(new NtpTimingClient());
            if (!session.ntp->Start(clientIp, static_cast<uint16_t>(timingPort))) {
                std::cerr << "[RTSP] Failed to start NTP timing client." << std::endl;
                session.ntp.reset();
            }
        } else {
            std::cerr << "[RTSP] Unsupported timingProtocol=\"" << timingProtocol << "\"" << std::endl;
        }

        resRoot.Set("timingPort", BPNode::MakeInt(session.ntp ? session.ntp->GetLocalPort() : 0));
        resRoot.Set("eventPort", BPNode::MakeInt(0));
    }

    const BPNode* streams = reqRoot.Find("streams");
    if (streams && streams->type == BPNode::Type::Array) {
        BPNode resStreams = BPNode::MakeArray();
        for (const auto& stream : streams->array) {
            uint64_t type = stream.FindInt("type", 0);

            if (type == 110) {
                // Mirroring stream
                uint64_t streamConnectionID = stream.FindInt("streamConnectionID", 0);
                uint16_t dataPort = 0;
                if (session.hasAudioKey) {
                    uint8_t videoKey[16], videoIv[16];
                    DeriveVideoKeyIv(streamConnectionID, session.audioAesKey, videoKey, videoIv);

                    session.video.reset(new MirrorVideoServer());
                    session.video->SetVideoDataCallback(m_videoDataCallback);
                    session.video->SetVideoSizeCallback(m_videoSizeCallback);
                    if (!m_videoDataCallback) {
                        std::cerr << "[RTSP] WARNING: no video callback wired; stream will be received but not decoded." << std::endl;
                    }
                    if (session.video->Start(0)) {
                        session.video->SetStreamKey(videoKey, videoIv);
                        dataPort = session.video->GetPort();
                        std::cout << "[RTSP] Mirroring stream setup (streamConnectionID="
                                  << streamConnectionID << ", dataPort=" << dataPort << ")" << std::endl;
                    } else {
                        std::cerr << "[RTSP] Failed to start mirror data listener." << std::endl;
                        session.video.reset();
                    }
                } else {
                    std::cerr << "[RTSP] No AES key available for stream setup." << std::endl;
                }

                BPNode resStream = BPNode::MakeDict();
                resStream.Set("dataPort", BPNode::MakeInt(dataPort));
                resStream.Set("type", BPNode::MakeInt(110));
                resStreams.Append(resStream);
            } else if (type == 96) {
                // Audio stream: AAC-ELD decode + WASAPI playback
                uint64_t ct = stream.FindInt("ct", 0);
                uint64_t spf = stream.FindInt("spf", 0);
                uint64_t audioFormat = stream.FindInt("audioFormat", 0);

                uint16_t audioPort = 0;
                if (ct == 8 && session.hasAudioKey) {
                    if (!session.audio) {
                        session.audio.reset(new AudioReceiver());
                        if (!session.audio->Start(0, session.audioAesKey, session.audioAesIv)) {
                            std::cerr << "[RTSP] Failed to start audio receiver." << std::endl;
                            session.audio.reset();
                        }
                    }
                    if (session.audio) audioPort = session.audio->GetPort();
                } else {
                    std::cerr << "[RTSP] Audio stream with unsupported ct=" << ct << " ignored." << std::endl;
                }

                BPNode resStream = BPNode::MakeDict();
                resStream.Set("dataPort", BPNode::MakeInt(audioPort));
                resStream.Set("controlPort", BPNode::MakeInt(audioPort));
                resStream.Set("type", BPNode::MakeInt(96));
                resStreams.Append(resStream);

                std::cout << "[RTSP] Audio stream setup: ct=" << ct << " spf=" << spf
                          << " audioFormat=0x" << std::hex << audioFormat << std::dec
                          << (ct == 2 ? " (ALAC)" : ct == 8 ? " (AAC-ELD)" : "")
                          << ", port " << audioPort << std::endl;
            } else {
                std::cerr << "[RTSP] Unknown stream type " << type << " requested." << std::endl;
            }
        }
        resRoot.Set("streams", resStreams);
    }

    SendResponse(sock, req, "application/x-apple-binary-plist", BPlistWrite(resRoot));
}

void RTSPServer::HandleTeardown(SOCKET sock, const Request& req, Session& session, bool& closeAfter) {
    // TEARDOWN carries a plist with the stream types to tear down. A partial
    // teardown (e.g. audio only, type 96) must NOT end the session: macOS
    // routinely adds and removes the audio stream while mirroring continues.
    BPNode reqRoot;
    bool hasStreams = false;
    if (!req.body.empty() && BPlistParse(req.body.data(), req.body.size(), reqRoot)) {
        if (const BPNode* streams = reqRoot.Find("streams")) {
            if (streams->type == BPNode::Type::Array && !streams->array.empty()) {
                hasStreams = true;
                for (const auto& stream : streams->array) {
                    uint64_t type = stream.FindInt("type", 0);
                    if (type == 110 && session.video) {
                        session.video->Stop();
                        session.video.reset();
                        std::cout << "[RTSP] Mirroring stream torn down." << std::endl;
                    } else if (type == 96) {
                        if (session.audio) {
                            session.audio->Stop();
                            session.audio.reset();
                        }
                        std::cout << "[RTSP] Audio stream torn down (video session continues)." << std::endl;
                    }
                }
            }
        }
    }

    if (!hasStreams) {
        // Full session teardown: release everything; the client closes the connection.
        if (session.video) { session.video->Stop(); session.video.reset(); }
        if (session.ntp) { session.ntp->Stop(); session.ntp.reset(); }
        if (session.audio) { session.audio->Stop(); session.audio.reset(); }
        std::cout << "[RTSP] Session torn down." << std::endl;
        SendEmptyOk(sock, req, { { "Connection", "close" } });
        closeAfter = true;
        return;
    }

    SendEmptyOk(sock, req);
}

void RTSPServer::ProcessRequest(SOCKET clientSock, const Request& req, const std::string& clientIp,
                                Session& session, bool& closeAfter) {
    std::cout << "[RTSP Request] " << req.method << " " << req.url << std::endl;

    if (req.method == "GET" && req.url.find("/info") != std::string::npos) {
        HandleInfo(clientSock, req);
    } else if (req.method == "POST" && req.url.find("/fp-setup") != std::string::npos) {
        HandleFpSetup(clientSock, req, session);
    } else if (req.method == "POST" && (req.url.find("/pair-setup") != std::string::npos ||
                                        req.url.find("/pair-verify") != std::string::npos)) {
        // Not expected: features bit 27 (legacy pairing) is cleared in our advertisement
        std::cerr << "[RTSP] Client attempted pairing despite bit27=0: " << req.url << std::endl;
        SendEmptyOk(clientSock, req);
    } else if (req.method == "OPTIONS") {
        SendEmptyOk(clientSock, req,
                    { { "Public", "SETUP, RECORD, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER, POST, GET" } });
    } else if (req.method == "SETUP") {
        HandleSetup(clientSock, req, clientIp, session);
    } else if (req.method == "RECORD") {
        SendEmptyOk(clientSock, req,
                    { { "Audio-Latency", "0" }, { "Audio-Jack-Status", "connected; type=analog" } });
        std::cout << "[RTSP] RECORD: streaming started." << std::endl;
    } else if (req.method == "FLUSH") {
        SendEmptyOk(clientSock, req);
    } else if (req.method == "TEARDOWN") {
        HandleTeardown(clientSock, req, session, closeAfter);
    } else if (req.method == "GET_PARAMETER") {
        const std::string* contentType = req.Header("Content-Type");
        if (contentType && *contentType == "text/parameters" && !req.body.empty() &&
            std::string((const char*)req.body.data(), req.body.size()).find("volume") != std::string::npos) {
            std::string volume = "volume: 0.0\r\n";
            SendResponse(clientSock, req, "text/parameters",
                         std::vector<uint8_t>(volume.begin(), volume.end()));
        } else {
            SendEmptyOk(clientSock, req);
        }
    } else if (req.method == "SET_PARAMETER") {
        // "volume: <dB>" (text/parameters) adjusts the mirrored audio playback volume
        const std::string* contentType = req.Header("Content-Type");
        bool isVolType = contentType && (contentType->find("text/parameters") != std::string::npos);
        if (isVolType && !req.body.empty() && session.audio) {
            std::string body((const char*)req.body.data(), req.body.size());
            size_t vpos = body.find("volume:");
            if (vpos != std::string::npos) {
                float v = strtof(body.c_str() + vpos + 7, nullptr);
                // AirPlay volume is usually dB (-30..0, -144=mute), but some
                // clients send a 0..1 linear multiplier; handle both.
                if (v > 0.0f && v <= 1.0f) {
                    session.audio->SetVolumeDb(20.0f * static_cast<float>(std::log10(v)));
                } else {
                    session.audio->SetVolumeDb(v);
                }
            }
        }
        SendEmptyOk(clientSock, req);
    } else if (req.method == "POST" && req.url.find("/feedback") != std::string::npos) {
        // Periodic keep-alive stats from the client; acknowledge silently
        SendEmptyOk(clientSock, req);
    } else if (req.method == "ANNOUNCE") {
        // Old (non-plist) AirPlay 1 protocol: not used by macOS mirroring clients
        SendEmptyOk(clientSock, req);
    } else {
        std::cout << "[RTSP] Unhandled request: " << req.method << " " << req.url << std::endl;
        SendEmptyOk(clientSock, req);
    }
}
