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

// Hardware H.264 decoder (Windows Media Foundation MFT backed by DXVA2/D3D11).
// No software decode fallback: if the hardware MFT is unavailable, Initialize
// fails and the caller keeps displaying the idle screen (per REQUIREMENTS 2.2).
class D3D11H264Decoder {
public:
    D3D11H264Decoder();
    ~D3D11H264Decoder();

    bool Initialize(ID3D11Device* d3d11Device);
    void Shutdown();

    // Decode one Annex-B access unit. Returns true when a frame was produced.
    // GPU path: outTexture/outSubresource + outSampleHolder (keeps the MFT pool
    //   buffer alive; release after the frame has been consumed).
    // CPU path (rare; e.g. MFT refuses D3D output): outCpu receives NV12 data.
    bool Decode(const uint8_t* data, size_t size,
                ComPtr<ID3D11Texture2D>& outTexture, uint32_t& outSubresource,
                ComPtr<IMFSample>& outSampleHolder,
                std::vector<uint8_t>& outCpu, uint32_t& outCpuPitch,
                uint32_t& outWidth, uint32_t& outHeight);

private:
    bool SetupMFT();
    bool SetOutputType();
    bool HandleOutput(ComPtr<IMFSample>& sample,
                      ComPtr<ID3D11Texture2D>& outTexture, uint32_t& outSubresource,
                      ComPtr<IMFSample>& outSampleHolder,
                      std::vector<uint8_t>& outCpu, uint32_t& outCpuPitch,
                      uint32_t& outWidth, uint32_t& outHeight);

    bool m_initialized = false;
    uint32_t m_width = 1280;       // visible size (display region)
    uint32_t m_height = 720;
    uint32_t m_codedHeight = 720;  // MB-aligned buffer height (UV plane offset math)
    int64_t m_sampleTime = 0;

    ComPtr<IMFTransform> m_mftDecoder;
    ComPtr<IMFDXGIDeviceManager> m_dxgiManager;
    UINT m_resetToken = 0;
};

#endif // DECODER_D3D11_H
