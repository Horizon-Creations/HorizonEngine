#pragma once
#include <memory>
#include <string>

// Steam Audio (Valve, Apache-2.0) as the optional spatialiser behind miniaudio:
// HRTF-based binaural rendering of a mono voice for headphones. miniaudio stays
// the mixer and the device; this is only the DSP a spatial voice can be routed
// through instead of miniaudio's own pan + distance gain.
//
// Two switches, on purpose different in kind:
//   * Build: HE_ENABLE_STEAM_AUDIO (root CMakeLists.txt). OFF = SteamAudio.cpp
//     compiles to stubs, no phonon header is included and no library ships.
//     compiledIn() tells which one this binary is.
//   * Run time: AudioEngine::setSpatialBackend(), saved per project in
//     AudioBusConfig::spatialBackend. Only then is libphonon opened — by name
//     from the directory of the engine library (dlopen / LoadLibrary), never
//     linked, so a missing or unloadable library leaves the engine on miniaudio
//     with lastError() saying why.
//
// This header is identical with and without the SDK (pimpl, no macro-dependent
// layout), so nothing that includes it cares which way the engine was built.
namespace HE {

// A direction in the LISTENER's frame, the convention Steam Audio takes:
// +x right, +y up, -z ahead (right-handed). Need not be normalised.
struct SpatialDirection
{
    float x = 0.0f, y = 0.0f, z = -1.0f;
};

// The listener-relative direction of a source `(dx,dy,dz)` away from the listener
// (world offset = source - listener), for a listener looking along `forward` with
// `up` above it. Pure math: no SDK needed, so it is tested everywhere. A zero
// offset (the source IS the listener) gives straight ahead; a `forward`/`up`
// pair that is degenerate (zero or parallel) falls back to the world axes
// (ahead = -z, up = +y).
SpatialDirection listenerRelativeDirection(float dx, float dy, float dz,
                                           float fx, float fy, float fz,
                                           float ux, float uy, float uz);

// What Steam Audio's direct effect does to a voice before it is spatialised: the
// frequency-dependent part of the path from the source to the listener. Each switch is
// one of the project's Steam Audio options; the numbers are the per-block result of
// the simulation (air absorption from the distance, occlusion from the geometry).
struct DirectParams
{
    bool  airAbsorption = false;
    float airBands[3]   = { 1.0f, 1.0f, 1.0f };   // low / mid / high band gain, 0..1
    bool  occlusion     = false;
    float visibility    = 1.0f;                   // Steam Audio's "occlusion": 1 = clear path, 0 = fully blocked
    float transmission[3] = { 1.0f, 1.0f, 1.0f }; // what gets through the blocker, per band
    // Reverb of the room the listener is in (Steam Audio's parametric reflection effect): the
    // voice is also fed through it and the tail is mixed in front, non-directional, scaled by
    // `reverbWet`. `reverbTimes` are the RT60s (seconds) of the low / mid / high band.
    bool  reflections   = false;
    float reverbTimes[3] = { 0.5f, 0.4f, 0.3f };
    float reverbWet     = 0.0f;
};

class SteamAudio
{
public:
    SteamAudio();
    ~SteamAudio();

    SteamAudio(const SteamAudio&)            = delete;
    SteamAudio& operator=(const SteamAudio&) = delete;

    // Whether this binary contains the backend at all (HE_ENABLE_STEAM_AUDIO).
    static bool        compiledIn();
    // "4.8.1" when compiled in, "" otherwise.
    static const char* version();

    // Load the library and create the context and the default HRTF for a mixer
    // running at `sampleRate` Hz in blocks of `frameSize` frames. False — with
    // lastError() set and nothing left half-open — when not compiled in, the
    // library is missing or refuses to load, or the SDK rejects the settings.
    // Calling it again while ready is a no-op that returns true.
    bool init(int sampleRate, int frameSize);
    void shutdown();
    bool isReady() const;
    int  frameSize() const;
    const std::string& lastError() const;

    // One voice's binaural renderer. Created from a ready SteamAudio and must be
    // destroyed BEFORE shutdown() (AudioEngine owns both and tears voices down
    // first). Not thread-safe; the mixer thread that owns the voice calls it.
    class Binaural
    {
    public:
        ~Binaural();

        // `frames` mono samples in, `frames` interleaved stereo frames out
        // (2 * frames floats). `frames` must be a non-zero multiple of the
        // SteamAudio frameSize — the effect works on whole blocks and a short
        // one would shift its filter memory. False for anything else. `direct`
        // (optional) runs the direct effect (air absorption, occlusion) on the
        // voice first.
        bool process(const float* mono, float* stereoOut, int frames,
                     const SpatialDirection& direction, const DirectParams* direct = nullptr);
        // The air-absorption bands (low / mid / high, 0..1) for a source at
        // (sx,sy,sz) and a listener at (lx,ly,lz), world metres, by Steam Audio's
        // default model. All 1 (nothing absorbed) when it cannot say.
        void airAbsorption(float sx, float sy, float sz, float lx, float ly, float lz, float out[3]) const;
        // Forget the filter memory (a voice restarting from the top).
        void reset();

    private:
        friend class SteamAudio;
        Binaural();
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    // nullptr when not ready or when the effect cannot be created.
    std::unique_ptr<Binaural> createBinaural();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace HE
