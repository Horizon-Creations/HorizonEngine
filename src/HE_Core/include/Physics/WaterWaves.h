#pragma once
#include <cmath>

// ─── The wave function of the water, in one place ────────────────────────────
// Engine/Materials/Water.hasset draws its waves from three Wave knobs (WaveA, WaveB,
// WaveC), each a Vec4: x = direction in degrees (0 = +X, 90 = +Z), y = speed in m/s,
// z = wavelength in metres, w = steepness (0 = flat; the slope of the surface at its
// steepest, so the amplitude is steepness · length / 2π). One train of the material is
//
//     h = (steepness / k) · sin( k · (d · p − speed · t) + warp + hexPhase ),   k = 2π / length
//
// (MatGen/main.cpp, waveTrain). The physics asks the same question — "how high is the
// surface here, and how does it tilt" — and must not answer it from a copy of the
// numbers that drifts when someone retunes the material. So the numbers and the formula
// live HERE, in HE_Core where the material generator (HE_Tools, mat_gen) and the
// physics (HE_Scene, WaterQuery) both reach them:
//
//   • mat_gen builds the knobs' defaults from kWaveA / kWaveB / kWaveC below, so the
//     engine water and the physics start from the same trains;
//   • WaterQuery evaluates sampleWaves() with the trains of a body of water.
//
// WHAT THE PHYSICS LEAVES OUT, ON PURPOSE. The shader bends the phase with an FBM
// field (`warp`) and gives every hex of a bombing grid its own direction and phase
// (`hexPhase`), and it adds a companion train per knob and a fine chop. None of that
// can be reproduced on the CPU without evaluating the whole graph, and none of it
// changes the SCALE of the surface: the amplitude steepness · length / 2π is the same.
// So the surface a body floats on has the heights and the directions of the water you
// see, not its phase; the largest difference is the physics amplitude (0.24 m with the
// defaults, exactly 0 at steepness 0). docs/water-physics-plan.md §2.
//
// TIME is the caller's: the shader uses the render clock, the physics the simulation
// time of the step (PhysicsWorld::simTime), so a paused or slowed game keeps its
// bodies and its waves in step with each other.
namespace HE::water
{
    inline constexpr double kWaveTwoPi = 6.283185307179586;

    struct WaveTrain
    {
        float directionDeg = 20.0f;   // 0 = +X, 90 = +Z (world XZ)
        float speed        = 1.0f;    // m/s along the direction
        float length       = 14.0f;   // wavelength in metres
        float steepness    = 0.07f;   // amplitude / wavelength · 2π; 0 = flat

        bool operator==(const WaveTrain& o) const
        {
            return directionDeg == o.directionDeg && speed == o.speed &&
                   length == o.length && steepness == o.steepness;
        }
        bool operator!=(const WaveTrain& o) const { return !(*this == o); }
    };

    // The defaults of the three Wave knobs of the engine water material.
    inline constexpr WaveTrain kWaveA{  20.0f, 1.0f, 14.0f, 0.07f };   // swell
    inline constexpr WaveTrain kWaveB{ 335.0f, 0.8f,  5.0f, 0.10f };
    inline constexpr WaveTrain kWaveC{  70.0f, 0.5f,  1.6f, 0.09f };   // fine ripples

    // SHORE PROXIMITY, mesh -> material (Thema 206 Schritt 2). The material pass reads
    // neither the scene depth nor a vertex colour, so the water mesh (HE_Scene,
    // WaterMesh.cpp) carries "how shallow is the water here" in the one per-vertex
    // channel the graph can see and the renderers all upload: the vertex normal. A
    // vertex gets the normal (kShoreNormalGain · e, 1, 0), e = 1 − depth / kShoreFadeDepth
    // clamped to 0..1, depth = the water over the ground in metres; the material
    // (mat_gen) reads e back as normal.x / normal.y / kShoreNormalGain. A mesh without
    // the channel (the engine plane, any user mesh) has x = 0, which reads as e = 0:
    // open water, nothing near a shore. A power of two, so the divide is exact.
    // Three metres since Thema 320: the material's colour and opacity follow the real depth over
    // the whole range, and with one metre the water went from clear to its deep colour inside a
    // single contour, which is the line the eye sees where the water meets the landscape.
    inline constexpr float kShoreFadeDepth  = 3.0f;          // metres of water over which e runs 1 -> 0
    inline constexpr float kShoreNormalGain = 1.0f / 32.0f;  // a 1.8 degree tilt at the waterline

    // The trains the physics follows: the two that carry the height (C is 2 cm).
    inline constexpr int       kPhysicsWaveCount = 2;
    inline constexpr WaveTrain kPhysicsWaves[kPhysicsWaveCount] = { kWaveA, kWaveB };

    // ─── Waves that follow the wind ─────────────────────────────────────────────────
    // The Environment's wind (EnvironmentComponent::windDirection, windSpeed; the weather
    // presets drive them) sets the sea the water shows and the physics floats on:
    //   • DIRECTION: the compass heading the wind blows toward (0 = -Z, clockwise) becomes
    //     the heading of the primary train (A); the other trains keep their angle to it,
    //     so the sea stays a mix of directions around the wind's.
    //   • STRENGTH: windSpeed is a multiplier, 1 = calm (the material's own numbers, so a
    //     scene at 1 keeps the sea it had). Fully developed seas grow roughly with the
    //     wind: the wavelength follows it, the speed follows sqrt(length) (deep-water
    //     dispersion, c = sqrt(g L / 2 pi)), the steepness a little (a stronger wind
    //     makes a longer AND steeper sea), so the height steepness * L / 2 pi grows with
    //     the wind's square: calm 1x, the storm preset (2.6) about 3.4x.
    // One definition: the renderer (material knobs at extract time), the physics
    // (WaterQuery) and the caustics all call it, so the three never disagree.
    inline constexpr float kWindCalm = 1.0f;

    struct WindWaveFactors
    {
        float length;      // x the base wavelength
        float speed;       // x the base speed
        float steepness;   // x the base steepness
    };

    inline WindWaveFactors windWaveFactors(float windSpeed)
    {
        const float u = windSpeed < 0.25f ? 0.25f : (windSpeed > 4.0f ? 4.0f : windSpeed);
        const float l = u;                         // wavelength grows with the wind
        const float st = 1.0f + 0.3f * (u - 1.0f); // and so does the steepness, gently
        return { l, std::sqrt(l), st < 0.4f ? 0.4f : st };
    }

    // Compass heading (0 = toward -Z, clockwise: 90 = +X) -> a wave direction in this
    // header's frame (0 = +X, 90 = +Z).
    inline float waveDirectionFromWind(float windCompassDeg) { return windCompassDeg - 90.0f; }

    // How far the wind turns the sea from the material's own: the shift of the primary
    // train's heading, degrees. Everything that rotates "the sea" uses this one number.
    inline float windTurnDeg(float windCompassDeg)
    {
        return waveDirectionFromWind(windCompassDeg) - kWaveA.directionDeg;
    }

    // One train under a wind. `base` is the material's train, `windCompassDeg` and
    // `windSpeed` the environment's. Calm wind from the material's own heading is the
    // identity.
    inline WaveTrain windTrain(const WaveTrain& base, float windCompassDeg, float windSpeed)
    {
        const WindWaveFactors f = windWaveFactors(windSpeed);
        WaveTrain t = base;
        t.directionDeg = base.directionDeg + windTurnDeg(windCompassDeg);
        t.length       = base.length * f.length;
        t.speed        = base.speed * f.speed;
        t.steepness    = base.steepness * f.steepness;
        return t;
    }

    // Height of one train: steepness · length / 2π, the factor in front of the sine.
    inline float waveAmplitude(const WaveTrain& w)
    {
        return static_cast<float>(static_cast<double>(w.steepness) * w.length / kWaveTwoPi);
    }

    struct WaveSample
    {
        float height = 0.0f;   // metres above the level, summed over the trains
        float dhdx   = 0.0f;   // slope along world X
        float dhdz   = 0.0f;   // slope along world Z
    };

    // The trains summed at world (x, z) and time t. The phase is computed in double:
    // a session that has run for an hour and a landscape a few km from the origin must
    // not lose the fraction of a wavelength a float cannot hold.
    inline WaveSample sampleWaves(const WaveTrain* trains, int count, double x, double z, double t)
    {
        WaveSample out;
        for (int i = 0; i < count; ++i)
        {
            const WaveTrain& w = trains[i];
            if (!(w.length > 0.0f) || w.steepness == 0.0f)
                continue;
            const double rad  = static_cast<double>(w.directionDeg) * (kWaveTwoPi / 360.0);
            const double dx   = std::cos(rad), dz = std::sin(rad);
            const double k    = kWaveTwoPi / static_cast<double>(w.length);
            const double ph   = k * (dx * x + dz * z - static_cast<double>(w.speed) * t);
            const double amp  = static_cast<double>(w.steepness) / k;
            out.height += static_cast<float>(amp * std::sin(ph));
            // d/dp of amp · sin(ph) = amp · k · cos(ph) · d = steepness · cos(ph) · d
            const double slope = static_cast<double>(w.steepness) * std::cos(ph);
            out.dhdx += static_cast<float>(slope * dx);
            out.dhdz += static_cast<float>(slope * dz);
        }
        return out;
    }

    // ─── The tessellated surface ────────────────────────────────────────────────────
    // With a landscape's water tessellation on, the surface is REAL geometry: the three trains
    // of the material's knobs (under the wind, windTrain) summed as plain sines, no hex
    // bombing, warp or gusts, so the CPU can name the height of the visible surface exactly
    // (the underwater post effect follows it):
    //     h(x, z, t) = height * shore(depth) * sum_i amp_i sin(k_i (d_i . p - speed_i t))
    // Two per-body numbers (WaterField.h TessellationParams, the material's "TessWaves" knob):
    //   height      multiplier of the trains' own amplitude: a river is a fraction of it, a
    //               sea close to (or above) 1.
    //   shoreWidth  metres of water over which the waves grow in from the coast. The mesh bakes
    //               the shore proximity into its normal (kShoreFadeDepth = 3 m of range), so the
    //               surface is flat AT the waterline however high the height is set.
    inline constexpr float kDefaultTessHeight = 0.6f;
    inline constexpr float kMaxTessHeight     = 2.0f;
    inline constexpr float kDefaultTessShore  = 1.8f;
    inline constexpr float kMinTessShore      = 0.1f;
    inline constexpr float kMaxTessShore      = 3.0f;   // the shore range the mesh knows

    // smoothstep(0, 1, depth / shoreWidth): 0 at the waterline, 1 from shoreWidth metres of water.
    inline float tessShoreFactor(float waterDepth, float shoreWidth)
    {
        const float w = shoreWidth < kMinTessShore ? kMinTessShore : (shoreWidth > kMaxTessShore ? kMaxTessShore : shoreWidth);
        float u = waterDepth / w;
        u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
        return u * u * (3.0f - 2.0f * u);
    }

    // The height of the tessellated surface above its level at (x, z), time t, `waterDepth`
    // metres of water there. `trains` are the three knob trains after windTrain.
    inline float tessellatedHeight(const WaveTrain* trains, double x, double z, double t, float waterDepth,
                                   float height, float shoreWidth)
    {
        const WaveSample s = sampleWaves(trains, 3, x, z, t);
        return height * tessShoreFactor(waterDepth, shoreWidth) * s.height;
    }
}
