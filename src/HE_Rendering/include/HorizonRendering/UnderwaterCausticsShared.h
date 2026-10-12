#pragma once
#include <Physics/WaterWaves.h>
#include <cstdio>
#include <cstring>
#include <string>

// What every backend's caustic pass (Thema 185 Schritt 3) takes from the engine water
// instead of keeping a copy of its own: the three wave trains of Physics/WaterWaves.h
// (kWaveA/B/C — the numbers the surface material and the water physics already share),
// spliced into the shader source as a flat list "dirDeg, speed, length, steepness" × 3.
// A retuned material therefore moves the net with it; no second table to drift.
//
// Each backend wraps the list in its own array syntax (GLSL float[12](...), HLSL / MSL
// static const float[12] = {...}). Offline-compiled sources (the Vulkan .frag, which
// glslc builds before the program runs) cannot splice and hard-code the same twelve
// numbers with a pointer here — keep them in step by hand.
namespace HE
{
    inline std::string causticsWaveTrainsList()
    {
        const water::WaveTrain trains[3] = { water::kWaveA, water::kWaveB, water::kWaveC };
        std::string out;
        char buf[32];
        for (int i = 0; i < 3; ++i)
        {
            const float v[4] = { trains[i].directionDeg, trains[i].speed, trains[i].length, trains[i].steepness };
            for (int j = 0; j < 4; ++j)
            {
                // %.9g round-trips a float; "14" would read as an int literal in GLSL/HLSL, so
                // a number without a point or exponent gets ".0" (the values are all finite).
                std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v[j]));
                if (!std::strpbrk(buf, ".eE")) std::strncat(buf, ".0", sizeof(buf) - std::strlen(buf) - 1);
                if (i || j) out += ", ";
                out += buf;
            }
        }
        return out;
    }
}
