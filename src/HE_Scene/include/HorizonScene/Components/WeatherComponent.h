#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <random>
#include "ParticleSystemComponent.h" // for Particle (precipitation reuses the pool type)

// Weather controller, authored on the World root entity alongside EnvironmentComponent.
// WeatherSystem blends the active weather toward `targetKind` over `transitionDuration`
// and writes the resulting sky parameters (cloud coverage, fog density, wind speed) into
// the EnvironmentComponent — exactly how AnimatorComponent/AnimationSystem drives
// SkeletalMeshComponent::boneMatrices. Those EnvironmentComponent fields therefore become
// weather-driven (the inspector shows them read-only while a WeatherComponent is present).
// It also produces a precipitation intensity/type (rain/snow) and storm state (lightning)
// consumed by the precipitation emitter (Phase 2) and the renderer flash (Phase 3).

enum class WeatherKind : uint8_t
{
    Clear = 0,
    Cloudy,
    Overcast,
    Foggy,
    Rain,
    Storm,
    Snow,
    Count
};

enum class PrecipType : uint8_t { None = 0, Rain = 1, Snow = 2 };

// Target sky state for one weather kind. WeatherSystem lerps between two of these.
struct WeatherPreset
{
    float      cloudCoverage = 0.0f;   // 0 = clear … 1 = overcast
    float      fogDensity    = 0.0f;   // 0 = off … ~0.12
    float      windSpeed     = 1.0f;   // cloud-drift multiplier (1 = calm)
    float      precip        = 0.0f;   // 0..1 precipitation strength
    PrecipType precipType    = PrecipType::None;
    float      wetness       = 0.0f;   // 0..1 surface wetness (Phase 4, reserved)
    bool       lightning     = false;  // storm lightning enabled (Phase 3)
};

// Built-in preset table + a display name for each kind (defined in WeatherSystem.cpp).
WeatherPreset weatherPreset(WeatherKind kind);
const char*   weatherKindName(WeatherKind kind);

struct WeatherComponent
{
    // ── Authored (serialized) ────────────────────────────────────────────────
    WeatherKind currentKind = WeatherKind::Clear;  // settled state
    WeatherKind targetKind  = WeatherKind::Clear;  // desired state (set to start a change)
    float       intensity   = 1.0f;                // 0..1 scales the target preset

    float transitionDuration = 8.0f;   // seconds for a full weather change
    bool  autoCycle          = false;  // randomly cycle between weather kinds
    float cycleSeconds       = 60.0f;  // mean dwell time per state when autoCycle
    HE::UUID thunderSound;             // audio asset played on each lightning strike (optional)

    // Precipitation budget: hard cap on simultaneously alive drops/flakes. Lets the
    // user trade density for performance. Emission throttles to stay under the cap.
    int   maxRainParticles = 800;
    int   maxSnowParticles = 600;
    // World-space ground height used when no physics collision is available (editor
    // preview / game): drops/flakes die at this Y. In play mode the collision grid
    // (raycasts) overrides this per-cell.
    float groundLevel = 0.0f;

    // ── Runtime (never serialized) ───────────────────────────────────────────
    // Blend bookkeeping: `from*` is a snapshot of the live output taken when a new
    // target is requested, so retargeting mid-transition stays continuous.
    WeatherKind prevTarget       = WeatherKind::Clear;
    float       transitionElapsed = 0.0f;
    float       cycleTimer        = 0.0f;
    float       weatherTime       = 0.0f; // accumulates for wind-gust noise

    float      fromCloud      = 0.0f;
    float      fromFog        = 0.0f;
    float      fromWind       = 1.0f;
    float      fromRain       = 0.0f;
    float      fromSnow       = 0.0f;
    float      fromWetness    = 0.0f;

    // Last value the weather wrote into each EnvironmentComponent field. If the live
    // env value has drifted from this (the user moved a slider) the weather stops
    // driving that field — until the next preset selection reclaims it. Sentinel
    // (-999) so nothing is "owned" before the first apply (a loaded scene's authored
    // env is respected; picking a preset reclaims everything).
    float      lastCloud   = -999.0f;
    float      lastFog     = -999.0f;
    float      lastWind    = -999.0f;
    float      lastRain    = -999.0f;
    float      lastSnow    = -999.0f;
    float      lastWetness = -999.0f;

    // Current blended output written into the EnvironmentComponent + precipitation.
    float      curCloudCoverage = 0.0f;
    float      curFogDensity    = 0.0f;
    float      curWindSpeed     = 1.0f;
    float      curPrecip        = 0.0f;   // 0..1 → emitter rate (Phase 2)
    PrecipType curPrecipType    = PrecipType::None;
    float      curWetness       = 0.0f;

    // Precipitation particle pool (camera-following volume). Simulated by WeatherSystem
    // and drawn by RenderExtractor as billboards (rain = velocity-stretched streaks,
    // snow = small camera-facing quads). Runtime only — never serialized.
    std::vector<Particle> precip;
    float                 precipEmitAccum = 0.0f;
    std::mt19937          precipRng { 1337u };

    // Collision height grid (runtime): kGrid×kGrid ground heights sampled around the
    // camera every ~0.4 s via downward raycasts (play mode) or groundLevel (preview).
    // Drops die at the bilinearly-sampled height — O(grid) cost, independent of count.
    static constexpr int kGrid = 7;
    std::array<float, kGrid * kGrid> groundGrid {}; // row-major: index = z*kGrid + x
    float gridMinX = 0.0f, gridMinZ = 0.0f, gridStep = 1.0f;
    float gridTimer = 0.0f;
    bool  gridReady = false;

    // Storm runtime (Phase 3).
    float lightningCountdown = 0.0f;  // seconds until the next strike
    float flashIntensity     = 0.0f;  // current flash 0..1, decays each frame
    bool  flashTriggered     = false; // true on the frame a strike starts (thunder cue)
};
