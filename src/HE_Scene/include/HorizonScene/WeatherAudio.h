#pragma once
#include <HorizonScene/Components/WeatherComponent.h>
#include <Types/UUID.h>

#include <array>
#include <cstdint>
#include <random>
#include <vector>

class AudioEngine;
class ContentManager;
class HorizonWorld;
struct EnvironmentComponent;

// The sound of the weather. WeatherSystem decides what the sky looks like; this
// decides what it sounds like, from the same live values, so gameplay code never
// has to start, fade or stop a rain loop.
//
//   * Four looping beds, one per kind of weather: rain, wind, snow, storm. Each has
//     a gain that follows the weather — rain and snow follow the amount that is
//     falling, wind follows the wind speed (with gusts), storm follows wind x rain.
//     A bed is a voice that exists only while it is audible. Gains are
//     sqrt-shaped, so a rain -> snow change is an equal-power cross-fade, and a
//     short ramp on top removes the steps a slider or a pause would make.
//   * Thunder, a one-shot per lightning strike, arriving after a random distance
//     (343 m/s) and quieter the farther it was.
//
// Which clip plays is WeatherComponent's business: a null slot is the EngineContent
// default (DefaultAssets.h, kEngineWeather*SoundId), a set one overrides that sound
// alone. A clip that cannot be found makes that bed silent (one log line), never an
// error.
//
// The voices belong to the application's State, not to the component: the component
// is copied by value (scene load, root migration), and a handle in a copy would be
// a voice nobody can stop.
//
// The curves are plain functions of floats so they can be tested without an audio
// device; update() is the only part that talks to the AudioEngine.
namespace WeatherAudio
{
    enum class Layer : uint8_t { Rain = 0, Wind, Snow, Storm, Count };
    inline constexpr int kLayerCount = static_cast<int>(Layer::Count);

    const char* layerName(Layer layer);

    // ── Tuning ──────────────────────────────────────────────────────────────
    inline constexpr float kHeadroom        = 0.8f;    // all beds at once must not clip
    inline constexpr float kRiseSeconds     = 1.2f;    // 0 -> 1 when a bed gets louder
    inline constexpr float kFallSeconds     = 2.0f;    // 1 -> 0 when it gets quieter
    inline constexpr float kMuteFadeSeconds = 0.3f;    // 1 -> 0 on pause / switched off
    inline constexpr float kSilent          = 0.002f;  // at or below this a bed has no voice
    inline constexpr float kRetrySeconds    = 1.0f;    // between looks for a missing clip

    inline constexpr float kSpeedOfSound          = 343.0f;   // m/s
    inline constexpr float kThunderMinDistance    = 150.0f;   // m, a strike this close is a crack
    inline constexpr float kThunderMaxDistance    = 2400.0f;  // m, a rumble seven seconds late
    inline constexpr float kThunderReferenceDist  = 300.0f;   // m, full level up to here
    inline constexpr float kThunderFloorLevel     = 0.1f;     // the farthest strike is still heard
    inline constexpr int   kMaxPendingStrikes     = 6;        // strikes waiting for their sound
    inline constexpr int   kMaxThunderVoices      = 3;        // rolls ringing at once

    // ── Curves (pure) ───────────────────────────────────────────────────────

    // Amount 0..1 -> loudness 0..1. sqrt, so two beds whose amounts add up to one
    // (rain fading to snow) keep a constant power; a smooth knee below 0.1 keeps a
    // last trace of rain from hissing.
    float loudness(float amount);

    // Wind speed multiplier (WeatherPreset::windSpeed, 1 = calm) -> amount 0..1.
    // At or below 1 the sky is calm and silent; the storm preset (2.6) is 1.
    float windAmount(float windSpeed);

    // How stormy it is: wind well above the rain preset's, with rain to go with it.
    // 0 for rain and calm, 1 for the storm preset.
    float stormAmount(float rainAmount, float windSpeed);

    // Slow swell of the wind, in [0.82, 1] as a function of the weather clock.
    float windGust(float weatherTime);

    // The target gain of each bed right now, volume and the enabled switch
    // included. env is the scene's sky when there is one: its rain/snow amount are
    // what the weather writes (and what a slider can override), so what you see
    // fall is what you hear. Without a sky only the wind is left to hear.
    struct LoopGains
    {
        float rain = 0.0f, wind = 0.0f, snow = 0.0f, storm = 0.0f;
        float of(Layer layer) const;
    };
    LoopGains loopGains(const WeatherComponent& weather, const EnvironmentComponent* env);

    // Move `current` toward `target` at a constant rate: a full 0 -> 1 takes
    // riseSeconds, a full 1 -> 0 takes fallSeconds. Never overshoots.
    float slew(float current, float target, float dt, float riseSeconds, float fallSeconds);

    // Seconds the sound of a strike `distance` metres away takes to arrive.
    float thunderDelay(float distance);
    // 1 up to the reference distance, then 1/distance, never below the floor.
    float thunderLevel(float distance);

    struct Strike { float distance = 0.0f; float delay = 0.0f; float level = 1.0f; };
    // One random strike: a distance in [kThunderMinDistance, kThunderMaxDistance]
    // and the delay and level that follow from it.
    Strike makeStrike(std::mt19937& rng);

    // The clip a layer plays: the component's own slot, else the engine default.
    HE::UUID soundFor(const WeatherComponent& weather, Layer layer);
    HE::UUID thunderSoundFor(const WeatherComponent& weather);

    // ── Runtime ─────────────────────────────────────────────────────────────

    struct Frame
    {
        float realDt  = 0.0f;   // wall-clock: ramps keep running while the game is paused
        float gameDt  = 0.0f;   // scaled: when a strike's sound arrives; 0 while paused
        bool  audible = true;   // false: fade everything out (paused, switched off)
    };

    // What the application keeps between frames: the voices it started and the ramps
    // they are on.
    struct State
    {
        // seed 0 = a different thunder every run; a test passes its own.
        explicit State(uint32_t seed = 0);

        struct Bed
        {
            uint64_t handle  = 0;          // 0 = no voice
            HE::UUID asset;                // the clip the voice plays
            float    gain    = 0.0f;       // where the ramp is now
            float    applied = -1.0f;      // what the voice was last told
            float    retry   = 0.0f;       // seconds until the next look for the clip
            HE::UUID warned;               // the clip we already complained about
        };
        struct Pending { float delay = 0.0f; float level = 1.0f; };

        std::array<Bed, kLayerCount> beds {};
        std::vector<Pending>         pending;        // strikes whose sound is still on its way
        std::vector<uint64_t>        thunder;        // rolls that are ringing
        uint32_t                     seenStrikes = 0;
        bool                         seenValid   = false;
        HE::UUID                     thunderWarned;
        std::mt19937                 rng;
    };

    // Advance the weather's sound by one frame. Call it after the weather tick, in
    // the editor (edit mode too) and in the game. No-op while the engine has no
    // device; stops everything when the world has no WeatherComponent.
    void update(State& state, HorizonWorld& world, AudioEngine& engine,
                ContentManager& content, const Frame& frame);

    // Stop every voice and forget the ramps (play stopped, scene closed).
    void stop(State& state, AudioEngine& engine);
}
