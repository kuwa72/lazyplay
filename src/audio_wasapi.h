#ifndef AUDIO_WASAPI_H
#define AUDIO_WASAPI_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

// Minimal WASAPI shared-mode player: accepts interleaved s16 stereo PCM at
// 44100 Hz, converts to the device mix format (resampling if needed), and
// renders through the default audio endpoint.
class WasapiPlayer {
public:
    WasapiPlayer();
    ~WasapiPlayer();

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
};

#endif // AUDIO_WASAPI_H
