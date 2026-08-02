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

bool D3D11H264Decoder::Initialize(ID3D11Device* d3d11Device, uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::cerr << "[Decoder] MFStartup failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    if (d3d11Device) {
        hr = MFCreateDXGIDeviceManager(&m_resetToken, &m_dxgiManager);
        if (SUCCEEDED(hr)) {
            hr = m_dxgiManager->ResetDevice(d3d11Device, m_resetToken);
            if (FAILED(hr)) {
                std::cerr << "[Decoder] DXGI Device Manager ResetDevice failed." << std::endl;
            }
        }
    }

    if (!SetupMFT()) {
        std::cerr << "[Decoder] Failed to setup Media Foundation H.264 MFT." << std::endl;
        return false;
    }

    m_initialized = true;
    std::cout << "[Decoder] D3D11 / DXVA2 Hardware H.264 Decoder Initialized (" 
              << width << "x" << height << ")" << std::endl;
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
        return false;
    }

    if (m_dxgiManager) {
        m_mftDecoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, ULONG_PTR(m_dxgiManager.Get()));
    }

    // Set Input Type (H.264)
    ComPtr<IMFMediaType> inputType;
    MFCreateMediaType(&inputType);
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, m_width, m_height);

    hr = m_mftDecoder->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr)) {
        return false;
    }

    // Set Output Type (NV12 or RGB32)
    ComPtr<IMFMediaType> outputType;
    for (DWORD i = 0; ; ++i) {
        hr = m_mftDecoder->GetOutputAvailableType(0, i, &outputType);
        if (FAILED(hr)) break;

        GUID subtype = {};
        outputType->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_RGB32 || subtype == MFVideoFormat_ARGB32) {
            hr = m_mftDecoder->SetOutputType(0, outputType.Get(), 0);
            if (SUCCEEDED(hr)) {
                break;
            }
        }
        outputType.Reset();
    }

    m_mftDecoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    m_mftDecoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_mftDecoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    return true;
}

bool D3D11H264Decoder::DecodePacket(const uint8_t* data, size_t size, std::vector<uint8_t>& outFrame, uint32_t& outWidth, uint32_t& outHeight) {
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

    hr = m_mftDecoder->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
        return false;
    }

    MFT_OUTPUT_DATA_BUFFER outputDataBuffer = {};
    outputDataBuffer.dwStreamID = 0;
    outputDataBuffer.pSample = nullptr;
    outputDataBuffer.dwStatus = 0;
    outputDataBuffer.pEvents = nullptr;

    DWORD status = 0;
    hr = m_mftDecoder->ProcessOutput(0, 1, &outputDataBuffer, &status);
    if (SUCCEEDED(hr) && outputDataBuffer.pSample) {
        ComPtr<IMFMediaBuffer> outMediaBuffer;
        outputDataBuffer.pSample->ConvertToContiguousBuffer(&outMediaBuffer);

        BYTE* pData = nullptr;
        DWORD maxLength = 0, currentLength = 0;
        if (SUCCEEDED(outMediaBuffer->Lock(&pData, &maxLength, &currentLength))) {
            outFrame.assign(pData, pData + currentLength);
            outMediaBuffer->Unlock();
            outWidth = m_width;
            outHeight = m_height;
        }

        outputDataBuffer.pSample->Release();
        return true;
    }

    return false;
}
