#pragma once
#include <Renderer/IRenderer.h>
#include <Math/Math.h>
#include <Math/AABB.h>
#include <algorithm>

// ─── Where the 3D cloud deck is, and which transparent draws lie behind it ────
// The 3D clouds (cloudMode 1) are composited over the OPAQUE scene only: the march ends at the opaque
// depth (SkyShaderSource.h, applyClouds3D*). Transparent draws write no depth, so the composite cannot
// know where they are. What it can be told is a per-DRAW fact: the deck is a horizontal slab, so when the
// camera is on one side of it and a draw lies wholly on the other, EVERY ray from the camera to that draw
// crosses the entire deck first — all of the clouds on it are in front of the draw, none between it and
// the opaque surface behind it. Those draws ("behind the deck") are drawn BEFORE the cloud composite and
// so get covered by it; every other transparent draw is drawn after it, as before.
//
//   opaque -> sky -> [transparent behind the deck] -> cloud composite -> [the other transparents] -> particles
//
// Back-to-front order between the two groups holds per pixel (a behind-the-deck point is always farther
// than the deck, a camera-side point always nearer), so the blend order stays right.
//
// Not covered (they keep the "after the composite" behaviour): a draw that straddles the deck, any view
// with the camera inside the deck, and a behind-the-deck draw with nothing opaque behind it (its pixels
// are sky, whose clouds were composited before it).
namespace HE
{

// MIRRORS kCloudRefAltitude / the slab of applyClouds3D + applyClouds3DReal in SkyShaderSource.h: the
// deck's base is the scene's cloudHeight (>= 1), it is 1.5 * kCloudRefAltitude thick, and the Realistic
// style reaches 0.12 of that below the base. tests/test_sky_shader.cpp pins the shader text to these.
inline constexpr float kCloudDeckRefAltitude = 200.0f;
inline constexpr float kCloudDeckThickness   = kCloudDeckRefAltitude * 1.5f;
inline constexpr float kCloudDeckBelowBase   = 0.12f * kCloudDeckThickness;
// A little air around the slab so a draw that just touches it is "straddling", not "behind".
inline constexpr float kCloudDeckMargin      = 8.0f;

struct CloudDeck { float bottom = 0.0f, top = 0.0f; };

inline CloudDeck cloudDeckOf(float cloudHeight)
{
	const float base = std::max(cloudHeight, 1.0f);
	return { base - kCloudDeckBelowBase - kCloudDeckMargin, base + kCloudDeckThickness + kCloudDeckMargin };
}

// Whether the composite draws 3D clouds at all for this environment.
inline bool cloudDeckActive(const EnvironmentSettings& env)
{
	return env.skyEnabled && env.cloudMode == 1 && env.cloudCoverage > 0.0f;
}

// `minY`/`maxY` = the draw's world-space vertical extent.
inline bool transparentBehindClouds(const EnvironmentSettings& env, float camY, float minY, float maxY)
{
	if (!cloudDeckActive(env)) return false;
	const CloudDeck d = cloudDeckOf(env.cloudHeight);
	if (camY > d.top    && maxY < d.bottom) return true;   // camera above the deck, draw below it
	if (camY < d.bottom && minY > d.top)    return true;   // camera below the deck, draw above it
	return false;
}

// Same, from the draw's object-space bounds and model matrix. No usable bounds -> not behind.
inline bool transparentBehindClouds(const EnvironmentSettings& env, const glm::vec3& camPos,
                                    const AABB& localBounds, const glm::mat4& model)
{
	if (!cloudDeckActive(env) || !localBounds.isValid()) return false;
	const AABB w = localBounds.transformed(model);
	return w.isValid() && transparentBehindClouds(env, camPos.y, w.min.y, w.max.y);
}

} // namespace HE
