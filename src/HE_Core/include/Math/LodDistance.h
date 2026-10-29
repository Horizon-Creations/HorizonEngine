#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

// Where a detail level may start, derived from the error its simplification
// introduced instead of a guessed distance (Thema 331, step 3).
//
// A level whose geometry is off by `worldError` metres from the level above it
// is off by worldError * referenceHeightPx / (2 * d * tan(fovY / 2)) pixels when
// seen from distance d (pinhole camera, the error seen side-on, which is the
// worst case). The level may take over where that stays within tolerancePx.
// The reference screen is a constant, not the live window, so a project's
// distances do not change with the size the editor happens to be docked at.
namespace HE
{

struct LodScreenMetrics
{
    float referenceHeightPx = 1080.0f;   // the screen the tolerance is measured on
    float fovYDegrees       = 60.0f;
    float tolerancePx       = 2.0f;      // the pixel error a level may show
};

// Distance at which `worldError` metres project to metrics.tolerancePx pixels.
inline float lodDistanceForError(float worldError, const LodScreenMetrics& m)
{
    const float halfFov = std::clamp(m.fovYDegrees, 5.0f, 170.0f) * 0.5f * 3.14159265358979f / 180.0f;
    const float denom   = 2.0f * std::tan(halfFov) * std::max(m.tolerancePx, 0.01f);
    return std::max(worldError, 0.0f) * m.referenceHeightPx / denom;
}

// Turns each level's start distance into a strictly increasing run: a level
// that is no worse than the one before it (error 0, or a worse level that
// projected nearer) would otherwise start at or before its predecessor, which
// the consumers read as an empty or inverted band. `minGap` is the smallest
// band a level gets, in metres.
inline std::vector<float> lodDistancesFromErrors(const std::vector<float>& worldErrors,
                                                 const LodScreenMetrics& m,
                                                 float minGap = 1.0f)
{
    std::vector<float> out;
    out.reserve(worldErrors.size());
    float prev = 0.0f;
    for (float e : worldErrors)
    {
        float d = lodDistanceForError(e, m);
        d = std::max(d, prev + minGap);
        out.push_back(d);
        prev = d;
    }
    return out;
}

} // namespace HE
