#ifndef RENDERER_D3D11_H
#define RENDERER_D3D11_H

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <mutex>
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
    void CycleFullscreenMonitor();
    bool IsFullscreen() const { return m_isFullscreen; }

    // Direct3D11 device access for DXVA decoder sharing
    ID3D11Device* GetDevice() const { return m_device.Get(); }
    ID3D11DeviceContext* GetContext() const { return m_context.Get(); }

    // Submit a decoded NV12 frame living in a D3D11 texture (e.g. MFT decoder output).
    // Performs a GPU-side copy; the source may be reused by the caller afterwards.
    void RenderNV12Frame(ID3D11Texture2D* srcTexture, uint32_t srcSubresource, uint32_t width, uint32_t height);

    // Submit a decoded NV12 frame living in system memory (Y plane followed by
    // interleaved UV plane, both with the given row pitch).
    void RenderNV12Cpu(const uint8_t* nv12, uint32_t width, uint32_t height, uint32_t pitch);

    // Clear + draw the latest video frame (aspect-preserved) + present.
    // Skips the present entirely when no new frame arrived (power saving).
    // Returns true when a frame was actually presented.
    bool Present();

    // VSync sync interval (1 = every vblank, 2 = every other, ...). 0 = no vsync.
    void SetSyncInterval(uint32_t interval) { m_syncInterval = interval; }

    // Client-reported visible (display) size. The coded NV12 frame may be
    // macroblock-padded (e.g. 640x368 for 360p); when a hint is set and is
    // consistent with the coded size, sampling is cropped to the visible region.
    void SetDisplayHint(uint32_t width, uint32_t height);

    // Diagnostics/tests: the current NV12 video texture (may be null before the first frame)
    ID3D11Texture2D* GetVideoTexture() const { return m_videoTexture.Get(); }
    void GetVideoSize(uint32_t& w, uint32_t& h) const { w = m_videoWidth; h = m_videoHeight; }

private:
    bool CreateShaderResources();
    bool EnsureVideoTexture(uint32_t width, uint32_t height);
    void UpdateVertexBuffer(float texU, float texV);

    HWND m_hwnd = nullptr;
    uint32_t m_width = 1280;   // window client size
    uint32_t m_height = 720;
    bool m_vsync = true;
    uint32_t m_syncInterval = 1;
    bool m_isFullscreen = false;
    bool m_frameDirty = false;   // new frame submitted since last present
    bool m_presentForced = true; // redraw needed (startup, resize)

    RECT m_windowedRect = {};

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_renderTargetView;

    // NV12 video frame texture + per-plane views
    ComPtr<ID3D11Texture2D> m_videoTexture;
    ComPtr<ID3D11ShaderResourceView> m_luminanceSRV;   // DXGI_FORMAT_R8_UNORM
    ComPtr<ID3D11ShaderResourceView> m_chrominanceSRV; // DXGI_FORMAT_R8G8_UNORM
    uint32_t m_videoWidth = 0;   // coded (buffer) size
    uint32_t m_videoHeight = 0;
    uint32_t m_visibleWidth = 0;  // display region within the coded frame
    uint32_t m_visibleHeight = 0;
    uint32_t m_hintWidth = 0;     // client-reported display size (0 = none)
    uint32_t m_hintHeight = 0;
    bool m_hasFrame = false;

    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader> m_pixelShader;
    ComPtr<ID3D11InputLayout> m_inputLayout;
    ComPtr<ID3D11SamplerState> m_samplerState;
    ComPtr<ID3D11Buffer> m_vertexBuffer;

    std::mutex m_frameMutex; // guards video texture vs Present()
};

#endif // RENDERER_D3D11_H
