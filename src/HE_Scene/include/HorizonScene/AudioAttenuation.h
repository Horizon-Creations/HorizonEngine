#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// ─── Distance attenuation of a spatial sound ─────────────────────────────────
// How a 3D sound gets quieter between its inner range (full volume) and its
// range (silence, or as quiet as the model gets). The three curves are the
// ones miniaudio's spatializer implements, and attenuationGain() below is a
// transcription of its formulas so the Details panel can DRAW the curve a
// source will be played with — the picture and the mixer must not disagree.
//
// Linear is the historic default and what every scene written before the
// field existed keeps playing with; the serializer treats an absent key as it.
enum class AudioAttenuation : uint8_t
{
    Linear      = 0,   // straight line from inner range to range; 0 at range
    Inverse     = 1,   // 1/d, the physical one: fast near, a long quiet tail
    Exponential = 2,   // (d/inner)^-rolloff; rolloff bends how sharp the knee is
    None        = 3,   // full volume everywhere, only the pan follows the position
};

// The gain miniaudio applies at `distance`, before the source's own volume.
// Mirrors ma_attenuation_{linear,inverse,exponential}: the distance is clamped
// to [minDist, maxDist], and a degenerate pair (min >= max) attenuates nothing
// rather than dividing by zero. The result is clamped to [0, 1] the way the
// spatializer's default min/max gain does — a linear curve with rolloff 2
// reaches zero halfway and stays there.
inline float attenuationGain(AudioAttenuation model, float distance,
                             float minDist, float maxDist, float rolloff)
{
    if (model == AudioAttenuation::None) return 1.0f;
    if (minDist >= maxDist)              return 1.0f;
    const float d = std::clamp(distance, minDist, maxDist);
    float g = 1.0f;
    switch (model)
    {
    case AudioAttenuation::Linear:
        g = 1.0f - rolloff * (d - minDist) / (maxDist - minDist);
        break;
    case AudioAttenuation::Inverse:
        g = minDist / (minDist + rolloff * (d - minDist));
        break;
    case AudioAttenuation::Exponential:
        g = minDist > 0.0f ? std::pow(d / minDist, -rolloff) : 1.0f;
        break;
    case AudioAttenuation::None:
        break;
    }
    return std::clamp(g, 0.0f, 1.0f);
}

// The spelling a scene file stores. Lower case and stable: a renamed enumerator
// must not change what an existing .hescene means.
inline const char* audioAttenuationName(AudioAttenuation m)
{
    switch (m)
    {
    case AudioAttenuation::Inverse:     return "inverse";
    case AudioAttenuation::Exponential: return "exponential";
    case AudioAttenuation::None:        return "none";
    case AudioAttenuation::Linear:      break;
    }
    return "linear";
}

// Unknown or empty reads as Linear — the value every scene had before the key.
inline AudioAttenuation audioAttenuationFromName(const char* name)
{
    if (!name) return AudioAttenuation::Linear;
    if (std::strcmp(name, "inverse")     == 0) return AudioAttenuation::Inverse;
    if (std::strcmp(name, "exponential") == 0) return AudioAttenuation::Exponential;
    if (std::strcmp(name, "none")        == 0) return AudioAttenuation::None;
    return AudioAttenuation::Linear;
}
