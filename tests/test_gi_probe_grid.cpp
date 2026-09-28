#include "doctest.h"
#include <HorizonRendering/GIProbeGrid.h>

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

// The DDGI probe-grid fit all five backends share (Thema 80, step 4: terrain in
// the probe grid). The old fit — fixed 4 m, ≤10 probes per axis, 36 m at most —
// is reproduced here verbatim as the reference for the "small scenes stay
// bit-identical" guarantee.

namespace
{
struct OldFit { glm::vec3 origin; glm::ivec3 counts; };

OldFit oldFit(const HE::AABB& sceneBox)
{
	constexpr float kGIProbeSpacing = 4.0f;
	constexpr int   kGIMaxProbesPerAxis = 10;
	const glm::vec3 padded = sceneBox.extents() + glm::vec3(kGIProbeSpacing);
	OldFit o;
	o.counts = glm::ivec3(
		std::clamp(static_cast<int>(std::ceil(padded.x * 2.0f / kGIProbeSpacing)) + 1, 2, kGIMaxProbesPerAxis),
		std::clamp(static_cast<int>(std::ceil(padded.y * 2.0f / kGIProbeSpacing)) + 1, 2, kGIMaxProbesPerAxis),
		std::clamp(static_cast<int>(std::ceil(padded.z * 2.0f / kGIProbeSpacing)) + 1, 2, kGIMaxProbesPerAxis));
	const glm::vec3 gridSpan = glm::vec3(o.counts - 1) * kGIProbeSpacing;
	o.origin = sceneBox.center() - gridSpan * 0.5f;
	return o;
}

HE::AABB box(glm::vec3 mn, glm::vec3 mx) { HE::AABB b; b.min = mn; b.max = mx; return b; }

bool covers(const HE::GIProbeGridFit& f, const HE::AABB& b)
{
	return glm::all(glm::lessThanEqual(f.origin, b.min))
	    && glm::all(glm::greaterThanEqual(f.maxCorner(), b.max));
}

RenderObject obj(uint32_t entity, uint64_t mesh, glm::vec3 mn, glm::vec3 mx)
{
	RenderObject o;
	o.entityId = entity;
	o.meshAssetId.hi = mesh; o.meshAssetId.lo = mesh * 7 + 1;
	o.worldBounds = box(mn, mx);
	return o;
}
} // namespace

TEST_CASE("GI probe grid: scenes the old 36 m grid covered fit bit-identically")
{
	const HE::AABB scenes[] = {
		box({ -1, 0, -1 }, { 1, 2, 1 }),                 // one cube
		box({ -10, -0.5f, -8 }, { 12, 6, 9 }),           // a room
		box({ 3, 1, 3 }, { 30, 20, 30 }),                // off-centre, ≈ 27 m
		box({ -14, 0, -14 }, { 14, 14, 14 }),            // right at the old cap
	};
	for (const HE::AABB& s : scenes)
	{
		const OldFit o = oldFit(s);
		REQUIRE(glm::all(glm::lessThanEqual(o.counts, glm::ivec3(10)))); // old fit not clamped
		const HE::GIProbeGridFit f = HE::FitGIProbeGrid(s);
		CHECK(f.spacing == 4.0f);
		CHECK(f.counts == o.counts);
		CHECK(f.origin == o.origin);
	}
}

TEST_CASE("GI probe grid: a 100 m terrain is covered edge to edge within the budget")
{
	// Default TerrainComponent: 100 × 100 m, a few metres of relief + skirts.
	const HE::AABB terrain = box({ -50, 296, -50 }, { 50, 312, 50 });

	const OldFit o = oldFit(terrain);
	const glm::vec3 oldMax = o.origin + glm::vec3(o.counts - 1) * 4.0f;
	CHECK(oldMax.x - o.origin.x == doctest::Approx(36.0f));   // the gap: 36 of 100 m

	const HE::GIProbeGridFit f = HE::FitGIProbeGrid(terrain);
	REQUIRE(f.valid());
	CHECK(covers(f, terrain));
	CHECK(f.probeCount() <= HE::kGIProbeBudget);
	CHECK(f.spacing < 10.0f);                     // flat → the probes go sideways
	CHECK(f.counts.x > 10);
	CHECK(f.counts.x == f.counts.z);
	CHECK(f.counts.y < f.counts.x);
}

TEST_CASE("GI probe grid: huge and lopsided boxes still fit and stay covered")
{
	const HE::AABB boxes[] = {
		box({ -2000, -50, -2000 }, { 2000, 400, 2000 }), // 4 km landscape
		box({ 0, 0, 0 }, { 500, 1, 1 }),                 // long thin corridor
		box({ -1, -300, -1 }, { 1, 300, 1 }),            // tall tower
	};
	for (const HE::AABB& b : boxes)
	{
		const HE::GIProbeGridFit f = HE::FitGIProbeGrid(b);
		REQUIRE(f.valid());
		CHECK(covers(f, b));
		CHECK(f.probeCount() <= HE::kGIProbeBudget);
		CHECK(f.counts.x <= HE::kGIProbeMaxPerAxis);
		CHECK(f.counts.y <= HE::kGIProbeMaxPerAxis);
		CHECK(f.counts.z <= HE::kGIProbeMaxPerAxis);
		CHECK(f.spacing >= HE::kGIProbeMinSpacing);
	}
	CHECK_FALSE(HE::FitGIProbeGrid(HE::AABB{}).valid());   // empty scene → no grid
}

TEST_CASE("GI probe grid: refit only when geometry escapes or the scene shrinks a lot")
{
	const HE::AABB props = box({ -5, 0, -5 }, { 5, 3, 5 });
	const HE::GIProbeGridFit f = HE::FitGIProbeGrid(props);
	REQUIRE(f.valid());

	CHECK_FALSE(HE::GIProbeGridNeedsRefit(f, props));
	CHECK_FALSE(HE::GIProbeGridNeedsRefit(f, HE::AABB{}));
	// Nudging past the scene box but inside the padding: keep the grid.
	CHECK_FALSE(HE::GIProbeGridNeedsRefit(f, box({ -6, 0, -5 }, { 5, 3, 5 })));

	// A terrain added after the props (the old one-shot fit missed it).
	HE::AABB withTerrain = props;
	withTerrain.expand(box({ -50, -2, -50 }, { 50, 4, 50 }));
	CHECK(HE::GIProbeGridNeedsRefit(f, withTerrain));
	const HE::GIProbeGridFit big = HE::FitGIProbeGrid(withTerrain);
	CHECK(covers(big, withTerrain));

	// Terrain deleted again → back to the fine 4 m props grid.
	CHECK(HE::GIProbeGridNeedsRefit(big, props));
	// …but a slightly smaller scene inside a coarse grid does not churn.
	CHECK_FALSE(HE::GIProbeGridNeedsRefit(big, box({ -40, -2, -40 }, { 40, 4, 40 })));

	CHECK(HE::GIProbeGridNeedsRefit(HE::GIProbeGridFit{}, props)); // no grid yet
}

TEST_CASE("GI probe grid: scene signature tracks geometry, not motion")
{
	std::vector<RenderObject> scene = {
		obj(1, 10, { 0, 0, 0 }, { 1, 1, 1 }),
		obj(2, 11, { 5, 0, 0 }, { 6, 1, 1 }),
	};
	const uint64_t s0 = HE::GIProbeSceneSignature(scene);

	// Order does not matter (extraction order is not stable).
	std::vector<RenderObject> swapped = { scene[1], scene[0] };
	CHECK(HE::GIProbeSceneSignature(swapped) == s0);

	// Moving (a falling body) does not change it.
	std::vector<RenderObject> moved = scene;
	moved[1].worldBounds = box({ 5, -900, 0 }, { 6, -899, 1 });
	moved[1].transform[3] = glm::vec4(5, -900, 0, 1);
	CHECK(HE::GIProbeSceneSignature(moved) == s0);

	// Adding a terrain chunk, removing an object, or swapping a mesh (LOD) does.
	std::vector<RenderObject> added = scene;
	added.push_back(obj(3, 12, { -50, 0, -50 }, { 50, 2, 50 }));
	CHECK(HE::GIProbeSceneSignature(added) != s0);
	std::vector<RenderObject> removed = { scene[0] };
	CHECK(HE::GIProbeSceneSignature(removed) != s0);
	std::vector<RenderObject> lod = scene;
	lod[0].meshAssetId.lo += 1;
	CHECK(HE::GIProbeSceneSignature(lod) != s0);

	// Bounds union ignores invalid (unresolved) entries.
	std::vector<RenderObject> withInvalid = scene;
	withInvalid.push_back(RenderObject{});
	int unresolved = -1;
	const HE::AABB u = HE::GIProbeSceneBounds(withInvalid, &unresolved);
	CHECK(u.min == glm::vec3(0, 0, 0));
	CHECK(u.max == glm::vec3(6, 1, 1));
	CHECK(unresolved == 1);   // → the backends keep re-checking until it resolves
	HE::GIProbeSceneBounds(scene, &unresolved);
	CHECK(unresolved == 0);
}

TEST_CASE("GI probe grid: tracker re-checks on geometry, never on motion alone")
{
	HE::GIProbeGridTracker t;
	const uint64_t sig = 42;

	// First frame: nothing built yet → evaluate, whatever the counts.
	CHECK_FALSE(t.canSkip(false, sig));
	CHECK(t.shouldEvaluate(false, sig, 0));
	// Built, same objects, nothing rebuilt → the O(1) skip.
	CHECK(t.canSkip(true, sig));
	// Objects added/removed → evaluate.
	CHECK_FALSE(t.canSkip(true, sig + 1));
	CHECK(t.shouldEvaluate(true, sig + 1, 0));
	// A mesh rebuilt in place (sculpt, tessellation) → evaluate once.
	t.meshRebuilt = true;
	CHECK_FALSE(t.canSkip(true, sig + 1));
	CHECK(t.shouldEvaluate(true, sig + 1, 0));
	CHECK(t.canSkip(true, sig + 1));
}

TEST_CASE("GI probe grid: a permanently unresolvable object does not turn motion into refits")
{
	HE::GIProbeGridTracker t;
	const uint64_t sig = 7;
	// Two meshes not resident yet at the first fit.
	CHECK(t.shouldEvaluate(false, sig, 2));
	// While some stay unresolved the backend has to look at bounds every frame…
	CHECK_FALSE(t.canSkip(true, sig));
	// …but with nothing newly resolved (only things moved) no decision is due.
	for (int frame = 0; frame < 100; ++frame)
		CHECK_FALSE(t.shouldEvaluate(true, sig, 2));
	// One of them arrives → evaluate (the grid may now need to grow).
	CHECK(t.shouldEvaluate(true, sig, 1));
	// The other is a broken reference and never resolves: still no churn.
	for (int frame = 0; frame < 100; ++frame)
		CHECK_FALSE(t.shouldEvaluate(true, sig, 1));
	// Geometry changes are still seen through it.
	CHECK(t.shouldEvaluate(true, sig + 1, 1));
}
