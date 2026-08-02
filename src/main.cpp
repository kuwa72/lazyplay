#include <winsock2.h>
#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include "renderer_d3d11.h"
#include "decoder_d3d11.h"
#include "mdns_sd.h"
#include "rtsp_server.h"

// Global Application Instance
D3D11Renderer* g_renderer = nullptr;

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

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int main(int argc, char* argv[]) {
    std::string deviceName = "lazyplay-display";
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t targetFps = 30;
    bool vsync = true;

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
        } else if (arg == "-novsync") {
            vsync = false;
        }
    }

    std::cout << "==========================================" << std::endl;
    std::cout << "  lazyplay - Lightweight AirPlay Receiver " << std::endl;
    std::cout << "  Device Name: " << deviceName << std::endl;
    std::cout << "  Target Resolution: " << width << "x" << height << " @" << targetFps << "fps" << std::endl;
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

    // Initialize D3D11 / DXVA2 Hardware H.264 Decoder
    D3D11H264Decoder decoder;
    if (!decoder.Initialize(renderer.GetDevice(), width, height)) {
        std::cerr << "[Warning] DXVA2 Hardware Decoder fallback notice." << std::endl;
    }

    // Start mDNS Service Discovery
    MDNSService mdns;
    mdns.Start(deviceName, 7000, 5000);

    // Start RTSP Server
    RTSPServer rtsp;
    rtsp.SetVideoFrameCallback([&](const uint8_t* data, size_t size) {
        std::vector<uint8_t> frameData;
        uint32_t fWidth = width, fHeight = height;
        if (decoder.DecodePacket(data, size, frameData, fWidth, fHeight)) {
            renderer.RenderFrame(frameData.data(), fWidth, fHeight, fWidth * 4);
        }
    });
    rtsp.Start(7000);

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

        renderer.Present();
        Sleep(1000 / targetFps); // Limit FPS
    }

    // Cleanup Resources
    rtsp.Stop();
    mdns.Stop();
    decoder.Shutdown();
    renderer.Cleanup();
    g_renderer = nullptr;

    return 0;
}
