#pragma once
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
// Coordinates are WORLD space with Y up. The ground sits a hair below y = 0 so
// it cannot z-fight with the grid lines lying exactly on the plane.

namespace HE
{

// Two triangles, 6 vertices (36 floats), spanning ±halfExtent in X/Z.
inline void buildPreviewGround(float halfExtent, std::vector<float>& out)
{
    constexpr float kGroundY = -0.002f;
    const glm::vec3 c(0.155f, 0.155f, 0.170f);
    const float h = halfExtent;
    const float quad[6][3] = {
        { -h, kGroundY, -h }, { -h, kGroundY,  h }, {  h, kGroundY,  h },
        { -h, kGroundY, -h }, {  h, kGroundY,  h }, {  h, kGroundY, -h },
    };
    for (const auto& v : quad)
        out.insert(out.end(), { v[0], v[1], v[2], c.r, c.g, c.b });
}

// Grid lines every `step` units out to ±halfExtent, with every tenth line
// brighter, the two world axes tinted, and a small triad marking the origin —
// which for a class preview is the character's OWN origin, the thing every
// component transform is relative to.
inline void buildPreviewGrid(float halfExtent, float step, std::vector<float>& out)
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
        push({ t, 0.0f, -halfExtent }, cz); push({ t, 0.0f, halfExtent }, cz);
        push({ -halfExtent, 0.0f, t }, cx); push({ halfExtent, 0.0f, t }, cx);
    }

    // Origin marker: the up axis (the ground grid already carries X and Z) plus
    // a short raised cross, so the origin stays readable when the camera looks
    // almost straight down the Y axis and the vertical line degenerates.
    const float a = step * 0.5f;
    push({ 0.0f, 0.0f, 0.0f }, axisY); push({ 0.0f, step, 0.0f }, axisY);
    push({ -a, 0.001f, 0.0f }, axisY); push({ a, 0.001f, 0.0f }, axisY);
    push({ 0.0f, 0.001f, -a }, axisY); push({ 0.0f, 0.001f, a }, axisY);
}

} // namespace HE
