#pragma once
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <functional>

// The room the listener stands in, as far as sound is concerned — the input of Steam
// Audio's parametric reflection effect (the project's "Reflections" option). Found the way
// the occlusion is (AudioOcclusion.h): rays through the physics world's colliders, so there
// is no second copy of the level. From the listener 26 rays go out (the six axes, twelve edge
// diagonals, eight corners); how many hit something and how far away tells the room's size
// and how closed it is.
//
// The decay time is Sabine's formula for a sphere-ish room, RT60 = 0.161 V / (S a) = 0.0537 r / a
// with r the mean distance to the walls and a the wall's absorption (a bare wall: 0.12 in the
// bass, 0.2 mid, 0.32 treble), scaled by how closed the room is: a clearing with one wall has
// almost no reverb, a corridor a lot.
namespace HE
{
    struct RoomEstimate
    {
        float reverbTimes[3] = { 0.0f, 0.0f, 0.0f };   // RT60 seconds, low / mid / high
        float wet            = 0.0f;                    // how much of the reverb to mix in, 0..1
        float closedFraction = 0.0f;                    // share of the rays that hit something
        float meanDistance   = 0.0f;                    // metres, over the rays that hit
    };

    inline constexpr float kRoomRayRange = 40.0f;       // metres: past this a ray counts as open sky
    inline constexpr float kRoomWallAbsorption[3] = { 0.12f, 0.20f, 0.32f };
    inline constexpr float kRoomMaxReverb = 4.0f;       // seconds
    inline constexpr float kRoomMaxWet    = 0.4f;

    // `hitDistance(from, direction, maxDistance)`: how far along the ray the first solid is, or a
    // negative number when nothing is within `maxDistance`.
    using RoomRay = std::function<float(const glm::vec3& from, const glm::vec3& direction, float maxDistance)>;

    inline RoomEstimate estimateRoom(const glm::vec3& listener, const RoomRay& hitDistance)
    {
        RoomEstimate out;
        if (!hitDistance) return out;

        glm::vec3 dirs[26];
        int n = 0;
        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                for (int z = -1; z <= 1; ++z)
                    if (x || y || z) dirs[n++] = glm::normalize(glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)));

        int hits = 0;
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const float d = hitDistance(listener, dirs[i], kRoomRayRange);
            if (d >= 0.0f && d < kRoomRayRange) { ++hits; sum += static_cast<double>(d); }
        }
        out.closedFraction = static_cast<float>(hits) / static_cast<float>(n);
        if (hits == 0) return out;

        out.meanDistance = static_cast<float>(sum / hits);
        // A leaky room (few walls) rings much less than its size says: the sound escapes.
        const float closed = out.closedFraction * out.closedFraction;
        for (int b = 0; b < 3; ++b)
            out.reverbTimes[b] = std::min(0.0537f * std::max(out.meanDistance, 0.5f) / kRoomWallAbsorption[b] * closed,
                                          kRoomMaxReverb);
        out.wet = kRoomMaxWet * closed;
        return out;
    }
}
