#include <initguid.h>
#include <wmcodecdsp.h>
#include "decoder_d3d11.h"
#include <codecapi.h>
#include <iostream>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

D3D11H264Decoder::D3D11H264Decoder() {}

D3D11H264Decoder::~D3D11H264Decoder() {
    Shutdown();
}

bool D3D11H264Decoder::Initialize(ID3D11Device* d3d11Device) {
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::cerr << "[Decoder] MFStartup failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    if (d3d11Device) {
        hr = MFCreateDXGIDeviceManager(&m_resetToken, &m_dxgiManager);
        if (FAILED(hr)) {
            std::cerr << "[Decoder] MFCreateDXGIDeviceManager failed: 0x" << std::hex << hr << std::endl;
        } else {
            hr = m_dxgiManager->ResetDevice(d3d11Device, m_resetToken);
            if (FAILED(hr)) {
                std::cerr << "[Decoder] DXGI Device Manager ResetDevice failed: 0x" << std::hex << hr << std::endl;
                m_dxgiManager.Reset();
            }
        }
    } else {
        std::cerr << "[Decoder] No D3D11 device supplied." << std::endl;
    }

    if (!SetupMFT()) {
        std::cerr << "[Decoder] Failed to setup Media Foundation H.264 MFT." << std::endl;
        return false;
    }

    m_initialized = true;
    std::cout << "[Decoder] D3D11 / DXVA2 Hardware H.264 Decoder initialized"
              << (m_dxgiManager ? " (GPU output)" : "") << std::endl;
    return true;
}

void D3D11H264Decoder::Shutdown() {
    if (m_initialized) {
        m_mftDecoder.Reset();
        m_dxgiManager.Reset();
        MFShutdown();
        m_initialized = false;
    }
}

bool D3D11H264Decoder::SetupMFT() {
    HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&m_mftDecoder));
    if (FAILED(hr)) {
        std::cerr << "[Decoder] H.264 decoder MFT not available: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // Low latency mode: emit frames without internal buffering/reordering delay
    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(m_mftDecoder->QueryInterface(IID_ICodecAPI, &codecApi))) {
        VARIANT v;
        v.vt = VT_UI4;
        v.ulVal = 1;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }

    if (m_dxgiManager) {
        m_mftDecoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, ULONG_PTR(m_dxgiManager.Get()));
    }

    // Input type: H.264 (Annex-B elementary stream, in-band SPS/PPS)
    ComPtr<IMFMediaType> inputType;
    MFCreateMediaType(&inputType);
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, m_width, m_height);

    hr = m_mftDecoder->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr)) {
        std::cerr << "[Decoder] SetInputType failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    if (!SetOutputType()) {
        return false;
    }

    m_mftDecoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    m_mftDecoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_mftDecoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    return true;
}

bool D3D11H264Decoder::SetOutputType() {
    // Prefer NV12 (native decoder output, zero colorspace conversion)
    for (DWORD i = 0; ; ++i) {
        ComPtr<IMFMediaType> outputType;
        HRESULT hr = m_mftDecoder->GetOutputAvailableType(0, i, &outputType);
        if (FAILED(hr)) break;

        GUID subtype = {};
        outputType->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == MFVideoFormat_NV12) {
            hr = m_mftDecoder->SetOutputType(0, outputType.Get(), 0);
            if (SUCCEEDED(hr)) {
                UINT64 frameSize = 0;
                outputType->GetUINT64(MF_MT_FRAME_SIZE, &frameSize);
                m_width = static_cast<uint32_t>(frameSize >> 32);
                m_height = static_cast<uint32_t>(frameSize & 0xFFFFFFFF);
                return true;
            }
        }
    }
    std::cerr << "[Decoder] No NV12 output type available from decoder MFT." << std::endl;
    return false;
}

bool D3D11H264Decoder::Decode(const uint8_t* data, size_t size,
                              ComPtr<ID3D11Texture2D>& outTexture, uint32_t& outSubresource,
                              ComPtr<IMFSample>& outSampleHolder,
                              std::vector<uint8_t>& outCpu, uint32_t& outCpuPitch,
                              uint32_t& outWidth, uint32_t& outHeight) {
    if (!m_initialized || !data || size == 0) return false;

    ComPtr<IMFMediaBuffer> mediaBuffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &mediaBuffer);
    if (FAILED(hr)) return false;

    BYTE* pBuffer = nullptr;
    mediaBuffer->Lock(&pBuffer, nullptr, nullptr);
    memcpy(pBuffer, data, size);
    mediaBuffer->Unlock();
    mediaBuffer->SetCurrentLength(static_cast<DWORD>(size));

    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(mediaBuffer.Get());
    sample->SetSampleTime(m_sampleTime);
    m_sampleTime += 333333; // 100ns units; value itself is irrelevant for display order

    hr = m_mftDecoder->ProcessInput(0, sample.Get(), 0);

    // Drain outputs (also handles the MF_E_NOTACCEPTING retry path)
    for (int iter = 0; iter < 16; ++iter) {
        if (hr == MF_E_NOTACCEPTING) {
            // Decoder is full: drain one output below, then resubmit
        } else if (FAILED(hr)) {
            return false;
        }

        MFT_OUTPUT_DATA_BUFFER outputDataBuffer = {};
        DWORD status = 0;
        HRESULT hrOut = m_mftDecoder->ProcessOutput(0, 1, &outputDataBuffer, &status);

        if (hrOut == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (hr == MF_E_NOTACCEPTING) {
                // Fully drained and still not accepting: drop this packet
                return false;
            }
            break;
        }
        if (hrOut == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!SetOutputType()) return false;
            continue;
        }
        if (FAILED(hrOut)) {
            return false;
        }

        if (hr == MF_E_NOTACCEPTING) {
            hr = m_mftDecoder->ProcessInput(0, sample.Get(), 0);
        }

        if (outputDataBuffer.pEvents) outputDataBuffer.pEvents->Release();

        if (outputDataBuffer.pSample) {
            ComPtr<IMFSample> outSample;
            outSample.Attach(outputDataBuffer.pSample);
            if (HandleOutput(outSample, outTexture, outSubresource, outSampleHolder,
                             outCpu, outCpuPitch, outWidth, outHeight)) {
                return true;
            }
        }
    }
    return false;
}

bool D3D11H264Decoder::HandleOutput(ComPtr<IMFSample>& sample,
                                    ComPtr<ID3D11Texture2D>& outTexture, uint32_t& outSubresource,
                                    ComPtr<IMFSample>& outSampleHolder,
                                    std::vector<uint8_t>& outCpu, uint32_t& outCpuPitch,
                                    uint32_t& outWidth, uint32_t& outHeight) {
    outWidth = m_width;
    outHeight = m_height;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer))) return false;

    // GPU path: the sample carries a D3D11 texture (NV12)
    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    if (SUCCEEDED(buffer.As(&dxgiBuffer))) {
        ComPtr<ID3D11Texture2D> texture;
        UINT subresource = 0;
        if (SUCCEEDED(dxgiBuffer->GetResource(IID_PPV_ARGS(&texture)))) {
            dxgiBuffer->GetSubresourceIndex(&subresource);
            outTexture = texture;
            outSubresource = subresource;
            outSampleHolder = sample;
            return true;
        }
    }

    // CPU path: contiguous NV12 buffer
    ComPtr<IMF2DBuffer> buffer2D;
    BYTE* pData = nullptr;
    LONG pitch = 0;
    if (SUCCEEDED(buffer.As(&buffer2D)) && SUCCEEDED(buffer2D->Lock2D(&pData, &pitch))) {
        size_t total = static_cast<size_t>(pitch) * m_height * 3 / 2;
        outCpu.assign(pData, pData + total);
        outCpuPitch = static_cast<uint32_t>(pitch);
        buffer2D->Unlock2D();
        return true;
    }
    return false;
}
