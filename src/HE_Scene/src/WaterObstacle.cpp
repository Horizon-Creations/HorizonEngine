#include "HorizonScene/WaterObstacle.h"
#include "HorizonScene/Components/WaterObstacleComponent.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace HE::water
{

ObstacleFootprint obstacleFootprint(const glm::mat4& world, const float boundsMin[3], const float boundsMax[3])
{
    const glm::vec3 lo(boundsMin[0], boundsMin[1], boundsMin[2]);
    const glm::vec3 hi(boundsMax[0], boundsMax[1], boundsMax[2]);
    const glm::vec3 c = glm::vec3(world * glm::vec4((lo + hi) * 0.5f, 1.0f));
    const glm::vec3 h = glm::max((hi - lo) * 0.5f, glm::vec3(0.0f));
    const glm::vec3 axis[3] = { glm::vec3(world[0]) * h.x, glm::vec3(world[1]) * h.y, glm::vec3(world[2]) * h.z };

    ObstacleFootprint fp;
    fp.center = glm::vec2(c.x, c.z);
    // The vertical range of the box: the centre plus the reach of all three half axes in Y.
    const float reachY = std::abs(axis[0].y) + std::abs(axis[1].y) + std::abs(axis[2].y);
    fp.minY = c.y - reachY;
    fp.maxY = c.y + reachY;
    // Seen from above, the two half axes that lie flattest are the footprint (an upright box: X and Z); the third,
    // the most vertical, adds its horizontal lean to the longer of them so a tilted box is still covered.
    int up = 0;
    for (int i = 1; i < 3; ++i)
    {
        const float ri = std::abs(axis[i].y) / std::max(glm::length(axis[i]), 1e-6f);
        const float ru = std::abs(axis[up].y) / std::max(glm::length(axis[up]), 1e-6f);
        if (ri > ru) up = i;
    }
    const int ia = (up + 1) % 3, ib = (up + 2) % 3;
    fp.halfA = glm::vec2(axis[ia].x, axis[ia].z);
    fp.halfB = glm::vec2(axis[ib].x, axis[ib].z);
    const glm::vec2 lean(axis[up].x, axis[up].z);
    if (glm::length(lean) > 1e-4f)
    {
        glm::vec2& longer = glm::length(fp.halfA) >= glm::length(fp.halfB) ? fp.halfA : fp.halfB;
        const float l = glm::length(longer);
        if (l > 1e-6f) longer *= (l + std::abs(glm::dot(lean, longer / l))) / l;
    }
    // A flat footprint (a plane seen edge-on) still gets a hair of width.
    if (glm::length(fp.halfA) < 0.01f) fp.halfA = glm::vec2(0.01f, 0.0f);
    if (glm::length(fp.halfB) < 0.01f) fp.halfB = glm::vec2(0.0f, 0.01f);
    return fp;
}

bool obstacleFoam(const WaterIndex& index, const ObstacleFootprint& fp, const WaterObstacleComponent& c, double t,
                  FoamBand& out)
{
    out = FoamBand{};
    if (!c.enabled || !(c.foam > 0.0f)) return false;
    const float a = glm::length(fp.halfA), b = glm::length(fp.halfB);
    const glm::vec2 ua = fp.halfA / std::max(a, 1e-6f), ub = fp.halfB / std::max(b, 1e-6f);
    const float reach = std::max(a, b);

    // The water around it: its level, and its current, averaged over a ring just outside the footprint (in the
    // obstacle's own place the water may be measured under the rock, or not at all).
    float level = 0.0f, levels = 0.0f;
    glm::vec2 flow(0.0f);
    int wet = 0;
    for (int k = 0; k < 12; ++k)
    {
        const float th = 6.2831853f * static_cast<float>(k) / 12.0f;
        const glm::vec2 q = fp.center + glm::vec2(std::cos(th), std::sin(th)) * (reach + 0.5f);
        Surface s;
        if (!index.surfaceAt(q.x, q.y, t, s) || !s.wet) continue;
        level += s.level; levels += 1.0f;
        flow += glm::vec2(s.flow.x, s.flow.z);
        ++wet;
    }
    if (wet == 0) return false;
    level /= levels;
    flow /= static_cast<float>(wet);
    // Through the surface: the bottom under it and the top over it. A rock wholly under water, or a beam above it,
    // does not part the water.
    if (!(fp.minY < level) || !(fp.maxY > level)) return false;
    const float speed = glm::length(flow);
    if (!(speed >= kObstacleMinSpeed)) return false;
    const glm::vec2 dir = flow / speed;
    const float sf = std::clamp((speed - kObstacleMinSpeed) / (kObstacleFullSpeed - kObstacleMinSpeed), 0.0f, 1.0f);
    const float strength = std::clamp(c.foam * (0.25f + 0.75f * sf), 0.0f, 63.0f / kObstacleFoamLevels);
    const int lvl = std::clamp(static_cast<int>(std::lround(strength * kObstacleFoamLevels)), 1, 63);

    out.flowDir  = dir;
    out.speed    = speed;
    out.strength = static_cast<float>(lvl) / kObstacleFoamLevels;
    out.level    = level;

    // The band, row by row outward from the obstacle. What the water does where it meets a rock in a current:
    //  * upstream it piles against the face — a white pillow, the widest and densest part of the foam;
    //  * at the sides it is turned and runs past — shoulders that sweep downstream into a V;
    //  * right behind it is an eddy, calm; the wake trails off from the shoulders, longer the faster the water.
    // Per vertex the band carries where it faces (uv.y: 1 = straight into the current, 0 = straight downstream) and
    // which way the water runs THERE (the normal's lean): along the obstacle's face close to it, the free current
    // further out, so streaks in the foam bend round the rock.
    const int K = kObstacleFoamSegments;
    constexpr int kRows = 5;
    constexpr float kRowS[kRows] = { 0.0f, 0.12f, 0.3f, 0.55f, 1.0f };
    out.vertices.reserve(static_cast<size_t>(K + 1) * kRows * 8);
    for (int k = 0; k <= K; ++k)   // K + 1: the seam gets its own column (the strip closes on itself)
    {
        const float th = 6.2831853f * static_cast<float>(k % K) / static_cast<float>(K);
        const float cs = std::cos(th), sn = std::sin(th);
        // A rounded box (superellipse, exponent 4) around the footprint: the bounds' shape, without the corners a
        // rock never has.
        const float ex = std::copysign(std::sqrt(std::abs(cs)), cs), ez = std::copysign(std::sqrt(std::abs(sn)), sn);
        const glm::vec2 p = fp.center + ua * (a * ex) + ub * (b * ez);
        // Its outward normal: the gradient of |x/a|^4 + |z/b|^4.
        glm::vec2 nl = ua * (ex * ex * ex / std::max(a, 1e-4f)) + ub * (ez * ez * ez / std::max(b, 1e-4f));
        nl = glm::length(nl) > 1e-6f ? glm::normalize(nl) : glm::vec2(cs, sn);
        const float facing = -glm::dot(nl, dir);          // 1 = the face the water runs into
        const float up = std::max(facing, 0.0f), down = std::max(-facing, 0.0f), side = 1.0f - std::abs(facing);
        // The pillow upstream (wider for a faster current), the shoulders at the sides, the wake behind.
        const float w = c.margin * (0.55f + (1.1f + 0.7f * sf) * std::pow(up, 0.8f) + 0.25f * side)
                      + c.wake * sf * std::pow(down, 1.3f);
        // Outward, swept downstream: at the sides the foam is carried along, behind it trails straight off.
        const glm::vec2 sweep = nl * (1.0f - 0.5f * down) + dir * (0.8f * side + 1.6f * down);
        const glm::vec2 outward = glm::length(sweep) > 1e-5f ? glm::normalize(sweep) : nl;
        const glm::vec2 inner = p - nl * 0.08f;   // a little under the obstacle's edge: no gap at a rounder mesh
        const glm::vec2 outer = p + outward * std::max(w, 0.05f);
        // The water along the face: the current with its part into the obstacle taken out (zero where it runs
        // straight in, the stagnation point of the pillow).
        const glm::vec2 alongFace = dir - nl * glm::dot(dir, nl);
        for (int r = 0; r < kRows; ++r)
        {
            const float s = kRowS[r];
            const glm::vec2 q = inner + (outer - inner) * s;
            Surface sw;
            const bool here = index.surfaceAt(q.x, q.y, t, sw) && sw.wet;
            const float y = (here ? sw.level : level) + kObstacleFoamLift;
            // The current here, from along the face (close in) to free (outside), times its share of full speed.
            const float free = std::pow(s, 0.35f);   // turned only close in: past the rock it runs on straight
            const glm::vec2 flowHere = (alongFace * (1.0f - free) + dir * free) * sf;
            const glm::vec3 n = glm::normalize(glm::vec3(flowHere.x * kObstacleFoamFlowGain, 1.0f,
                                                         flowHere.y * kObstacleFoamFlowGain));
            // A vertex over dry ground is drawn fully faded: the band ends at the shore without a hard cut.
            const float u = static_cast<float>(lvl) + 0.001f + 0.998f * (here ? s : 1.0f);
            const float v = 0.5f + 0.5f * facing;
            out.vertices.insert(out.vertices.end(), { q.x, y, q.y, n.x, n.y, n.z, u, v });
            out.bounds.expand(glm::vec3(q.x, y, q.y));
        }
    }
    out.indices.reserve(static_cast<size_t>(K) * (kRows - 1) * 6);
    for (int k = 0; k < K; ++k)
        for (int r = 0; r + 1 < kRows; ++r)
        {
            const uint32_t i0 = static_cast<uint32_t>(k * kRows + r), i1 = i0 + 1;
            const uint32_t j0 = static_cast<uint32_t>((k + 1) * kRows + r), j1 = j0 + 1;
            out.indices.insert(out.indices.end(), { i0, j0, j1, i0, j1, i1 });
        }
    return true;
}

} // namespace HE::water
