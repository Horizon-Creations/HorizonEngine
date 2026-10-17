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

    // The trains the physics follows: the two that carry the height (C is 2 cm).
    inline constexpr int       kPhysicsWaveCount = 2;
    inline constexpr WaveTrain kPhysicsWaves[kPhysicsWaveCount] = { kWaveA, kWaveB };

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
}
