#pragma once
#include <HorizonScene/AudioOcclusion.h>   // OcclusionRay
#include <glm/glm.hpp>
#include <cmath>

// Sound finding its way round a corner or through a door — the project's "Pathing" option.
//
// Steam Audio's own pathing walks a graph of probes baked into the level in the editor; a
// scene here has no such data, so this estimates the same thing live. When the straight line
// from the listener to a voice is blocked, a handful of waypoints beside it are tried (three
// places along the line, eight lateral offsets each, in the horizontal plane): the first leg
// from the listener and the second to the source must both be clear. The shortest such detour
// wins, and the voice is then heard as coming from the waypoint's direction at the length of
// the whole detour — which is what a person hears when a door is open down the corridor: the
// sound arrives from the doorway, a little quieter than it would be in the open.
//
// The rays are the physics colliders', like the occlusion's.
namespace HE
{
    struct PathResult
    {
        bool      routed   = false;   // a clear detour exists and the straight line is blocked
        glm::vec3 position{ 0.0f };   // where to HEAR the voice: toward the waypoint, at the detour's length
        float     length   = 0.0f;    // metres of the whole detour (listener -> waypoint -> source)
    };

    // What the occlusion keeps of a voice that arrives by a detour instead of through the wall:
    // not blocked, but a corner takes some treble.
    inline constexpr float kPathVisibility = 0.75f;

    inline PathResult estimatePath(const glm::vec3& listener, const glm::vec3& source, const OcclusionRay& blocked)
    {
        PathResult out;
        const glm::vec3 d = source - listener;
        const float len = glm::length(d);
        if (!(len > 1.0f) || !blocked) return out;

        auto clear = [&](const glm::vec3& a, const glm::vec3& b) {
            const glm::vec3 v = b - a;
            const float vl = glm::length(v);
            return vl <= 0.05f || !blocked(a, v / vl, vl - 0.05f);
        };
        if (clear(listener, source)) return out;   // straight line is open: no detour needed

        const glm::vec3 fwd = d / len;
        glm::vec3 side = glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f));
        if (glm::length(side) < 1.0e-4f) return out;   // looking straight up or down the line
        side = glm::normalize(side);

        static constexpr float kAlong[3]   = { 0.3f, 0.5f, 0.7f };
        static constexpr float kLateral[8] = { 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f, 8.0f, -8.0f };
        float best = 1.0e30f;
        for (float f : kAlong)
            for (float l : kLateral)
            {
                const glm::vec3 w = listener + d * f + side * l;
                const float total = glm::length(w - listener) + glm::length(source - w);
                if (total >= best) continue;
                if (!clear(listener, w) || !clear(w, source)) continue;
                best = total;
                out.routed   = true;
                out.length   = total;
                out.position = listener + glm::normalize(w - listener) * total;
            }
        return out;
    }
}
