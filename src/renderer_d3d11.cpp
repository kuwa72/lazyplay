#include "renderer_d3d11.h"
#include <d3dcompiler.h>
#include <iostream>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

struct Vertex {
    float pos[2];
    float tex[2];
};

D3D11Renderer::D3D11Renderer() {}

D3D11Renderer::~D3D11Renderer() {
    Cleanup();
}

bool D3D11Renderer::Initialize(HWND hwnd, uint32_t width, uint32_t height, bool vsync) {
    m_hwnd = hwnd;
    m_width = width;
    m_height = height;
    m_vsync = vsync;

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &scd,
        &m_swapChain,
        &m_device,
        &featureLevel,
        &m_context
    );

    if (FAILED(hr)) {
        std::cerr << "[D3D11] Failed to create D3D11 Device and SwapChain. HR=0x" 
                  << std::hex << hr << std::endl;
        return false;
    }

    // Enable multithread protection for DXVA decoder & rendering threads
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(m_device.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    Resize(width, height);
    return CreateShaderResources();
}

void D3D11Renderer::Cleanup() {
    m_renderTargetView.Reset();
    m_swapChain.Reset();
    m_videoTexture.Reset();
    m_videoSRV.Reset();
    m_vertexBuffer.Reset();
    m_pixelShader.Reset();
    m_vertexShader.Reset();
    m_samplerState.Reset();
    m_context.Reset();
    m_device.Reset();
}

void D3D11Renderer::Resize(uint32_t width, uint32_t height) {
    if (!m_swapChain) return;

    m_width = width;
    m_height = height;

    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_renderTargetView.Reset();

    m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);

    ComPtr<ID3D11Texture2D> backBuffer;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_renderTargetView);

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    m_context->RSSetViewports(1, &vp);
}

void D3D11Renderer::ToggleFullscreen() {
    if (!m_hwnd) return;

    m_isFullscreen = !m_isFullscreen;
    DWORD dwStyle = GetWindowLong(m_hwnd, GWL_STYLE);

    if (m_isFullscreen) {
        GetWindowRect(m_hwnd, &m_windowedRect);
        SetWindowLong(m_hwnd, GWL_STYLE, dwStyle & ~WS_OVERLAPPEDWINDOW);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowPos(m_hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLong(m_hwnd, GWL_STYLE, dwStyle | WS_OVERLAPPEDWINDOW);
        SetWindowPos(m_hwnd, NULL,
            m_windowedRect.left, m_windowedRect.top,
            m_windowedRect.right - m_windowedRect.left,
            m_windowedRect.bottom - m_windowedRect.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

bool D3D11Renderer::CreateShaderResources() {
    // Vertex Data
    Vertex vertices[] = {
        { {-1.0f,  1.0f}, {0.0f, 0.0f} },
        { { 1.0f,  1.0f}, {1.0f, 0.0f} },
        { {-1.0f, -1.0f}, {0.0f, 1.0f} },
        { { 1.0f, -1.0f}, {1.0f, 1.0f} },
    };

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(vertices);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = vertices;

    HRESULT hr = m_device->CreateBuffer(&bd, &initData, &m_vertexBuffer);
    if (FAILED(hr)) return false;

    // Default Texture
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_width;
    texDesc.Height = m_height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DYNAMIC;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_videoTexture);
    if (FAILED(hr)) return false;

    hr = m_device->CreateShaderResourceView(m_videoTexture.Get(), nullptr, &m_videoSRV);
    if (FAILED(hr)) return false;

    // Sampler State
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    m_device->CreateSamplerState(&sampDesc, &m_samplerState);

    return true;
}

void D3D11Renderer::RenderFrame(const uint8_t* data, uint32_t width, uint32_t height, uint32_t pitch) {
    if (!m_videoTexture || !data) return;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_videoTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
        uint32_t copyPitch = (pitch < mapped.RowPitch) ? pitch : mapped.RowPitch;
        for (uint32_t y = 0; y < height; ++y) {
            memcpy(dst + y * mapped.RowPitch, data + y * pitch, copyPitch);
        }
        m_context->Unmap(m_videoTexture.Get(), 0);
    }
}

void D3D11Renderer::Present() {
    if (!m_swapChain || !m_renderTargetView) return;

    float clearColor[4] = { 0.05f, 0.05f, 0.08f, 1.0f }; // Dark Background
    m_context->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);

    m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), nullptr);
    m_swapChain->Present(m_vsync ? 1 : 0, 0);
}
