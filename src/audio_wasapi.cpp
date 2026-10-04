#include "audio_wasapi.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace {

const size_t RING_FRAMES = 44100; // 1 second of stereo audio

std::wstring DeviceId(IMMDevice* device) {
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) return L"";
    std::wstring result = id;
    CoTaskMemFree(id);
    return result;
}

std::wstring FriendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) return L"";
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
        name = pv.pwszVal;
    PropVariantClear(&pv);
    return name;
}

std::wstring DefaultDeviceId(IMMDeviceEnumerator* enumerator) {
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return L"";
    return DeviceId(device.Get());
}

bool DeviceActive(IMMDevice* device) {
    DWORD state = 0;
    return SUCCEEDED(device->GetState(&state)) && state == DEVICE_STATE_ACTIVE;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

} // namespace

std::vector<WasapiEndpoint> WasapiPlayer::EnumerateEndpoints() {
    std::vector<WasapiEndpoint> out;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = (hr == S_OK || hr == S_FALSE);

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 IID_PPV_ARGS(&enumerator)))) {
        ComPtr<IMMDeviceCollection> collection;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
            UINT count = 0;
            collection->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                ComPtr<IMMDevice> device;
                if (FAILED(collection->Item(i, &device))) continue;
                WasapiEndpoint ep{DeviceId(device.Get()), FriendlyName(device.Get())};
                if (!ep.id.empty()) out.push_back(std::move(ep));
            }
        }
    }

    if (uninit) CoUninitialize();
    return out;
}

void WasapiPlayer::SelectEndpoint(const std::wstring& id) {
    {
        std::lock_guard<std::mutex> lock(s_selMutex);
        s_selectedId = id;
    }
    ++s_generation;
}

std::wstring WasapiPlayer::SelectedEndpointId() {
    std::lock_guard<std::mutex> lock(s_selMutex);
    return s_selectedId;
}

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

    // Outer loop: one iteration per endpoint session. Re-entered when the
    // selected endpoint changes, the system default moves, or a pinned device
    // is removed/invalidated. Failures while opening an endpoint just retry.
    while (m_running) {
        const uint64_t gen = s_generation.load();
        const std::wstring want = SelectedEndpointId();

        ComPtr<IMMDevice> device;
        if (want.empty()) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        } else {
            hr = enumerator->GetDevice(want.c_str(), &device);
            if (SUCCEEDED(hr) && !DeviceActive(device.Get())) hr = E_FAIL;
        }
        if (FAILED(hr)) {
            // Pinned device unplugged (or no default endpoint): silent retry.
            Sleep(500);
            continue;
        }
        const std::wstring curId = DeviceId(device.Get());
        const std::wstring curName = FriendlyName(device.Get());

        ComPtr<IAudioClient> client;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);
        HANDLE audioEvent = SUCCEEDED(hr) ? CreateEvent(nullptr, FALSE, FALSE, nullptr) : nullptr;
        REFERENCE_TIME bufferDuration = 300000; // 30 ms in 100 ns units
        if (SUCCEEDED(hr) && audioEvent) {
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    bufferDuration, 0, mix, nullptr);
        }
        ComPtr<IAudioRenderClient> renderClient;
        uint32_t bufferFrames = 0;
        if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&renderClient));
        if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
        if (FAILED(hr) || !audioEvent || !mix) {
            std::cerr << "[Audio] endpoint init failed: 0x" << std::hex << hr << std::dec << std::endl;
            if (mix) CoTaskMemFree(mix);
            if (audioEvent) CloseHandle(audioEvent);
            Sleep(500);
            continue;
        }
        client->SetEventHandle(audioEvent);

        const uint32_t deviceRate = mix->nSamplesPerSec;
        const uint32_t channels = mix->nChannels;
        client->Start();
        std::cout << "[Audio] WASAPI rendering on '" << WideToUtf8(curName) << "': "
                  << deviceRate << " Hz, " << channels << " ch"
                  << (deviceRate != 44100 ? " (resampling from 44100)" : "")
                  << " " << WfxTag(mix) << std::endl;

        // Drop audio buffered while the endpoint was unavailable or being switched.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_ringRead = m_ringWrite = m_ringCount = 0;
        }

        // 44.1k -> device rate linear resampler state
        double resamplePos = 0.0;
        const double step = 44100.0 / deviceRate;

        std::vector<int16_t> chunk;   // pending tail + freshly popped source samples
        std::vector<int16_t> pending;
        std::vector<float> outBuf;

        bool rearm = false; // leave the pump loop to (re)open the endpoint
        auto nextWatch = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        uint64_t pumpCount = 0;
        while (m_running && !rearm) {
            // Hybrid mode: event wakes us early if supported, 10ms polling otherwise
            WaitForSingleObject(audioEvent, 10);

            uint32_t padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) { rearm = true; break; }
            uint32_t available = bufferFrames - padding;
            if (available == 0) goto watch;

            {
                BYTE* dest = nullptr;
                if (FAILED(renderClient->GetBuffer(available, &dest)) || !dest) { rearm = true; break; }

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

        watch:
            if (std::chrono::steady_clock::now() >= nextWatch) {
                nextWatch = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                if (s_generation.load() != gen) {
                    rearm = true; // user picked another output
                } else if (want.empty()) {
                    rearm = (DefaultDeviceId(enumerator.Get()) != curId); // follow default device
                } else {
                    rearm = !DeviceActive(device.Get()); // pinned device went away
                }
            }
        }

        client->Stop();
        CoTaskMemFree(mix);
        CloseHandle(audioEvent);
        std::cout << "[Audio] Endpoint session ended." << std::endl;
    }

    CoUninitialize();
    std::cout << "[Audio] Renderer stopped." << std::endl;
}
