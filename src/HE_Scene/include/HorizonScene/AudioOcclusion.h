#pragma once
#include <glm/glm.hpp>
#include <functional>

// How much of a 3D voice reaches the listener through the world's geometry — the input of
// Steam Audio's direct effect (the project's "Occlusion" option). The geometry is the
// physics world's own colliders, asked with rays, so there is no second copy of the level
// to keep in step: whatever blocks the player blocks the sound.
//
// A single ray from the listener to the source flips between "heard" and "silenced" as the
// listener walks past a pillar. Steam Audio's own answer for a source with a radius is a
// FRACTION, so the source is treated as a small sphere here too: five rays, one to its
// centre and four to points round it across the line of sight, and the share that are
// blocked is the occlusion.
namespace HE
{
    struct OcclusionResult
    {
        float visibility = 1.0f;   // Steam Audio's "occlusion" value: 1 = clear path, 0 = fully blocked
        // What a blocker lets through, per band (low / mid / high): a wall passes
        // bass and stops treble.
        float transmission[3] = { 1.0f, 1.0f, 1.0f };
    };

    inline constexpr float kOcclusionSourceRadius = 0.4f;   // metres: how far the four side rays sit from the centre
    inline constexpr float kOcclusionTransmission[3] = { 0.30f, 0.12f, 0.04f };

    // `blocked(from, toward, maxDistance)`: true when something solid is hit along the ray before
    // `maxDistance`. The estimator stops the ray a little short of the source so the source's own
    // collider does not count.
    using OcclusionRay = std::function<bool(const glm::vec3& from, const glm::vec3& direction, float maxDistance)>;

    inline OcclusionResult estimateOcclusion(const glm::vec3& listener, const glm::vec3& source,
                                              const OcclusionRay& blocked)
    {
        OcclusionResult out;
        const glm::vec3 d = source - listener;
        const float len = glm::length(d);
        if (!(len > 0.5f) || !blocked) return out;   // on top of the listener: nothing between

        const glm::vec3 fwd = d / len;
        glm::vec3 side = glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f));
        side = glm::length(side) > 1.0e-4f ? glm::normalize(side) : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 up = glm::normalize(glm::cross(side, fwd));

        const glm::vec3 targets[5] = {
            source,
            source + side * kOcclusionSourceRadius, source - side * kOcclusionSourceRadius,
            source + up   * kOcclusionSourceRadius, source - up   * kOcclusionSourceRadius,
        };
        int hit = 0;
        for (const glm::vec3& t : targets)
        {
            const glm::vec3 v = t - listener;
            const float vl = glm::length(v);
            const float reach = vl - 0.5f;   // short of the source's own collider
            if (reach > 0.05f && blocked(listener, v / vl, reach)) ++hit;
        }
        out.visibility = 1.0f - static_cast<float>(hit) / 5.0f;
        for (int b = 0; b < 3; ++b) out.transmission[b] = kOcclusionTransmission[b];
        return out;
    }
}
