#include "HorizonScene/WeatherSystem.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/PhysicsWorld.h"
#include "HorizonScene/Components/WeatherComponent.h"
#include "HorizonScene/Components/EnvironmentComponent.h"

#include <algorithm>
#include <random>

// ── Preset table ────────────────────────────────────────────────────────────
// One target sky state per weather kind. WeatherSystem lerps between two of these.
WeatherPreset weatherPreset(WeatherKind kind)
{
    switch (kind)
    {
        //                     cloud  fog    wind  precip  type               wet   lightning
        case WeatherKind::Clear:    return { 0.05f, 0.00f, 0.8f, 0.0f, PrecipType::None, 0.0f, false };
        case WeatherKind::Cloudy:   return { 0.45f, 0.00f, 1.2f, 0.0f, PrecipType::None, 0.0f, false };
        case WeatherKind::Overcast: return { 0.85f, 0.01f, 1.4f, 0.0f, PrecipType::None, 0.1f, false };
        case WeatherKind::Foggy:    return { 0.60f, 0.08f, 0.4f, 0.0f, PrecipType::None, 0.2f, false };
        case WeatherKind::Rain:     return { 0.90f, 0.03f, 1.6f, 0.7f, PrecipType::Rain, 0.8f, false };
        case WeatherKind::Storm:    return { 1.00f, 0.04f, 2.6f, 1.0f, PrecipType::Rain, 1.0f, true  };
        case WeatherKind::Snow:     return { 0.80f, 0.05f, 0.9f, 0.6f, PrecipType::Snow, 0.3f, false };
        default:                    return {};
    }
}

const char* weatherKindName(WeatherKind kind)
{
    switch (kind)
    {
        case WeatherKind::Clear:    return "Clear";
        case WeatherKind::Cloudy:   return "Cloudy";
        case WeatherKind::Overcast: return "Overcast";
        case WeatherKind::Foggy:    return "Foggy";
        case WeatherKind::Rain:     return "Rain";
        case WeatherKind::Storm:    return "Storm";
        case WeatherKind::Snow:     return "Snow";
        default:                    return "Unknown";
    }
}

namespace
{
    inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

    WeatherKind randomKindExcept(WeatherKind except)
    {
        static std::mt19937 rng{ std::random_device{}() };
        std::uniform_int_distribution<int> d(0, static_cast<int>(WeatherKind::Count) - 1);
        WeatherKind k;
        do { k = static_cast<WeatherKind>(d(rng)); } while (k == except);
        return k;
    }

    // Bilinearly sample the collision height grid at world (x,z). Outside the grid (or
    // before the first refresh) falls back to the flat groundLevel.
    float sampleGround(const WeatherComponent& wx, float x, float z)
    {
        constexpr int N = WeatherComponent::kGrid;
        if (!wx.gridReady || wx.gridStep <= 0.0f) return wx.groundLevel;
        const float fx = (x - wx.gridMinX) / wx.gridStep;
        const float fz = (z - wx.gridMinZ) / wx.gridStep;
        if (fx < 0.0f || fz < 0.0f || fx > N - 1 || fz > N - 1) return wx.groundLevel;
        const int x0 = std::min(static_cast<int>(fx), N - 2);
        const int z0 = std::min(static_cast<int>(fz), N - 2);
        const float tx = fx - static_cast<float>(x0);
        const float tz = fz - static_cast<float>(z0);
        auto G = [&](int gx, int gz) { return wx.groundGrid[gz * N + gx]; };
        const float a = G(x0, z0)     + (G(x0 + 1, z0)     - G(x0, z0))     * tx;
        const float b = G(x0, z0 + 1) + (G(x0 + 1, z0 + 1) - G(x0, z0 + 1)) * tx;
        return a + (b - a) * tz;
    }
}

void WeatherSystem::update(HorizonWorld& world, float dt, const glm::vec3& cameraPos,
                           const PhysicsWorld* physics, bool gpuPrecip)
{
    auto& reg = world.registry();

    // First WeatherComponent wins (weather is scene-wide).
    WeatherComponent* w = nullptr;
    for (auto [e, wc] : reg.view<WeatherComponent>().each()) { w = &wc; break; }
    if (!w) return;
    WeatherComponent& wx = *w;

    // Auto-cycle: once settled, pick a fresh target after the dwell time elapses.
    if (wx.autoCycle)
    {
        wx.cycleTimer += dt;
        if (wx.cycleTimer >= std::max(wx.cycleSeconds, 0.0f) && wx.targetKind == wx.currentKind)
        {
            wx.targetKind = randomKindExcept(wx.currentKind);
            wx.cycleTimer = 0.0f;
        }
    }

    // Resolve the (intensity-scaled) target preset values.
    const float intensity = std::clamp(wx.intensity, 0.0f, 1.0f);
    const WeatherPreset tp = weatherPreset(wx.targetKind);
    const float tCloud = std::clamp(tp.cloudCoverage * intensity, 0.0f, 1.0f);
    const float tFog   = std::max(tp.fogDensity * intensity, 0.0f);
    const float tWind  = 1.0f + (tp.windSpeed - 1.0f) * intensity; // intensity 0 → calm
    const float tPrec  = std::clamp(tp.precip * intensity, 0.0f, 1.0f);
    const float tWet   = std::clamp(tp.wetness * intensity, 0.0f, 1.0f);

    if (wx.targetKind == wx.currentKind)
    {
        // Settled (also the load / steady-state path): snap outputs to the target.
        wx.curCloudCoverage = tCloud;
        wx.curFogDensity    = tFog;
        wx.curWindSpeed     = tWind;
        wx.curPrecip        = tPrec;
        wx.curPrecipType    = tp.precipType;
        wx.curWetness       = tWet;
        wx.transitionElapsed = 0.0f;
        wx.prevTarget        = wx.targetKind;
    }
    else
    {
        // Transitioning currentKind → targetKind. Snapshot the live output as the
        // blend origin the first frame a (new) target is seen — robust to retargeting
        // mid-transition.
        if (wx.targetKind != wx.prevTarget)
        {
            wx.fromCloud      = wx.curCloudCoverage;
            wx.fromFog        = wx.curFogDensity;
            wx.fromWind       = wx.curWindSpeed;
            wx.fromPrecip     = wx.curPrecip;
            wx.fromWetness    = wx.curWetness;
            wx.fromPrecipType = wx.curPrecipType;
            wx.transitionElapsed = 0.0f;
            wx.prevTarget        = wx.targetKind;
        }

        const float dur = std::max(wx.transitionDuration, 1e-4f);
        wx.transitionElapsed = std::min(wx.transitionElapsed + std::max(dt, 0.0f), dur);
        const float a = std::clamp(wx.transitionElapsed / dur, 0.0f, 1.0f);
        const float s = a * a * (3.0f - 2.0f * a); // smoothstep

        wx.curCloudCoverage = lerpf(wx.fromCloud,   tCloud, s);
        wx.curFogDensity    = lerpf(wx.fromFog,     tFog,   s);
        wx.curWindSpeed     = lerpf(wx.fromWind,    tWind,  s);
        wx.curPrecip        = lerpf(wx.fromPrecip,  tPrec,  s);
        wx.curWetness       = lerpf(wx.fromWetness, tWet,   s);
        wx.curPrecipType    = (s >= 0.5f) ? tp.precipType : wx.fromPrecipType;

        if (a >= 1.0f) wx.currentKind = wx.targetKind;
    }

    // ── Lightning (storm only) ──────────────────────────────────────────────
    // Random strikes set flashIntensity to 1; it decays fast (a brief flash). The
    // strike frame raises flashTriggered so a consumer can fire thunder audio.
    wx.weatherTime += dt;
    wx.flashTriggered = false;
    const bool storming = weatherPreset(wx.targetKind).lightning && intensity > 0.05f;
    if (storming)
    {
        wx.lightningCountdown -= dt;
        if (wx.lightningCountdown <= 0.0f)
        {
            wx.flashIntensity = 1.0f;
            wx.flashTriggered = true;
            std::uniform_real_distribution<float> iv(2.5f, 11.0f);
            wx.lightningCountdown = iv(wx.precipRng) * (1.2f - intensity); // stormier = more frequent
        }
    }
    wx.flashIntensity = std::max(0.0f, wx.flashIntensity - dt * 6.0f); // fast decay

    // Drive the scene environment. windSpeed is the SMOOTH curWindSpeed (no per-frame
    // gust) — gusts modulated the cloud-drift uniform and made the clouds wobble.
    float windDirDeg = 30.0f;
    if (auto* env = reg.try_get<EnvironmentComponent>(world.rootEntity()))
    {
        // In manual-environment mode the user owns cloud/fog/wind from the inspector —
        // leave them untouched. Flash stays weather-driven (it's a transient effect, not
        // an authored slider). windSpeed for the precip drift then follows env->windSpeed.
        if (!wx.manualEnvironment)
        {
            env->cloudCoverage = wx.curCloudCoverage;
            env->fogDensity    = wx.curFogDensity;
            env->windSpeed     = wx.curWindSpeed;
        }
        else
        {
            wx.curWindSpeed = env->windSpeed;   // keep precip drift in sync with the manual value
        }
        env->flash         = wx.flashIntensity;
        windDirDeg         = env->windDirection;
    }
    // GPU precipitation: the renderer owns the rain/snow pool (transform feedback),
    // so skip the CPU volume entirely. Free the CPU pool once so RenderExtractor stops
    // emitting billboards for it. cloud/fog/wind + curPrecip were already updated above.
    if (gpuPrecip)
    {
        if (!wx.precip.empty()) { wx.precip.clear(); wx.precip.shrink_to_fit(); }
        return;
    }

    // Steady horizontal wind vector (direction 0 = toward -Z, matching the clouds).
    const float wr = glm::radians(windDirDeg);
    const glm::vec3 windVec = glm::vec3(std::sin(wr), 0.0f, -std::cos(wr)) * wx.curWindSpeed;

    // ── Precipitation: camera-following volume with collision ────────────────
    // Drops spawn well above the viewer and fall (wind-slanted), dying at the ground
    // height sampled from the collision grid. Snow is slow with a gentle sway.
    const bool  isSnow      = (wx.curPrecipType == PrecipType::Snow);
    const bool  wantsPrecip = (wx.curPrecipType != PrecipType::None) && (wx.curPrecip > 0.001f);
    constexpr float kBoxHalf = 16.0f;  // horizontal half-extent around the camera
    constexpr float kBoxTop  = 24.0f;  // spawn height above the camera
    const int   maxParticles = isSnow ? std::max(0, wx.maxSnowParticles)
                                      : std::max(0, wx.maxRainParticles);
    const float fallSpeed = isSnow ? 2.0f : 18.0f;
    const float lifeSpan  = (kBoxTop + 60.0f) / std::max(fallSpeed, 0.01f); // generous fall cap

    // Refresh the collision height grid (~0.4 s) centred on the camera. With a physics
    // world each cell is a downward raycast (real collision against bodies/colliders);
    // otherwise every cell is the flat groundLevel. Cost is O(grid), not O(particles).
    wx.gridTimer -= dt;
    if (!wx.gridReady || wx.gridTimer <= 0.0f)
    {
        wx.gridTimer = 0.4f;
        wx.gridStep  = (2.0f * kBoxHalf) / static_cast<float>(WeatherComponent::kGrid - 1);
        wx.gridMinX  = cameraPos.x - kBoxHalf;
        wx.gridMinZ  = cameraPos.z - kBoxHalf;
        const float rayTop = cameraPos.y + kBoxTop;
        const float rayLen = kBoxTop + 150.0f;
        for (int gz = 0; gz < WeatherComponent::kGrid; ++gz)
            for (int gx = 0; gx < WeatherComponent::kGrid; ++gx)
            {
                float h = wx.groundLevel;
                if (physics)
                {
                    const glm::vec3 o(wx.gridMinX + gx * wx.gridStep, rayTop,
                                      wx.gridMinZ + gz * wx.gridStep);
                    const auto hr = physics->raycast(o, glm::vec3(0.0f, -1.0f, 0.0f), rayLen);
                    if (hr.hit) h = hr.point.y;
                }
                wx.groundGrid[gz * WeatherComponent::kGrid + gx] = h;
            }
        wx.gridReady = true;
    }

    // Integrate, then recycle drops that reached the ground or outlived the fall cap.
    for (Particle& p : wx.precip)
    {
        p.lifetime -= dt;
        p.position += p.velocity * dt;
        if (isSnow) // gentle horizontal sway
            p.position.x += std::sin((p.maxLifetime - p.lifetime) * 2.2f) * 0.5f * dt;
    }
    wx.precip.erase(std::remove_if(wx.precip.begin(), wx.precip.end(),
        [&](const Particle& p)
        { return p.lifetime <= 0.0f || p.position.y <= sampleGround(wx, p.position.x, p.position.z); }),
        wx.precip.end());

    if (wantsPrecip)
    {
        // Grow the pool to its cap once so the emission loop never reallocates mid-fill.
        if (static_cast<int>(wx.precip.capacity()) < maxParticles)
            wx.precip.reserve(static_cast<size_t>(maxParticles));
        const float emitRate = wx.curPrecip * (isSnow ? 400.0f : 900.0f); // particles/sec
        const float interval = (emitRate > 0.0f) ? 1.0f / emitRate : 1e30f;
        wx.precipEmitAccum += dt;
        std::uniform_real_distribution<float> u11(-1.0f, 1.0f);
        while (wx.precipEmitAccum >= interval && static_cast<int>(wx.precip.size()) < maxParticles)
        {
            wx.precipEmitAccum -= interval;
            Particle p;
            p.position = glm::vec3(cameraPos.x + u11(wx.precipRng) * kBoxHalf,
                                   cameraPos.y + kBoxTop,
                                   cameraPos.z + u11(wx.precipRng) * kBoxHalf);
            glm::vec3 vel(0.0f, -fallSpeed, 0.0f);
            if (isSnow)
            {
                vel.x += u11(wx.precipRng) * 0.6f + windVec.x * 0.3f;
                vel.z += u11(wx.precipRng) * 0.6f + windVec.z * 0.3f;
            }
            else // rain leans with the wind
            {
                vel.x += windVec.x * 1.2f;
                vel.z += windVec.z * 1.2f;
            }
            p.velocity    = vel;
            p.lifetime    = lifeSpan;
            p.maxLifetime = lifeSpan;
            wx.precip.push_back(p);
        }
        if (wx.precipEmitAccum > interval) wx.precipEmitAccum = interval; // no burst after a stall
    }
}
