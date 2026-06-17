#pragma once
#include <cstdint>
#include <memory>
#include <vector>

// Wraps miniaudio's ma_engine to play decoded int16 PCM audio from AudioAsset.
// Supports headless/no-device mode for tests (init(true)).
// Sounds are identified by opaque uint64_t handles.
class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // noDevice=true: skip hardware device (test/headless mode).
    bool init(bool noDevice = false);
    void shutdown();
    bool isInitialized() const { return m_initialized; }

    // Play int16 interleaved PCM (from AudioAsset::audioData).
    // Returns 0 on failure.
    uint64_t play(const std::vector<uint8_t>& pcmData, int sampleRate, int channels,
                  float volume = 1.0f, float pitch = 1.0f, bool loop = false);

    void stop(uint64_t handle);
    void stopAll();
    bool isPlaying(uint64_t handle) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    uint64_t              m_nextHandle = 1;
    bool                  m_initialized = false;
};
