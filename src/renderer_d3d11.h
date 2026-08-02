#ifndef RENDERER_D3D11_H
#define RENDERER_D3D11_H

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

class D3D11Renderer {
public:
    D3D11Renderer();
    ~D3D11Renderer();

    bool Initialize(HWND hwnd, uint32_t width, uint32_t height, bool vsync = true);
    void Cleanup();
    void Resize(uint32_t width, uint32_t height);
    void ToggleFullscreen();
    bool IsFullscreen() const { return m_isFullscreen; }

    // Direct3D11 device access for DXVA2 decoder sharing
    ID3D11Device* GetDevice() const { return m_device.Get(); }
    ID3D11DeviceContext* GetContext() const { return m_context.Get(); }

    // Render an RGBA or NV12 frame buffer
    void RenderFrame(const uint8_t* data, uint32_t width, uint32_t height, uint32_t pitch);
    void Present();

private:
    bool CreateShaderResources();

    HWND m_hwnd = nullptr;
    uint32_t m_width = 1280;
    uint32_t m_height = 720;
    bool m_vsync = true;
    bool m_isFullscreen = false;

    RECT m_windowedRect = {};

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_renderTargetView;

    ComPtr<ID3D11Texture2D> m_videoTexture;
    ComPtr<ID3D11ShaderResourceView> m_videoSRV;

    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader> m_pixelShader;
    ComPtr<ID3D11SamplerState> m_samplerState;
    ComPtr<ID3D11Buffer> m_vertexBuffer;
};

#endif // RENDERER_D3D11_H
