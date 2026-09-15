#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include <string>
#include <ContentManager/Assets.h>   // AudioAsset, AudioEncoding
#include <HorizonScene/AudioAttenuation.h>

namespace HE { struct AudioBusConfig; }

// Wraps miniaudio's ma_engine to play AudioAsset clips: int16 PCM straight from
// a buffer, Ogg Vorbis decoded on the fly by the mixer (a voice holds the
// compressed bytes and never the whole PCM — see play(const AudioAsset&)).
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

    // Tear a bus down. Every voice still routed through it is stopped first —
    // miniaudio would otherwise leave those sounds attached to a group that no
    // longer exists. Returns false for an unknown name.
    bool  removeBus(const std::string& name);

    // The buses that exist, sorted by name so a caller drawing them gets a
    // stable order across frames (the table behind it is unordered).
    std::vector<std::string> busNames() const;

    // Mute keeps the bus's volume and silences its group; unmute puts the
    // volume back. getBusVolume() keeps answering the REMEMBERED volume while
    // muted, so a fader does not jump to zero when its M lights up. No-op for
    // an unknown bus; isBusMuted() is false for one.
    void  setBusMuted(const std::string& name, bool muted);
    bool  isBusMuted(const std::string& name) const;

    // Gain in front of every bus and every voice — the mixer's master fader.
    // 1 when nothing was set; get returns 1 when not initialised.
    void  setMasterVolume(float volume);
    float getMasterVolume() const;

    // Voices alive on a bus right now ("" = the ones on master). Finished
    // voices are still counted until something stops or reaps them; it is what
    // the mixer shows as activity, not an exact "audible now".
    int   busVoiceCount(const std::string& name) const;

    // Bring the engine in line with a project's bus list: create what is
    // missing, set every listed volume and the master. Buses that exist here
    // but not in the config are LEFT ALONE — a script may have made them, and
    // this is called on project load and before play, not as a reset. Muting
    // is untouched too; it belongs to the editor session, not the project.
    void  applyBusConfig(const HE::AudioBusConfig& config);

    // ─── Playback ────────────────────────────────────────────────────────────

    // Play a clip asset, whatever its encoding. PCM16 copies the samples into
    // the voice (as the raw overload below); Vorbis copies only the Ogg bytes
    // and decodes them as the mixer pulls frames, so a five-minute track costs
    // its compressed size per voice, not its PCM size. Both copy because the
    // asset lives in ContentManager's dense storage, which the next loadAsset()
    // may relocate under a playing voice. Returns 0 on failure (empty data,
    // bad format, or an Ogg stream the decoder rejects).
    // busName: route through a named bus ("" = master).
    uint64_t play(const AudioAsset& clip,
                  float volume = 1.0f, float pitch = 1.0f, bool loop = false,
                  const std::string& busName = {});
    // attenuation/rolloff: the curve between minDist and maxDist — see
    // AudioAttenuation.h. Trailing with defaults so every caller that only
    // knew the linear model keeps compiling and keeps sounding the same.
    uint64_t playSpatial(const AudioAsset& clip,
                         float volume, float pitch, bool loop,
                         float x, float y, float z,
                         float minDist = 1.0f, float maxDist = 20.0f,
                         const std::string& busName = {},
                         AudioAttenuation attenuation = AudioAttenuation::Linear,
                         float rolloff = 1.0f);

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
                         const std::string& busName = {},
                         AudioAttenuation attenuation = AudioAttenuation::Linear,
                         float rolloff = 1.0f);

    // Update the world-space position of a playing spatial sound.
    void setSoundPosition(uint64_t handle, float x, float y, float z);

    // Re-shape the falloff of a spatial sound that is already running — the
    // Details panel edits a range during play and expects to hear it. No-op
    // for an unknown handle or a 2D voice; the same clamps startSound applies
    // (minDist > 0, maxDist > minDist) apply here.
    void setSoundAttenuation(uint64_t handle, AudioAttenuation model,
                             float minDist, float maxDist, float rolloff);
    // What the voice is currently attenuating with; Linear for an unknown
    // handle. Lets a test see the model landed without reaching into miniaudio.
    AudioAttenuation getSoundAttenuation(uint64_t handle) const;

    // Update the listener transform (call once per frame from AudioListener entity).
    // forward and up should be unit vectors.
    void setListenerTransform(float px, float py, float pz,
                              float fx, float fy, float fz,
                              float ux, float uy, float uz);

    void stop(uint64_t handle);
    void stopAll();
    bool isPlaying(uint64_t handle) const;

    // ─── Transport / live parameters ─────────────────────────────────────────
    // Everything below is a no-op (or 0) for an unknown handle.

    // Playback position and total length, both in SOURCE PCM frames — i.e. frames
    // of the buffer that was handed to play(), not of the engine's output rate. So
    // `cursor / asset.sampleRate` is the position in seconds no matter what pitch
    // the sound is running at, and no resampler compensation is needed anywhere.
    uint64_t getSoundCursorFrames(uint64_t handle) const;
    uint64_t getSoundLengthFrames(uint64_t handle) const;

    // Jump to `frame` (source frames, clamped by miniaudio to the buffer length).
    void seekSound(uint64_t handle, uint64_t frame);

    // Pause keeping the cursor and the decoded buffer, resume from where it left
    // off. Distinct from stop(), which tears the voice down — for a long clip
    // that difference is a multi-megabyte PCM copy per press. isPlaying() reports
    // false while paused, so a caller that reaps finished voices has to remember
    // it paused this one.
    void pauseSound(uint64_t handle);
    void resumeSound(uint64_t handle);
    // True between pauseSound() and resumeSound()/stop(). A script needs this to
    // tell a paused voice from one that has finished — isPlaying() says false
    // for both, and only one of them should be reaped.
    bool isPaused(uint64_t handle) const;

    // Change what play() set up, on a sound that is already running.
    void setSoundLooping(uint64_t handle, bool loop);
    void setSoundVolume(uint64_t handle, float volume);
    void setSoundPitch(uint64_t handle, float pitch);
    // Read back what play()/the setters above left on the voice. Unknown handle:
    // 0 for volume (silent), 1 for pitch (unchanged) — the neutral value of each.
    float getSoundVolume(uint64_t handle) const;
    float getSoundPitch(uint64_t handle) const;

    // Sample rate the playing sound is actually being fed to the mixer with, in Hz.
    // Should equal the rate passed to play()/playSpatial(); 0 = unknown handle.
    int  getSoundSampleRate(uint64_t handle) const;

    // ─── Headless mix pull ───────────────────────────────────────────────────

    // Pull `frameCount` mixed frames (f32, interleaved, the engine's channel
    // count and rate) out of the mixer into `out`, as the output device would.
    // Only meaningful in noDevice mode — with a device its thread is the one
    // pulling — and there it is what makes the streamed decode observable at
    // all: a Vorbis voice decodes exactly when frames are asked for. Returns the
    // frames written (0 when not initialised).
    uint64_t readMixedFrames(float* out, uint64_t frameCount);
    int      outputChannels() const;   // 0 when not initialised

    // ─── Offline decode ──────────────────────────────────────────────────────

    // Decode a whole clip to interleaved int16 PCM at its own rate/channels —
    // for the editor's waveform and analysis, and for tests; NOT for playback,
    // which streams. A PCM16 clip is simply copied. Works without init(): it is
    // a pure function of the bytes. Returns false (and leaves outPcm empty) on
    // an empty clip or a stream the decoder rejects.
    static bool decodeToPcm16(const AudioAsset& clip, std::vector<uint8_t>& outPcm);

private:
    // Positional setup for a spatial sound — see playSpatial().
    struct SpatialParams
    {
        float x, y, z, minDist, maxDist;
        AudioAttenuation attenuation;
        float rolloff;
    };

    // Shared body of every play variant: they only differ in the spatialization
    // flag and in the positional setup applied before the sound starts.
    // spatial == nullptr ⇒ non-spatial (play()). `bytes` are interpreted by
    // `encoding` (PCM16 needs sampleRate/channels, Vorbis carries its own).
    uint64_t startSound(const std::vector<uint8_t>& bytes, AudioEncoding encoding,
                        int sampleRate, int channels,
                        float volume, float pitch, bool loop, const std::string& busName,
                        const SpatialParams* spatial);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    uint64_t              m_nextHandle = 1;
    bool                  m_initialized = false;
};
