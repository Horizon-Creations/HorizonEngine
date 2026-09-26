#pragma once
#include "RenderObject.h"
#include <Math/AABB.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// ─── DDGI probe-grid placement, shared by all five backends ──────────────────
// The grid used to be a fixed 4 m lattice clamped to 10 probes per axis, i.e.
// at most 36 m wide, fitted ONCE. A default 100 × 100 m terrain therefore got a
// 36 m patch of probes in its middle; everywhere else sampleDDGIIrradiance found
// no probe and returned 0 — and because GI REPLACES the diffuse sky ambient,
// those surfaces lost all ambient light, not just the bounce (Thema 80,
// docs/terrain-vegetation-gap-audit-2026-09-26.md §4). And a terrain added
// after the first props never made it in at all.
//
// Now: the spacing grows past 4 m until the whole scene box fits a fixed probe
// BUDGET (10³ = the old worst case, so probe count and atlas memory stay
// bounded; the probe rays' max distance grows with the grid, though),
// with a per-axis cap of 32 instead of 10 so a flat landscape spends its probes
// horizontally. Scenes the old grid already covered fit bit-identically (same
// 4 m spacing, same counts, same centred origin). The spacing reaches the
// shaders through gridOrigin.w, which every kernel already reads — no shader
// change.
//
// Refit policy (GIProbeGridTracker): the backends re-check only when the
// scene's GEOMETRY changed — objects added/removed (GIProbeSceneSignature), a
// mesh rebuilt through InvalidateMesh (sculpt, terrain LOD/tessellation), or a
// mesh whose bounds were unresolvable at the last check has resolved since —
// never on motion alone: a physics body falling off the world must not drag
// the grid after it, even while some other object's bounds stay unresolvable
// for good (a broken mesh reference). A re-check
// refits only when the scene box pokes out of the grid by more than half a
// spacing, or when the scene shrank enough that a fresh fit would be at least
// twice as fine. A refit recreates the probe atlases, so indirect light
// re-converges over the next frames (same as the first fit).
namespace HE
{

inline constexpr float kGIProbeMinSpacing = 4.0f; // metres; finest lattice (the old fixed one)
inline constexpr int   kGIProbeBudget     = 1000; // total probes — the old 10×10×10 worst case
inline constexpr int   kGIProbeMaxPerAxis = 32;   // lets a flat 128 m landscape keep 4 m spacing

struct GIProbeGridFit
{
	glm::vec3  origin{ 0.0f };   // world position of probe (0,0,0)
	glm::ivec3 counts{ 0 };      // probes per axis
	float      spacing = 0.0f;   // metres between neighbouring probes

	bool      valid()      const { return spacing > 0.0f && counts.x > 0 && counts.y > 0 && counts.z > 0; }
	int       probeCount() const { return counts.x * counts.y * counts.z; }
	glm::vec3 maxCorner()  const { return origin + glm::vec3(counts - 1) * spacing; }
};

// Probes per axis for a box of half-size `half` at spacing s: one spacing of
// padding on every side, at least 2 so trilinear interpolation has a pair.
inline glm::ivec3 GIProbeCountsFor(const glm::vec3& half, float s)
{
	auto axis = [s](float h) {
		return std::max(2, static_cast<int>(std::ceil((h + s) * 2.0f / s)) + 1);
	};
	return { axis(half.x), axis(half.y), axis(half.z) };
}

// Centred grid over `box`, as fine as the budget allows. Invalid (spacing 0)
// for an empty or non-finite box.
inline GIProbeGridFit FitGIProbeGrid(const AABB& box,
                                     float minSpacing = kGIProbeMinSpacing,
                                     int   budget     = kGIProbeBudget,
                                     int   maxPerAxis = kGIProbeMaxPerAxis)
{
	GIProbeGridFit f;
	if (!box.isValid()) return f;
	const glm::vec3 half = box.extents();
	if (!std::isfinite(half.x) || !std::isfinite(half.y) || !std::isfinite(half.z)) return f;

	auto fits = [&](const glm::ivec3& c) {
		return c.x * c.y * c.z <= budget
		    && c.x <= maxPerAxis && c.y <= maxPerAxis && c.z <= maxPerAxis;
	};
	float      s = minSpacing;
	glm::ivec3 c = GIProbeCountsFor(half, s);
	// 5 % steps: coarse enough to converge in < 250 iterations for any box a
	// float can hold, fine enough that the budget is used well.
	for (int i = 0; i < 1000 && !fits(c); ++i)
	{
		s *= 1.05f;
		c = GIProbeCountsFor(half, s);
	}
	if (!fits(c)) return f;

	f.spacing = s;
	f.counts  = c;
	f.origin  = box.center() - glm::vec3(c - 1) * s * 0.5f;
	return f;
}

// Union of every object's world bounds (callers refresh worldBounds from the
// real mesh bounds first — the extractor seeds proxies). `unresolved` counts
// objects left out because their bounds are invalid (mesh not uploaded yet):
// the signature alone would not notice those meshes arriving, so the tracker
// watches this count.
inline AABB GIProbeSceneBounds(const std::vector<RenderObject>& objects, int* unresolved = nullptr)
{
	AABB box;
	int  missing = 0;
	for (const RenderObject& obj : objects)
	{
		if (obj.worldBounds.isValid()) box.expand(obj.worldBounds);
		else                           ++missing;
	}
	if (unresolved) *unresolved = missing;
	return box;
}

// Order-independent fingerprint of WHICH geometry is in the scene (entity +
// mesh), deliberately blind to transforms. Changes when objects appear or
// disappear or swap meshes (LOD); stays put while things merely move.
inline uint64_t GIProbeSceneSignature(const std::vector<RenderObject>& objects)
{
	uint64_t sum = 0x9E3779B97F4A7C15ull * (objects.size() + 1);
	for (const RenderObject& obj : objects)
	{
		uint64_t h = (static_cast<uint64_t>(obj.entityId) << 32)
		           ^ obj.meshAssetId.hi ^ (obj.meshAssetId.lo * 0xBF58476D1CE4E5B9ull);
		h ^= h >> 31; h *= 0x94D049BB133111EBull; h ^= h >> 29;   // splitmix finaliser
		sum += h;
	}
	return sum;
}

// Per-backend bookkeeping for the re-check triggers above. Usage, per frame:
//   if (tracker.canSkip(built, sig)) return;          // O(1), the usual frame
//   … refresh worldBounds, box = GIProbeSceneBounds(objects, &unresolved) …
//   if (!tracker.shouldEvaluate(built, sig, unresolved)) return;
//   … GIProbeGridNeedsRefit / FitGIProbeGrid …
// and `meshRebuilt = true` from the backend's InvalidateMesh drain.
struct GIProbeGridTracker
{
	uint64_t sig         = 0;     // GIProbeSceneSignature at the last evaluation
	int      unresolved  = 0;     // objects with invalid bounds at the last evaluation
	bool     meshRebuilt = false; // set by the InvalidateMesh drain

	// Nothing that could move the fit happened: skip without touching bounds.
	bool canSkip(bool built, uint64_t s) const
	{
		return built && s == sig && !meshRebuilt && unresolved == 0;
	}
	// Bounds are known now — is a (re)fit decision due? Records the new state.
	// Pending unresolved objects alone only count once some of them RESOLVED;
	// otherwise a permanently unresolvable one would re-check every frame and
	// let plain motion refit the grid.
	bool shouldEvaluate(bool built, uint64_t s, int nowUnresolved)
	{
		const bool due = !built || s != sig || meshRebuilt || nowUnresolved < unresolved;
		sig         = s;
		unresolved  = nowUnresolved;
		meshRebuilt = false;
		return due;
	}
};

// Should an existing grid be replaced for this scene box? See the policy above.
inline bool GIProbeGridNeedsRefit(const GIProbeGridFit& current, const AABB& box)
{
	if (!box.isValid()) return false;
	if (!current.valid()) return true;
	const glm::vec3 tol(current.spacing * 0.5f);
	const glm::vec3 lo = current.origin - tol;
	const glm::vec3 hi = current.maxCorner() + tol;
	if (glm::any(glm::lessThan(box.min, lo)) || glm::any(glm::greaterThan(box.max, hi)))
		return true;
	const GIProbeGridFit fresh = FitGIProbeGrid(box);
	return fresh.valid() && fresh.spacing * 2.0f <= current.spacing;
}

} // namespace HE
