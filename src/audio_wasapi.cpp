#include "audio_wasapi.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace {

const size_t RING_FRAMES = 44100; // 1 second of stereo audio

} // namespace

WasapiPlayer::WasapiPlayer() {
    m_ring.resize(RING_FRAMES * 2);
}

WasapiPlayer::~WasapiPlayer() {
    Stop();
}

void WasapiPlayer::SetVolumeDb(float db) {
    if (db <= -144.0f) {
        m_gain.store(0.0f);
    } else {
        m_gain.store(powf(10.0f, db / 20.0f));
    }
    std::cout << "[Audio] volume set to " << db << " dB, gain=" << m_gain.load() << std::endl;
}

static std::string WfxTag(const WAVEFORMATEX* mix) {
    if (!mix) return "null";
    std::ostringstream s;
    s << "tag=" << mix->wFormatTag << " rate=" << mix->nSamplesPerSec
      << " ch=" << mix->nChannels << " bits=" << mix->wBitsPerSample;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
        auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
        s << " subfmt=" << std::hex << ext->SubFormat.Data1;
    }
    return s.str();
}

void WasapiPlayer::PushPcm(const int16_t* pcm, size_t frames) {
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t samples = frames * 2;
    int16_t peak = 0;
    for (size_t i = 0; i < samples; ++i) {
        int16_t s = pcm[i];
        if (s > peak || s < -peak) peak = s > 0 ? s : -s;
    }
    static uint64_t pushCnt = 0;
    if (++pushCnt <= 3 || pushCnt % 100 == 0) {
        std::cout << "[Audio] PushPcm frames=" << frames << " peak=" << peak << std::endl;
    }

    // Drop oldest on overflow (slow renderer must not stall the network thread)
    if (m_ringCount + samples > m_ring.size()) {
        size_t drop = m_ringCount + samples - m_ring.size();
        m_ringRead = (m_ringRead + drop) % m_ring.size();
        m_ringCount -= drop;
    }
    for (size_t i = 0; i < samples; ++i) {
        m_ring[m_ringWrite] = pcm[i];
        m_ringWrite = (m_ringWrite + 1) % m_ring.size();
    }
    m_ringCount += samples;
}

bool WasapiPlayer::Start() {
    m_running = true;
    m_thread = std::thread(&WasapiPlayer::ThreadMain, this);
    return true;
}

void WasapiPlayer::Stop() {
    if (m_running) {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
    }
}

void WasapiPlayer::ThreadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) { std::cerr << "[Audio] MMDeviceEnumerator failed." << std::endl; return; }

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) { std::cerr << "[Audio] No default audio endpoint." << std::endl; return; }

    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr)) { std::cerr << "[Audio] IAudioClient activate failed." << std::endl; return; }

    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) { std::cerr << "[Audio] GetMixFormat failed." << std::endl; return; }

    const uint32_t deviceRate = mix->nSamplesPerSec;
    const bool isFloat = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                         (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE);
    const uint32_t channels = mix->nChannels;

    HANDLE audioEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    REFERENCE_TIME bufferDuration = 300000; // 30 ms in 100 ns units
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            bufferDuration, 0, mix, nullptr);
    if (FAILED(hr)) {
        std::cerr << "[Audio] IAudioClient Initialize failed: 0x" << std::hex << hr << std::dec << std::endl;
        CoTaskMemFree(mix);
        CloseHandle(audioEvent);
        return;
    }
    client->SetEventHandle(audioEvent);

    ComPtr<IAudioRenderClient> renderClient;
    hr = client->GetService(IID_PPV_ARGS(&renderClient));
    if (FAILED(hr)) { std::cerr << "[Audio] GetService render failed." << std::endl; return; }

    uint32_t bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);
    client->Start();
    std::cout << "[Audio] WASAPI rendering: " << deviceRate << " Hz, " << channels << " ch"
              << (deviceRate != 44100 ? " (resampling from 44100)" : "") << " " << WfxTag(mix) << std::endl;

    hr = client->SetEventHandle(audioEvent);
    if (FAILED(hr)) {
        std::cerr << "[Audio] SetEventHandle failed: 0x" << std::hex << hr << std::dec << std::endl;
    }

    // 44.1k -> device rate linear resampler state
    double resamplePos = 0.0;
    const double step = 44100.0 / deviceRate;

    std::vector<int16_t> chunk;   // pending tail + freshly popped source samples
    std::vector<int16_t> pending;
    std::vector<float> outBuf;

    uint64_t pumpCount = 0;
    while (m_running) {
        // Hybrid mode: event wakes us early if supported, 10ms polling otherwise
        WaitForSingleObject(audioEvent, 10);

        uint32_t padding = 0;
        client->GetCurrentPadding(&padding);
        uint32_t available = bufferFrames - padding;
        if (available == 0) continue;

        BYTE* dest = nullptr;
        if (FAILED(renderClient->GetBuffer(available, &dest)) || !dest) continue;

        // Pull enough source frames for `available` output frames
        size_t needSrc = static_cast<size_t>(available * step) + 4;
        size_t pendingSamples = pending.size();
        chunk.assign(pending.begin(), pending.end());
        chunk.resize(pendingSamples + needSrc * 2);
        size_t gotSamples = 0;
        size_t ringCountSnap = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            gotSamples = (m_ringCount < needSrc * 2) ? m_ringCount : needSrc * 2;
            gotSamples &= ~size_t(1); // keep stereo alignment
            for (size_t i = 0; i < gotSamples; ++i) {
                chunk[pendingSamples + i] = m_ring[m_ringRead];
                m_ringRead = (m_ringRead + 1) % m_ring.size();
            }
            m_ringCount -= gotSamples;
            ringCountSnap = m_ringCount;
        }
        size_t srcFrames = (pendingSamples + gotSamples) / 2;

        float gain = m_gain.load();
        outBuf.resize(static_cast<size_t>(available) * channels);
        float maxAbs = 0.0f;

        // Don't let an earlier source underflow leave resamplePos far beyond the source.
        if (resamplePos >= static_cast<double>(srcFrames)) resamplePos = 0.0;

        bool hasSrc = (srcFrames > 0 && resamplePos < static_cast<double>(srcFrames));
        for (uint32_t i = 0; i < available; ++i) {
            float l = 0.0f, r = 0.0f;
            if (hasSrc) {
                size_t idx = static_cast<size_t>(resamplePos);
                float frac = static_cast<float>(resamplePos - idx);
                if (idx + 1 < srcFrames) {
                    float l0 = chunk[idx * 2], l1 = chunk[idx * 2 + 2];
                    float r0 = chunk[idx * 2 + 1], r1 = chunk[idx * 2 + 3];
                    l = l0 + (l1 - l0) * frac;
                    r = r0 + (r1 - r0) * frac;
                } else if (idx < srcFrames) {
                    l = chunk[idx * 2];
                    r = chunk[idx * 2 + 1];
                }
                resamplePos += step;
                if (resamplePos >= static_cast<double>(srcFrames)) hasSrc = false;
            }
            float* out = outBuf.data() + i * channels;
            out[0] = l * gain / 32768.0f;
            if (channels >= 2) out[1] = r * gain / 32768.0f;
            for (uint32_t c = 2; c < channels; ++c) out[c] = 0.0f;
            float peak = std::max(std::abs(out[0]), std::abs(channels >= 2 ? out[1] : 0.0f));
            if (i == 0 && pumpCount <= 3) {
                std::cout << "[Audio] out0=" << out[0] << " out1=" << (channels >= 2 ? out[1] : 0.0f)
                          << " l=" << l << " r=" << r << " hasSrc=" << hasSrc << std::endl;
            }
            maxAbs = std::max(maxAbs, peak);
        }

        // carry the unconsumed tail (and fractional position) to the next call
        size_t consumed = (resamplePos < static_cast<double>(srcFrames))
                           ? static_cast<size_t>(resamplePos)
                           : srcFrames;
        if (consumed >= srcFrames) {
            resamplePos = 0.0;
            pending.clear();
        } else {
            pending.assign(chunk.begin() + consumed * 2, chunk.begin() + srcFrames * 2);
            resamplePos -= consumed;
        }

        memcpy(dest, outBuf.data(), available * channels * sizeof(float));
        renderClient->ReleaseBuffer(available, 0);
        if (++pumpCount <= 3 || pumpCount % 50 == 0) {
            int16_t c0 = (srcFrames > 0) ? chunk[0] : 0;
            int16_t c1 = (srcFrames > 1) ? chunk[2] : 0;
            std::cout << "[Audio] pump #" << pumpCount << " avail=" << available
                      << " ring=" << ringCountSnap << " src=" << srcFrames
                      << " gain=" << gain << " maxAbs=" << maxAbs
                      << " chunk0=" << c0 << " chunk1=" << c1 << std::endl;
        }
    }

    client->Stop();
    CoTaskMemFree(mix);
    CloseHandle(audioEvent);
    CoUninitialize();
    std::cout << "[Audio] Renderer stopped." << std::endl;
}
