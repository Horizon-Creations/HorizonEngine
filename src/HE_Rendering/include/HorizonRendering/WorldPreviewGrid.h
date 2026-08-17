#pragma once
#include <glm/vec3.hpp>
#include <cmath>
#include <vector>

// ─── World-preview grid ─────────────────────────────────────────────────────
// The grid and the origin marker that IRenderer::RenderWorldPreview draws
// behind the world (see the Class Editor's viewport and the Mesh viewer).
//
// LINES ONLY. There used to be a filled ground quad under them, and a filled
// floor in a viewer is only ever in the way: it hides whatever sits below it —
// the underside of the mesh, and with a sky on, the whole lower half of the
// world. Lines say where the ground is without standing in front of anything.
//
// Free functions in a header, deliberately: every backend has to build the same
// grid, and one that is a shade darker or half a unit wider on OpenGL than on
// Metal is exactly the kind of difference nobody notices until an author
// switches machines and thinks the scale changed. Vertices are plain
// pos3 + color3 floats, which is the layout every backend's small line
// preview program already uses.
//
// Coordinates are WORLD space with Y up. Everything is laid out RELATIVE TO
// `origin` — for a class preview that is the character's own origin, the point
// every component transform is measured from, and it is normally the world
// origin. Centring the grid on it is what makes "objects sit relative to the
// origin" true in the picture and not just in the data.

namespace HE
{

// The preview's studio background, and the reference point for the colours
// below. LINEAR — the preview renders HDR and resolves through the scene's ACES
// tonemap + gamma encode, which lifts a value a long way: the 0.22 this used to
// be arrived on screen at about 0.60, a bright silver that fought with
// everything in front of it. 0.030 lands near 0.17, the dark neutral an asset
// viewer wants. Change this and the grid colours together or the contrast
// between them drifts.
constexpr float kPreviewBackground[3] = { 0.030f, 0.030f, 0.034f };

// Grid lines every `step` units out to ±halfExtent, with every tenth line
// brighter, the two axes THROUGH the origin tinted, and a small marker on the
// origin itself. Because the lines are laid out from `origin` rather than from
// world zero, the tinted pair always passes exactly through it.
inline void buildPreviewGrid(float halfExtent, float step, std::vector<float>& out,
                             const glm::vec3& origin = glm::vec3(0.0f))
{
    // LINEAR values feeding the same ACES + gamma resolve as the background —
    // roughly a quarter of what they would be if this drew straight to the
    // screen. Read them as "minor ≈ 0.28 on screen, major ≈ 0.33": the numbers
    // here look far darker than the result.
    const glm::vec3 minor(0.060f, 0.060f, 0.066f);
    const glm::vec3 major(0.085f, 0.085f, 0.092f);
    const glm::vec3 axisX(0.130f, 0.048f, 0.048f);
    const glm::vec3 axisZ(0.048f, 0.064f, 0.150f);
    const glm::vec3 axisY(0.052f, 0.130f, 0.062f);

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

} // namespace HE
