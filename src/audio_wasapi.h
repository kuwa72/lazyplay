#ifndef AUDIO_WASAPI_H
#define AUDIO_WASAPI_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

// One active render endpoint, used to build the output-device menu.
struct WasapiEndpoint {
    std::wstring id;    // WASAPI endpoint ID
    std::wstring name;  // friendly name for display
};

// Minimal WASAPI shared-mode player: accepts interleaved s16 stereo PCM at
// 44100 Hz, converts to the device mix format (resampling if needed), and
// renders through the selected audio endpoint (system default by default).
class WasapiPlayer {
public:
    WasapiPlayer();
    ~WasapiPlayer();

    // Shared output-device selection; applies to every player instance and is
    // picked up by a running render thread within ~500 ms. An empty id selects
    // the system default endpoint and follows default-device changes; a
    // non-empty id pins output to that endpoint and silently retries until the
    // device reappears.
    static std::vector<WasapiEndpoint> EnumerateEndpoints();
    static void SelectEndpoint(const std::wstring& id);
    static std::wstring SelectedEndpointId();

    bool Start();
    void Stop();

    // Feed PCM (called from the audio receiver thread)
    void PushPcm(const int16_t* pcm, size_t frames); // frames = samples per channel

    // AirPlay volume: dB, typically -30..0 (-144 = mute)
    void SetVolumeDb(float db);

private:
    void ThreadMain();

    std::atomic<bool> m_running{false};
    std::thread m_thread;

    std::mutex m_mutex;
    std::vector<int16_t> m_ring; // interleaved stereo s16
    size_t m_ringRead = 0;
    size_t m_ringWrite = 0;
    size_t m_ringCount = 0;

    std::atomic<float> m_gain{1.0f};

    inline static std::wstring s_selectedId;
    inline static std::mutex s_selMutex;
    inline static std::atomic<uint64_t> s_generation{0};
};

#endif // AUDIO_WASAPI_H
