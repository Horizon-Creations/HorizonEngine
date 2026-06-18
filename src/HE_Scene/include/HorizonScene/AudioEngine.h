#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include <string>

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

    // ─── Bus (mixer group) management ─────────────────────────────────────────

    // Create a named bus (idempotent — re-creating is safe). Returns false on error.
    bool  createBus(const std::string& name, float volume = 1.0f);
    void  setBusVolume(const std::string& name, float volume);
    float getBusVolume(const std::string& name) const; // returns 1.0 if bus not found
    bool  hasBus(const std::string& name) const;

    // ─── Playback ────────────────────────────────────────────────────────────

    // Play non-spatial int16 interleaved PCM. Returns 0 on failure.
    // busName: route through a named bus ("" = master).
    uint64_t play(const std::vector<uint8_t>& pcmData, int sampleRate, int channels,
                  float volume = 1.0f, float pitch = 1.0f, bool loop = false,
                  const std::string& busName = {});

    // Play spatial sound at world-space position. minDist = full-volume radius,
    // maxDist = silence radius. Uses linear attenuation. Returns 0 on failure.
    // busName: route through a named bus ("" = master).
    uint64_t playSpatial(const std::vector<uint8_t>& pcmData, int sampleRate, int channels,
                         float volume, float pitch, bool loop,
                         float x, float y, float z,
                         float minDist = 1.0f, float maxDist = 20.0f,
                         const std::string& busName = {});

    // Update the world-space position of a playing spatial sound.
    void setSoundPosition(uint64_t handle, float x, float y, float z);

    // Update the listener transform (call once per frame from AudioListener entity).
    // forward and up should be unit vectors.
    void setListenerTransform(float px, float py, float pz,
                              float fx, float fy, float fz,
                              float ux, float uy, float uz);

    void stop(uint64_t handle);
    void stopAll();
    bool isPlaying(uint64_t handle) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    uint64_t              m_nextHandle = 1;
    bool                  m_initialized = false;
};
