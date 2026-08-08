#include "renderer_d3d11.h"
#include <d3dcompiler.h>
#include <iostream>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

struct Vertex {
    float pos[2];
    float tex[2];
};

const char* SHADER_SOURCE = R"(
struct VSIn  { float2 pos : POSITION; float2 tex : TEXCOORD; };
struct VSOut { float4 pos : SV_POSITION; float2 tex : TEXCOORD; };

VSOut VSMain(VSIn input) {
    VSOut output;
    output.pos = float4(input.pos, 0.0f, 1.0f);
    output.tex = input.tex;
    return output;
}

Texture2D texY  : register(t0);
Texture2D texUV : register(t1);
SamplerState samp : register(s0);

// BT.601 studio (limited) range NV12 -> RGB
float4 PSMain(VSOut input) : SV_TARGET {
    float y = (texY.Sample(samp, input.tex).r - 0.0625f) * 1.164f;
    float2 uv = texUV.Sample(samp, input.tex).rg - 0.5f;
    float r = y + 1.596f * uv.y;
    float g = y - 0.391f * uv.x - 0.813f * uv.y;
    float b = y + 2.018f * uv.x;
    return float4(saturate(r), saturate(g), saturate(b), 1.0f);
}
)";

} // namespace

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
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // cheapest on the compositor

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
        0,
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

    // Enable multithread protection: video frames arrive on the network thread
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(m_device.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    Resize(width, height);
    return CreateShaderResources();
}

void D3D11Renderer::Cleanup() {
    std::lock_guard<std::mutex> lock(m_frameMutex);
    m_renderTargetView.Reset();
    m_swapChain.Reset();
    m_videoTexture.Reset();
    m_luminanceSRV.Reset();
    m_chrominanceSRV.Reset();
    m_vertexBuffer.Reset();
    m_inputLayout.Reset();
    m_pixelShader.Reset();
    m_vertexShader.Reset();
    m_samplerState.Reset();
    m_context.Reset();
    m_device.Reset();
}

void D3D11Renderer::Resize(uint32_t width, uint32_t height) {
    if (!m_swapChain || width == 0 || height == 0) return;

    std::lock_guard<std::mutex> lock(m_frameMutex);
    m_width = width;
    m_height = height;

    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_renderTargetView.Reset();

    m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);

    ComPtr<ID3D11Texture2D> backBuffer;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_renderTargetView);
    m_presentForced = true;
}

void D3D11Renderer::ToggleFullscreen() {
    if (!m_hwnd) return;

    m_isFullscreen = !m_isFullscreen;
    DWORD dwStyle = GetWindowLong(m_hwnd, GWL_STYLE);

    if (m_isFullscreen) {
        GetWindowRect(m_hwnd, &m_windowedRect);
        SetWindowLong(m_hwnd, GWL_STYLE, dwStyle & ~WS_OVERLAPPEDWINDOW);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
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
    // Fullscreen quad
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

    // Compile shaders
    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
    hr = D3DCompile(SHADER_SOURCE, strlen(SHADER_SOURCE), nullptr, nullptr, nullptr,
                    "VSMain", "vs_4_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        std::cerr << "[D3D11] VS compile failed: "
                  << (errorBlob ? (const char*)errorBlob->GetBufferPointer() : "") << std::endl;
        return false;
    }
    hr = D3DCompile(SHADER_SOURCE, strlen(SHADER_SOURCE), nullptr, nullptr, nullptr,
                    "PSMain", "ps_4_0", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        std::cerr << "[D3D11] PS compile failed: "
                  << (errorBlob ? (const char*)errorBlob->GetBufferPointer() : "") << std::endl;
        return false;
    }

    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);
    if (FAILED(hr)) return false;
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);
    if (FAILED(hr)) return false;

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = m_device->CreateInputLayout(layout, ARRAYSIZE(layout),
                                     vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_inputLayout);
    if (FAILED(hr)) return false;

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = m_device->CreateSamplerState(&sampDesc, &m_samplerState);
    return SUCCEEDED(hr);
}

bool D3D11Renderer::EnsureVideoTexture(uint32_t width, uint32_t height) {
    if (m_videoTexture && m_videoWidth == width && m_videoHeight == height) return true;

    m_luminanceSRV.Reset();
    m_chrominanceSRV.Reset();
    m_videoTexture.Reset();

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_NV12;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_videoTexture);
    if (FAILED(hr)) {
        std::cerr << "[D3D11] Failed to create NV12 video texture (" << width << "x" << height
                  << ") HR=0x" << std::hex << hr << std::endl;
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    srvDesc.Format = DXGI_FORMAT_R8_UNORM; // Y plane
    hr = m_device->CreateShaderResourceView(m_videoTexture.Get(), &srvDesc, &m_luminanceSRV);
    if (FAILED(hr)) return false;

    srvDesc.Format = DXGI_FORMAT_R8G8_UNORM; // interleaved UV plane
    hr = m_device->CreateShaderResourceView(m_videoTexture.Get(), &srvDesc, &m_chrominanceSRV);
    if (FAILED(hr)) return false;

    m_videoWidth = width;
    m_videoHeight = height;
    std::cout << "[D3D11] Video texture created: " << width << "x" << height << std::endl;
    return true;
}

void D3D11Renderer::RenderNV12Frame(ID3D11Texture2D* srcTexture, uint32_t srcSubresource, uint32_t width, uint32_t height) {
    if (!srcTexture || !m_context) return;

    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (!EnsureVideoTexture(width, height)) return;

    D3D11_BOX box = {};
    box.left = 0;
    box.top = 0;
    box.front = 0;
    box.right = width;
    box.bottom = height;
    box.back = 1;
    m_context->CopySubresourceRegion(m_videoTexture.Get(), 0, 0, 0, 0, srcTexture, srcSubresource, &box);
    m_hasFrame = true;
    m_frameDirty = true;
}

void D3D11Renderer::RenderNV12Cpu(const uint8_t* nv12, uint32_t width, uint32_t height, uint32_t pitch) {
    if (!nv12 || !m_context) return;

    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (!EnsureVideoTexture(width, height)) return;

    // NV12 single subresource: Y plane then UV plane, both at the given row pitch
    m_context->UpdateSubresource(m_videoTexture.Get(), 0, nullptr, nv12, pitch, 0);
    m_hasFrame = true;
    m_frameDirty = true;
}

bool D3D11Renderer::Present() {
    if (!m_swapChain || !m_renderTargetView) return false;
    if (!m_frameDirty && !m_presentForced) return false; // nothing new: skip present

    std::lock_guard<std::mutex> lock(m_frameMutex);

    float clearColor[4] = { 0.05f, 0.05f, 0.08f, 1.0f };
    m_context->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);

    if (m_hasFrame && m_videoTexture && m_videoWidth > 0 && m_videoHeight > 0) {
        // Aspect-preserved (letterboxed) viewport
        float windowAspect = static_cast<float>(m_width) / m_height;
        float videoAspect = static_cast<float>(m_videoWidth) / m_videoHeight;
        float vpW = static_cast<float>(m_width), vpH = static_cast<float>(m_height);
        float vpX = 0.0f, vpY = 0.0f;
        if (windowAspect > videoAspect) {
            vpW = m_height * videoAspect;
            vpX = (m_width - vpW) * 0.5f;
        } else {
            vpH = m_width / videoAspect;
            vpY = (m_height - vpH) * 0.5f;
        }

        D3D11_VIEWPORT vp = {};
        vp.Width = vpW;
        vp.Height = vpH;
        vp.TopLeftX = vpX;
        vp.TopLeftY = vpY;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &vp);

        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        m_context->IASetInputLayout(m_inputLayout.Get());
        m_context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { m_luminanceSRV.Get(), m_chrominanceSRV.Get() };
        m_context->PSSetShaderResources(0, 2, srvs);
        m_context->PSSetSamplers(0, 1, m_samplerState.GetAddressOf());
        m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), nullptr);
        m_context->Draw(4, 0);
    }

    m_swapChain->Present(m_vsync ? m_syncInterval : 0, 0);
    m_frameDirty = false;
    m_presentForced = false;
    return true;
}
