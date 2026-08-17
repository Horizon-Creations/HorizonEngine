#pragma once
#include "SkyEnvBake.h"   // HE::SkyColorCPU — the sky the scene renders, on the CPU
#include <glm/vec3.hpp>
#include <cmath>
#include <vector>

// ─── World-preview backdrop geometry ────────────────────────────────────────
// The ground plane, the grid and the origin marker that IRenderer::
// RenderWorldPreview draws behind the world (see the Class Editor's viewport).
//
// Free functions in a header, deliberately: every backend has to build the same
// backdrop, and a grid that is one shade darker or half a unit wider on OpenGL
// than on Metal is exactly the kind of difference nobody notices until an author
// switches machines and thinks the scale changed. Vertices are plain
// pos3 + color3 floats, which is the layout every backend's small line/triangle
// preview program already uses.
//
// Coordinates are WORLD space with Y up. Everything is laid out RELATIVE TO
// `origin` — for a class preview that is the character's own origin, the point
// every component transform is measured from, and it is normally the world
// origin. Centring the backdrop on it is what makes "objects sit relative to
// the origin" true in the picture and not just in the data. The ground sits a
// hair below the grid plane so the two cannot z-fight.

namespace HE
{

constexpr float kPreviewGroundDrop = 0.002f;

// Two triangles, 6 vertices (36 floats), spanning ±halfExtent in X/Z around
// `origin`.
inline void buildPreviewGround(float halfExtent, std::vector<float>& out,
                               const glm::vec3& origin = glm::vec3(0.0f))
{
    const float y = origin.y - kPreviewGroundDrop;
    const glm::vec3 c(0.155f, 0.155f, 0.170f);
    const float h = halfExtent;
    const float x0 = origin.x - h, x1 = origin.x + h;
    const float z0 = origin.z - h, z1 = origin.z + h;
    const float quad[6][3] = {
        { x0, y, z0 }, { x0, y, z1 }, { x1, y, z1 },
        { x0, y, z0 }, { x1, y, z1 }, { x1, y, z0 },
    };
    for (const auto& v : quad)
        out.insert(out.end(), { v[0], v[1], v[2], c.r, c.g, c.b });
}

// Grid lines every `step` units out to ±halfExtent, with every tenth line
// brighter, the two axes THROUGH the origin tinted, and a small marker on the
// origin itself. Because the lines are laid out from `origin` rather than from
// world zero, the tinted pair always passes exactly through it.
inline void buildPreviewGrid(float halfExtent, float step, std::vector<float>& out,
                             const glm::vec3& origin = glm::vec3(0.0f))
{
    const glm::vec3 minor(0.255f, 0.255f, 0.275f);
    const glm::vec3 major(0.360f, 0.360f, 0.385f);
    const glm::vec3 axisX(0.55f, 0.22f, 0.22f);
    const glm::vec3 axisZ(0.22f, 0.28f, 0.62f);
    const glm::vec3 axisY(0.24f, 0.55f, 0.28f);

    auto push = [&out](const glm::vec3& p, const glm::vec3& c) {
        out.insert(out.end(), { p.x, p.y, p.z, c.r, c.g, c.b });
    };

    if (step <= 0.0f) step = 1.0f;
    const int lines = static_cast<int>(halfExtent / step);
    for (int i = -lines; i <= lines; ++i)
    {
        const float t = static_cast<float>(i) * step;
        const bool  isAxis  = (i == 0);
        const bool  isMajor = (i % 10 == 0);
        const glm::vec3 cz = isAxis ? axisZ : (isMajor ? major : minor); // line along Z
        const glm::vec3 cx = isAxis ? axisX : (isMajor ? major : minor); // line along X
        const float x = origin.x + t, z = origin.z + t;
        const float y = origin.y;
        push({ x, y, origin.z - halfExtent }, cz); push({ x, y, origin.z + halfExtent }, cz);
        push({ origin.x - halfExtent, y, z }, cx); push({ origin.x + halfExtent, y, z }, cx);
    }

    // Origin marker: the up axis (the ground grid already carries X and Z) plus
    // a short raised cross, so the origin stays readable when the camera looks
    // almost straight down the Y axis and the vertical line degenerates.
    const float a = step * 0.5f;
    const float y = origin.y, yr = origin.y + 0.001f;
    push({ origin.x, y, origin.z }, axisY); push({ origin.x, y + step, origin.z }, axisY);
    push({ origin.x - a, yr, origin.z }, axisY); push({ origin.x + a, yr, origin.z }, axisY);
    push({ origin.x, yr, origin.z - a }, axisY); push({ origin.x, yr, origin.z + a }, axisY);
}

// ─── Sky dome ───────────────────────────────────────────────────────────────
// A sphere around the camera whose vertices carry the sky's own colour, so a
// preview can have a real sky without a sky pass.
//
// The colours come from `SkyColorCPU` — the SAME function that bakes the
// scene's image-based ambient cubemap — so the preview and the scene can never
// disagree about what a given sun position looks like. Interpolating a coarse
// grid is enough because the sky IS a gradient: the one feature a per-pixel
// pass would add (the sun disc) sits behind the geometry a preview is about.
//
// Drawn as plain triangles through the same pos3+color3 program as the ground,
// which is what makes it cost no shader on any backend. Depth testing must be
// OFF while it draws: it is a backdrop, not geometry.
//
// `sunDir` points TOWARD the sun. `radius` should sit well inside the far
// plane; the dome is centred on `camPos`, so no amount of flying leaves it.
inline void buildPreviewSkyDome(const glm::vec3& camPos, float radius,
                                const glm::vec3& sunDir, std::vector<float>& out)
{
    constexpr int kSeg   = 24;   // around
    constexpr int kRings = 12;   // pole to pole
    const glm::vec3 sun = glm::normalize(sunDir);

    // One evaluation per unique grid vertex, not per triangle corner:
    // SkyColorCPU runs a scattering integral, and the naive version would run it
    // six times for every quad.
    struct V { glm::vec3 p, c; };
    std::vector<V> grid((kRings + 1) * (kSeg + 1));
    for (int r = 0; r <= kRings; ++r)
    {
        const float phi = 3.14159265f * static_cast<float>(r) / kRings;   // 0 = up
        const float sy = std::cos(phi), sr = std::sin(phi);
        for (int s = 0; s <= kSeg; ++s)
        {
            const float th = 6.2831853f * static_cast<float>(s) / kSeg;
            const glm::vec3 dir(sr * std::sin(th), sy, sr * std::cos(th));
            V& v = grid[static_cast<size_t>(r) * (kSeg + 1) + s];
            v.p  = camPos + dir * radius;
            v.c  = SkyColorCPU(dir, sun);
        }
    }

    auto push = [&out](const V& v) {
        out.insert(out.end(), { v.p.x, v.p.y, v.p.z, v.c.r, v.c.g, v.c.b });
    };
    for (int r = 0; r < kRings; ++r)
        for (int s = 0; s < kSeg; ++s)
        {
            const V& a = grid[static_cast<size_t>(r)     * (kSeg + 1) + s];
            const V& b = grid[static_cast<size_t>(r)     * (kSeg + 1) + s + 1];
            const V& c = grid[static_cast<size_t>(r + 1) * (kSeg + 1) + s + 1];
            const V& d = grid[static_cast<size_t>(r + 1) * (kSeg + 1) + s];
            push(a); push(b); push(c);
            push(a); push(c); push(d);
        }
}

} // namespace HE
