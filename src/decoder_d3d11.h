#ifndef DECODER_D3D11_H
#define DECODER_D3D11_H

#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

class D3D11H264Decoder {
public:
    D3D11H264Decoder();
    ~D3D11H264Decoder();

    bool Initialize(ID3D11Device* d3d11Device, uint32_t width = 1280, uint32_t height = 720);
    void Shutdown();

    // Decode NAL unit packet (H.264 Annex-B or AVCC format)
    bool DecodePacket(const uint8_t* data, size_t size, std::vector<uint8_t>& outFrame, uint32_t& outWidth, uint32_t& outHeight);

private:
    bool SetupMFT();

    bool m_initialized = false;
    uint32_t m_width = 1280;
    uint32_t m_height = 720;

    ComPtr<IMFTransform> m_mftDecoder;
    ComPtr<IMFDXGIDeviceManager> m_dxgiManager;
    UINT m_resetToken = 0;
};

#endif // DECODER_D3D11_H
