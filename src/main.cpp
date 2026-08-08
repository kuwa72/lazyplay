#include <winsock2.h>
#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

#include "renderer_d3d11.h"
#include "decoder_d3d11.h"
#include "mdns_sd.h"
#include "rtsp_server.h"

// Global Application Instance
D3D11Renderer* g_renderer = nullptr;

namespace {

std::string SanitizeHostLabel(const std::string& name) {
    std::string out;
    for (char c : name) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '-') out.push_back(c);
        else out.push_back('-');
    }
    if (out.empty()) out = "lazyplay";
    return out;
}

} // namespace

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_SIZE:
        if (g_renderer && wParam != SIZE_MINIMIZED) {
            uint32_t width = LOWORD(lParam);
            uint32_t height = HIWORD(lParam);
            g_renderer->Resize(width, height);
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE || wParam == 'Q') {
            PostQuitMessage(0);
            return 0;
        }
        if (wParam == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000)) { // Alt + Enter
            if (g_renderer) {
                g_renderer->ToggleFullscreen();
            }
            return 0;
        }
        break;

    case WM_RBUTTONUP:
        // Keyboard-less (tablet) exit path: right-click / touch long-press quits
        PostQuitMessage(0);
        return 0;

    case WM_SETCURSOR:
        // Hide the mouse cursor over the video in fullscreen
        if (g_renderer && g_renderer->IsFullscreen() && LOWORD(lParam) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int main(int argc, char* argv[]) {
    std::string deviceName = "lazyplay-display";
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t targetFps = 30;
    bool vsync = true;
    bool startFullscreen = true; // appliance default (dot-perfect on the panel)

    // Parse Command Line Arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-name" && i + 1 < argc) {
            deviceName = argv[++i];
        } else if (arg == "-fps" && i + 1 < argc) {
            targetFps = std::stoi(argv[++i]);
        } else if (arg == "-res" && i + 1 < argc) {
            std::string res = argv[++i];
            if (res == "1080p") {
                width = 1920; height = 1080;
            } else if (res == "720p") {
                width = 1280; height = 720;
            }
        } else if (arg == "-vsync" && i + 1 < argc) {
            vsync = (std::stoi(argv[++i]) != 0);
        } else if (arg == "-novsync") {
            vsync = false;
        } else if (arg == "-fullscreen") {
            startFullscreen = true;
        } else if (arg == "-window") {
            startFullscreen = false;
        }
    }

    // Per-monitor DPI awareness: the window maps 1:1 to physical pixels even
    // when the display uses Windows scaling (tablets often default to 125%+),
    // which otherwise bitmap-scales the video and shifts/blurs pixels.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::cout << "==========================================" << std::endl;
    std::cout << "  lazyplay - Lightweight AirPlay Receiver " << std::endl;
    std::cout << "  Device Name: " << deviceName << std::endl;
    std::cout << "  Target Resolution: " << width << "x" << height << " @" << targetFps << "fps" << std::endl;
    std::cout << "  Quit: right-click / long-press (or Esc)" << std::endl;
    std::cout << "==========================================" << std::endl;

    // Register Win32 Window Class
    const wchar_t CLASS_NAME[] = L"LazyplayWindow";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

    RegisterClassW(&wc);

    // With DPI awareness, AdjustWindowRect* yields physical pixels directly
    RECT wr = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"lazyplay - AirPlay SubDisplay",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr
    );

    if (!hwnd) {
        std::cerr << "[Error] Failed to create Win32 Window." << std::endl;
        return -1;
    }

    // Initialize D3D11 Renderer
    D3D11Renderer renderer;
    if (!renderer.Initialize(hwnd, width, height, vsync)) {
        std::cerr << "[Error] Failed to initialize D3D11 Renderer." << std::endl;
        return -1;
    }
    g_renderer = &renderer;

    // Fullscreen from the start (e.g. -fullscreen for appliance-style displays);
    // borderless window keeps the client area exactly at the panel resolution.
    if (startFullscreen) {
        renderer.ToggleFullscreen();
    }

    // Initialize D3D11 / DXVA2 Hardware H.264 Decoder (no software fallback, REQUIREMENTS 2.2)
    D3D11H264Decoder decoder;
    if (!decoder.Initialize(renderer.GetDevice())) {
        std::cerr << "[Warning] Hardware decoder unavailable; video will not be decoded." << std::endl;
    }

    // AirPlay service identity
    AirPlayAdvertiseInfo advertise;
    advertise.deviceName = deviceName;
    advertise.macColon = GetLocalMacAddress();
    advertise.macPlain = advertise.macColon;
    advertise.macPlain.erase(std::remove(advertise.macPlain.begin(), advertise.macPlain.end(), ':'), advertise.macPlain.end());
    advertise.hostName = SanitizeHostLabel(deviceName) + ".local";
    advertise.airplayPort = 7000;
    advertise.raopPort = 5000;

    // Start mDNS Service Discovery (_airplay._tcp + _raop._tcp)
    MDNSService mdns;
    mdns.Start(advertise);

    // AirPlay RTSP server (port 7000) with the video pipeline wired in
    RTSPServer rtsp;
    AirPlayServerConfig config;
    config.advertise = advertise;
    config.displayWidth = static_cast<uint16_t>(width);
    config.displayHeight = static_cast<uint16_t>(height);
    config.maxFps = static_cast<uint8_t>(targetFps);
    config.refreshRate = 60;
    config.airplayTxt = BuildAirPlayTxt(advertise);
    config.raopTxt = BuildRaopTxt(advertise);
    rtsp.SetConfig(config);

    VideoDataCallback videoCallback = [&](const uint8_t* data, size_t size) {
        static uint64_t decodeCalls = 0, decodeOk = 0;
        decodeCalls++;

        ComPtr<ID3D11Texture2D> texture;
        uint32_t subresource = 0, frameWidth = 0, frameHeight = 0, cpuPitch = 0;
        ComPtr<IMFSample> sampleHolder;
        std::vector<uint8_t> cpuFrame;
        if (decoder.Decode(data, size, texture, subresource, sampleHolder,
                           cpuFrame, cpuPitch, frameWidth, frameHeight)) {
            decodeOk++;
            if (texture) {
                renderer.RenderNV12Frame(texture.Get(), subresource, frameWidth, frameHeight);
            } else if (!cpuFrame.empty()) {
                renderer.RenderNV12Cpu(cpuFrame.data(), frameWidth, frameHeight, cpuPitch);
            }
        }
        if (decodeCalls <= 5 || decodeCalls % 60 == 0) {
            std::cout << "[Video] decode calls=" << decodeCalls << " frames-out=" << decodeOk
                      << " (input " << size << "B)" << std::endl;
        }
    };
    VideoSizeCallback sizeCallback = [&](uint32_t w, uint32_t h) {
        std::cout << "[Video] Client reports stream size: " << w << "x" << h << std::endl;
        renderer.SetDisplayHint(w, h); // crop macroblock padding (e.g. 1080p in a 1088 buffer)
    };
    rtsp.SetVideoDataCallback(videoCallback);
    rtsp.SetVideoSizeCallback(sizeCallback);
    rtsp.Start(advertise.airplayPort);

    // RAOP endpoint (port 5000): AirPlay 2 clients run the whole session
    // (including mirroring) over the _raop._tcp port, so wire the same
    // callbacks into this instance too. Audio is still discarded (REQUIREMENTS 2.4).
    RTSPServer raop;
    raop.SetConfig(config);
    raop.SetVideoDataCallback(videoCallback);
    raop.SetVideoSizeCallback(sizeCallback);
    raop.Start(advertise.raopPort);

    // Frame pacing: when vsync is on, Present() blocks on the sync interval
    // (60Hz display assumed); otherwise fall back to a sleep-based limiter.
    uint32_t syncInterval = targetFps >= 60 ? 1 : static_cast<uint32_t>(60 / (targetFps ? targetFps : 30));
    renderer.SetSyncInterval(syncInterval);

    // Main Win32 Event Loop
    MSG msg = {};
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        bool presented = renderer.Present(); // skipped when no new frame arrived
        if (!vsync) {
            Sleep(1000 / targetFps); // Limit FPS
        } else if (!presented) {
            Sleep(4); // idle: keep the message pump responsive without busy-waiting
        }
    }

    // Cleanup Resources
    rtsp.Stop();
    raop.Stop();
    mdns.Stop();
    decoder.Shutdown();
    renderer.Cleanup();
    g_renderer = nullptr;

    return 0;
}
