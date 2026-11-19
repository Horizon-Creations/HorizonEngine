#include <HorizonRendering/GiHeightfield.h>
#include "doctest.h"
#include <HorizonRendering/GiBvh.h>

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

// The CPU BVH is the software acceleration structure for the OpenGL-compute GI
// port (Windows/Linux, GL 4.3+). Its traversal is mirrored 1:1 by the GLSL
// kernel, so these tests are the ground truth the blind port leans on: if the
// algorithm is right here, the shader runs the same math.

namespace
{
// Brute-force reference: Möller-Trumbore over every triangle (both faces).
bool bruteForce(const std::vector<glm::vec3>& tris, const glm::vec3& o, const glm::vec3& d,
                float tMin, float tMax, float& tOut)
{
	bool  hit  = false;
	float best = tMax;
	for (size_t i = 0; i + 2 < tris.size(); i += 3)
	{
		const glm::vec3 e1 = tris[i + 1] - tris[i];
		const glm::vec3 e2 = tris[i + 2] - tris[i];
		const glm::vec3 p  = glm::cross(d, e2);
		const float det    = glm::dot(e1, p);
		if (std::fabs(det) < 1e-9f) continue;
		const float invDet = 1.0f / det;
		const glm::vec3 s  = o - tris[i];
		const float u = glm::dot(s, p) * invDet;
		if (u < 0.0f || u > 1.0f) continue;
		const glm::vec3 q = glm::cross(s, e1);
		const float v = glm::dot(d, q) * invDet;
		if (v < 0.0f || u + v > 1.0f) continue;
		const float t = glm::dot(e2, q) * invDet;
		if (t <= tMin || t >= best) continue;
		best = t;
		hit  = true;
	}
	tOut = best;
	return hit;
}

// Packs loose triangle vertices into (positions, indices) suitable for buildGiBvh.
void packSoup(const std::vector<glm::vec3>& tris, std::vector<float>& pos, std::vector<uint32_t>& idx)
{
	pos.clear(); idx.clear();
	for (size_t i = 0; i < tris.size(); ++i)
	{
		pos.insert(pos.end(), { tris[i].x, tris[i].y, tris[i].z });
		idx.push_back(static_cast<uint32_t>(i));
	}
}
} // namespace

TEST_CASE("GiBvh: single triangle hit and miss")
{
	const std::vector<float> pos = { 0,0,0,  1,0,0,  0,1,0 };
	const std::vector<uint32_t> idx = { 0, 1, 2 };
	const HE::GiBvh bvh = HE::buildGiBvh(pos.data(), 3, 3, idx.data(), idx.size());
	REQUIRE(bvh.valid());
	CHECK(bvh.triangles.size() == 1);

	float t; uint32_t tri;
	// Straight through the triangle interior.
	CHECK(HE::giBvhIntersect(bvh, glm::vec3(0.25f, 0.25f, 1.0f), glm::vec3(0, 0, -1), 0.001f, 100.0f, false, t, tri));
	CHECK(t == doctest::Approx(1.0f).epsilon(1e-4));
	// Backface (ray from behind) must ALSO hit — occluders are two-sided.
	CHECK(HE::giBvhIntersect(bvh, glm::vec3(0.25f, 0.25f, -1.0f), glm::vec3(0, 0, 1), 0.001f, 100.0f, false, t, tri));
	// Misses to the side / beyond tMax / before tMin.
	CHECK_FALSE(HE::giBvhIntersect(bvh, glm::vec3(2.0f, 2.0f, 1.0f), glm::vec3(0, 0, -1), 0.001f, 100.0f, false, t, tri));
	CHECK_FALSE(HE::giBvhIntersect(bvh, glm::vec3(0.25f, 0.25f, 1.0f), glm::vec3(0, 0, -1), 0.001f, 0.5f, false, t, tri));
	CHECK_FALSE(HE::giBvhIntersect(bvh, glm::vec3(0.25f, 0.25f, 1.0f), glm::vec3(0, 0, -1), 2.0f, 100.0f, false, t, tri));
}

TEST_CASE("GiBvh: random soup matches brute force (closest hit + any-hit agreement)")
{
	std::mt19937 rng(1337);
	std::uniform_real_distribution<float> u01(-1.0f, 1.0f);
	std::uniform_real_distribution<float> u10(-10.0f, 10.0f);

	std::vector<glm::vec3> soup;
	for (int i = 0; i < 200; ++i)
	{
		const glm::vec3 base(u10(rng), u10(rng), u10(rng));
		soup.push_back(base);
		soup.push_back(base + glm::vec3(u01(rng), u01(rng), u01(rng)) * 2.0f);
		soup.push_back(base + glm::vec3(u01(rng), u01(rng), u01(rng)) * 2.0f);
	}
	std::vector<float> pos; std::vector<uint32_t> idx;
	packSoup(soup, pos, idx);
	const HE::GiBvh bvh = HE::buildGiBvh(pos.data(), soup.size(), 3, idx.data(), idx.size());
	REQUIRE(bvh.valid());
	CHECK(bvh.triangles.size() == 200);

	int hits = 0;
	for (int r = 0; r < 500; ++r)
	{
		const glm::vec3 o(u10(rng), u10(rng), u10(rng));
		glm::vec3 d(u01(rng), u01(rng), u01(rng));
		if (glm::length(d) < 1e-3f) d = glm::vec3(1, 0, 0);
		d = glm::normalize(d);

		float tRef, tBvh; uint32_t tri;
		const bool refHit = bruteForce(soup, o, d, 0.001f, 1000.0f, tRef);
		const bool bvhHit = HE::giBvhIntersect(bvh, o, d, 0.001f, 1000.0f, false, tBvh, tri);
		REQUIRE(refHit == bvhHit);
		if (refHit)
		{
			CHECK(tBvh == doctest::Approx(tRef).epsilon(1e-3));
			++hits;
		}
		// anyHit must agree on the hit/no-hit verdict (t may differ).
		float tAny; uint32_t triAny;
		CHECK(HE::giBvhIntersect(bvh, o, d, 0.001f, 1000.0f, true, tAny, triAny) == refHit);
	}
	CHECK(hits > 50); // sanity: the scene is dense enough that the test means something
}

TEST_CASE("GiBvh: interleaved stride-8 positions (engine mesh layout) match packed")
{
	// Same quad twice: packed xyz vs interleaved pos3+normal3+uv2 (stride 8).
	const std::vector<float> packed = { -1,-1,0,  1,-1,0,  1,1,0,  -1,1,0 };
	std::vector<float> inter;
	for (int v = 0; v < 4; ++v)
	{
		inter.insert(inter.end(), { packed[v*3], packed[v*3+1], packed[v*3+2] });
		inter.insert(inter.end(), { 0,0,1, 0.5f,0.5f }); // normal + uv filler
	}
	const std::vector<uint32_t> idx = { 0,1,2, 0,2,3 };
	const HE::GiBvh a = HE::buildGiBvh(packed.data(), 4, 3, idx.data(), idx.size());
	const HE::GiBvh b = HE::buildGiBvh(inter.data(),  4, 8, idx.data(), idx.size());
	REQUIRE(a.valid()); REQUIRE(b.valid());

	std::mt19937 rng(42);
	std::uniform_real_distribution<float> u(-2.0f, 2.0f);
	for (int r = 0; r < 100; ++r)
	{
		const glm::vec3 o(u(rng), u(rng), 3.0f);
		const glm::vec3 d = glm::normalize(glm::vec3(u(rng) * 0.2f, u(rng) * 0.2f, -1.0f));
		float ta, tb; uint32_t tri;
		const bool ha = HE::giBvhIntersect(a, o, d, 0.001f, 100.0f, false, ta, tri);
		const bool hb = HE::giBvhIntersect(b, o, d, 0.001f, 100.0f, false, tb, tri);
		REQUIRE(ha == hb);
		if (ha) CHECK(ta == doctest::Approx(tb).epsilon(1e-5));
	}
}

TEST_CASE("GiBvh: rays from inside a closed cube always hit (occlusion correctness)")
{
	// Unit cube as 12 triangles — the shadow-ray guarantee: from inside a closed
	// mesh, EVERY direction must report an occluder.
	const std::vector<float> pos = {
		-1,-1,-1,  1,-1,-1,  1,1,-1,  -1,1,-1,   // z-
		-1,-1, 1,  1,-1, 1,  1,1, 1,  -1,1, 1 }; // z+
	const std::vector<uint32_t> idx = {
		0,1,2, 0,2,3,  4,6,5, 4,7,6,   // z- / z+
		0,4,5, 0,5,1,  3,2,6, 3,6,7,   // y- / y+
		0,3,7, 0,7,4,  1,5,6, 1,6,2 }; // x- / x+
	const HE::GiBvh bvh = HE::buildGiBvh(pos.data(), 8, 3, idx.data(), idx.size(), 2);
	REQUIRE(bvh.valid());
	CHECK(bvh.triangles.size() == 12);
	CHECK(bvh.nodes.size() > 1); // leafSize 2 forces real subdivision

	std::mt19937 rng(7);
	std::uniform_real_distribution<float> u(-1.0f, 1.0f);
	for (int r = 0; r < 300; ++r)
	{
		glm::vec3 d(u(rng), u(rng), u(rng));
		if (glm::length(d) < 1e-3f) d = glm::vec3(0, 1, 0);
		d = glm::normalize(d);
		float t; uint32_t tri;
		REQUIRE(HE::giBvhIntersect(bvh, glm::vec3(0.0f), d, 0.001f, 100.0f, true, t, tri));
	}
	// And from OUTSIDE pointing away: never hits.
	float t; uint32_t tri;
	CHECK_FALSE(HE::giBvhIntersect(bvh, glm::vec3(5, 0, 0), glm::vec3(1, 0, 0), 0.001f, 100.0f, true, t, tri));
}

TEST_CASE("GiBvh: degenerate inputs are safe")
{
	float t; uint32_t tri;
	// Empty / null.
	CHECK_FALSE(HE::buildGiBvh(nullptr, 0, 3, nullptr, 0).valid());
	const std::vector<float> pos = { 0,0,0, 1,0,0, 0,1,0 };
	const std::vector<uint32_t> few = { 0, 1 }; // < 3 indices
	CHECK_FALSE(HE::buildGiBvh(pos.data(), 3, 3, few.data(), few.size()).valid());
	// Out-of-range indices are skipped, not crashed on.
	const std::vector<uint32_t> bad = { 0, 1, 99 };
	CHECK_FALSE(HE::buildGiBvh(pos.data(), 3, 3, bad.data(), bad.size()).valid());
	// Intersect on an invalid BVH.
	HE::GiBvh empty;
	CHECK_FALSE(HE::giBvhIntersect(empty, glm::vec3(0), glm::vec3(0, 0, 1), 0.0f, 1.0f, true, t, tri));
	// Many coincident triangles (degenerate centroid spread → oversized leaf, no infinite recursion).
	std::vector<float> same; std::vector<uint32_t> sameIdx;
	for (int i = 0; i < 32; ++i)
	{
		same.insert(same.end(), { 0,0,0, 1,0,0, 0,1,0 });
		sameIdx.insert(sameIdx.end(), { static_cast<uint32_t>(i*3), static_cast<uint32_t>(i*3+1), static_cast<uint32_t>(i*3+2) });
	}
	const HE::GiBvh dup = HE::buildGiBvh(same.data(), 96, 3, sameIdx.data(), sameIdx.size());
	REQUIRE(dup.valid());
	CHECK(HE::giBvhIntersect(dup, glm::vec3(0.25f, 0.25f, 1.0f), glm::vec3(0, 0, -1), 0.001f, 10.0f, true, t, tri));
}

TEST_CASE("GI height-field pyramid: every cell's maximum covers the vertices it stands for")
{
    GiHeightfield hf;
    hf.res = 65;
    auto field = std::make_shared<std::vector<float>>(size_t(hf.res) * hf.res);
    uint32_t s = 12345u;
    for (float& h : *field) { s = s * 1664525u + 1013904223u; h = float(s >> 8) / float(1u << 24) * 40.0f - 5.0f; }
    hf.data = field;
    const HE::GiHeightPyramid p = HE::BuildGiHeightPyramid(hf);
    REQUIRE(p.levels.size() >= 4);
    CHECK(p.dims[0] == 65);
    CHECK(p.levels[0] == *field);
    CHECK(p.maxHeight == *std::max_element(field->begin(), field->end()));
    for (size_t k = 1; k < p.levels.size(); ++k)
    {
        CHECK(p.dims[k] == std::max(1, p.dims[k - 1] >> 1));   // the mip rule the textures use
        const int span = 1 << k;
        for (int j = 0; j < p.dims[k]; ++j)
            for (int i = 0; i < p.dims[k]; ++i)
            {
                float worst = -1e30f;
                for (int z = j * span; z <= std::min((j + 1) * span + span / 2, 64); ++z)   // the cell plus the half-cell a step can reach
                    for (int x = i * span; x <= std::min((i + 1) * span + span / 2, 64); ++x)
                        worst = std::max(worst, (*field)[size_t(z) * 65 + x]);
                // Conservative: never below any vertex of its range (it may be above, where the 4x4 blocks overlap).
                if (!(p.levels[k][size_t(j) * p.dims[k] + i] >= worst)) { CHECK(p.levels[k][size_t(j) * p.dims[k] + i] >= worst); return; }
            }
    }
    // A field too small to pyramid is refused rather than half-built.
    GiHeightfield tiny; tiny.res = 4; tiny.data = std::make_shared<std::vector<float>>(16, 0.0f);
    CHECK(HE::BuildGiHeightPyramid(tiny).levels.empty());
}

// ── Closest-hit march (Thema 326, step 16: reflections against the tile landscape) ───────────────
// GiHeightPyramidClosestHitRef is the CPU reference the GLSL hfClosestHit (OpenGLRenderer.cpp)
// mirrors by hand, same relationship as the BVH tests above have to buildGiBvh/giBvhIntersect.

TEST_CASE("GiHeightPyramidClosestHitRef: a tilted plane is hit at (about) its analytic height and normal")
{
    const int   res   = 32;
    const float resM1 = float(res - 1);
    const float a = 0.2f, b = -0.1f, h0 = 10.0f;
    GiHeightfield hf; hf.res = res;
    auto field = std::make_shared<std::vector<float>>(size_t(res) * res);
    for (int j = 0; j < res; ++j)
        for (int i = 0; i < res; ++i)
            (*field)[size_t(j) * res + i] = h0 + a * (float(i) - resM1 * 0.5f) + b * (float(j) - resM1 * 0.5f);
    hf.data = field;
    const HE::GiHeightPyramid pyr = HE::BuildGiHeightPyramid(hf);
    REQUIRE(!pyr.levels.empty());

    const float      invSize = 1.0f / resM1; // 1 texel == 1 local unit, origin at the field's centre
    const glm::vec3  o(3.0f, 50.0f, -4.0f);
    const glm::vec3  d = glm::normalize(glm::vec3(0.3f, -1.0f, 0.2f));
    float t; glm::vec3 n;
    REQUIRE(HE::GiHeightPyramidClosestHitRef(pyr, o, d, 0.0f, 1000.0f, invSize, invSize, t, n));

    const glm::vec3 hit        = o + d * t;
    const float      expectedH = h0 + a * hit.x + b * hit.z;
    // Generous absolute tolerance: the march only resolves the crossing to within half a texel's
    // step at the finest mip level (the same granularity hfAnyHit uses for shadows), so the hit is
    // not at the exact geometric crossing.
    CHECK(std::fabs(hit.y - expectedH) < 1.0f);
    const glm::vec3 expectedN = glm::normalize(glm::vec3(-a, 1.0f, -b));
    CHECK(glm::dot(n, expectedN) > 0.99f); // same direction, loose on magnitude noise
}

TEST_CASE("GiHeightPyramidClosestHitRef: misses above the field, hits once the ray enters its height range")
{
    const int   res   = 64; // wide footprint: a mild lateral drift over a 45-unit fall stays inside it
    const float resM1 = float(res - 1);
    GiHeightfield hf; hf.res = res;
    hf.data = std::make_shared<std::vector<float>>(size_t(res) * res, 5.0f);
    const HE::GiHeightPyramid pyr = HE::BuildGiHeightPyramid(hf);
    REQUIRE(!pyr.levels.empty());
    const float invSize = 1.0f / resM1;

    float t; glm::vec3 n;
    const glm::vec3 oblique = glm::normalize(glm::vec3(0.05f, -1.0f, 0.03f));
    // Travelling further away from the field (up and out): never crosses it.
    CHECK_FALSE(HE::GiHeightPyramidClosestHitRef(pyr, glm::vec3(0, 50, 0), -oblique, 0.0f, 1000.0f, invSize, invSize, t, n));
    // Same start, aimed down through the flat top at y = 5.
    REQUIRE(HE::GiHeightPyramidClosestHitRef(pyr, glm::vec3(0, 50, 0), oblique, 0.0f, 1000.0f, invSize, invSize, t, n));
    CHECK(n.y > 0.9f); // flat field: normal points straight up
}

TEST_CASE("GiHeightPyramidClosestHitRef: closest hit agrees with a dense per-texel brute force on a bumpy field")
{
    const int   res   = 64;
    const float resM1 = float(res - 1);
    GiHeightfield hf; hf.res = res;
    auto field = std::make_shared<std::vector<float>>(size_t(res) * res);
    for (int j = 0; j < res; ++j)
        for (int i = 0; i < res; ++i)
            (*field)[size_t(j) * res + i] = 3.0f * std::sin(0.3f * i) * std::cos(0.25f * j) + 0.05f * i;
    hf.data = field;
    const HE::GiHeightPyramid pyr = HE::BuildGiHeightPyramid(hf);
    REQUIRE(!pyr.levels.empty());
    const float invSize = 1.0f / resM1;

    // Dense reference, deliberately NOT using the pyramid's mip-skip: steps a fraction of a texel at
    // a time and reports the first crossing, so agreement is a real check on the accelerated march.
    auto denseBilinear = [&](float x, float z) {
        const float cx = std::clamp(x + resM1 * 0.5f, 0.0f, resM1), cz = std::clamp(z + resM1 * 0.5f, 0.0f, resM1);
        const int x0 = int(std::floor(cx)), z0 = int(std::floor(cz));
        const int x1 = std::min(x0 + 1, res - 1), z1 = std::min(z0 + 1, res - 1);
        const float fx = cx - x0, fz = cz - z0;
        const auto& f = *field;
        const float h00 = f[size_t(z0) * res + x0], h10 = f[size_t(z0) * res + x1];
        const float h01 = f[size_t(z1) * res + x0], h11 = f[size_t(z1) * res + x1];
        return (h00 * (1 - fx) + h10 * fx) * (1 - fz) + (h01 * (1 - fx) + h11 * fx) * fz;
    };
    auto denseHit = [&](const glm::vec3& o, const glm::vec3& d, float& outT) {
        for (float t = 0.0f; t < 200.0f; t += 0.02f)
            if (o.y + d.y * t < denseBilinear(o.x + d.x * t, o.z + d.z * t))
            { outT = t; return true; }
        return false;
    };

    // Field half-width is resM1/2 ≈ 31.5; starting points and lateral drift are kept well inside it
    // for the whole ~35-unit fall from o.y, or the ray legitimately leaves the (necessarily finite)
    // footprint before it would reach the surface — a real miss the bounded march is right to
    // report, which a dense march with no footprint bound (clamped-edge sampling forever) would not.
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> xz(-15.0f, 15.0f), side(0.1f, 0.3f), sign01(0.0f, 1.0f);
    int checked = 0;
    for (int i = 0; i < 40; ++i)
    {
        const glm::vec3 o(xz(rng), 30.0f, xz(rng));
        // Horizontal component kept away from zero: a purely vertical ray makes the march's
        // texel-pacing step size blow up (division by a near-zero horizontal speed) — a pre-
        // existing property of hfAnyHit's own march, not something this closest-hit variant adds.
        const float dx = side(rng) * (sign01(rng) < 0.5f ? -1.0f : 1.0f);
        const float dz = side(rng) * (sign01(rng) < 0.5f ? -1.0f : 1.0f);
        const glm::vec3 d = glm::normalize(glm::vec3(dx, -1.0f, dz));

        float refT = 0.0f, gotT = 0.0f; glm::vec3 n;
        const bool refHit = denseHit(o, d, refT);
        const bool gotHit = HE::GiHeightPyramidClosestHitRef(pyr, o, d, 0.0f, 150.0f, invSize, invSize, gotT, n);
        REQUIRE(refHit == gotHit);
        // The march's finest step is half a TEXEL along (x,z) (0.5 / texLen, texLen = the ray's
        // horizontal speed in texels per unit t) — for a mostly-vertical ray that is several units
        // of t, so the reported hit lands up to about one such step PAST the true crossing (it only
        // samples at those intervals, never before). hfAnyHit has this exact same coarseness for
        // shadows; a loose, one-sided-in-practice bound is the honest check here, not a tight one.
        if (refHit) { CHECK(std::fabs(gotT - refT) < 3.0f); ++checked; }
    }
    CHECK(checked > 20); // most rays should actually hit, or the comparison above proves nothing
}

// ── Refit: the same tree over moved vertices (re-stitched / sculpted terrain chunks) ──────────────
namespace
{
// An (n+1)x(n+1) height-field grid with SHARED vertices, 3 floats per vertex: the topology of a terrain chunk.
void makeGrid(int n, std::vector<float>& pos, std::vector<uint32_t>& idx)
{
	pos.clear(); idx.clear();
	for (int z = 0; z <= n; ++z)
		for (int x = 0; x <= n; ++x)
			pos.insert(pos.end(), { float(x), 0.5f * std::sin(0.7f * x) * std::cos(0.5f * z), float(z) });
	for (int z = 0; z < n; ++z)
		for (int x = 0; x < n; ++x)
		{
			const uint32_t a = uint32_t(z * (n + 1) + x), b = a + 1, c = a + uint32_t(n + 1), d = c + 1;
			idx.insert(idx.end(), { a, c, b, b, c, d });
		}
}
// What stitching does to a chunk: the vertices on one edge change height, the indices do not.
void moveEdge(std::vector<float>& pos, int n, float dy)
{
	for (int x = 0; x <= n; ++x) pos[size_t(x) * 3 + 1] += dy;   // the z = 0 row
}
std::vector<glm::vec3> expand(const std::vector<float>& pos, const std::vector<uint32_t>& idx)
{
	std::vector<glm::vec3> out;
	for (uint32_t i : idx) out.emplace_back(pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]);
	return out;
}
} // namespace

TEST_CASE("GiBvh refit: moved vertices give the brute-force answer, unmoved ones give the build")
{
	const int n = 24;
	std::vector<float> pos; std::vector<uint32_t> idx;
	makeGrid(n, pos, idx);
	std::vector<uint32_t> source;
	const HE::GiBvh built = HE::buildGiBvh(pos.data(), pos.size() / 3, 3, idx.data(), idx.size(), 4, &source);
	REQUIRE(built.valid());
	REQUIRE(source.size() == idx.size() / 3);

	// Unmoved vertices: the refit is the build, node for node.
	HE::GiBvh same;
	REQUIRE(HE::refitGiBvh(built.nodes, source, pos.data(), pos.size() / 3, 3, idx.data(), idx.size(), same));
	REQUIRE(same.nodes.size() == built.nodes.size());
	for (size_t i = 0; i < built.nodes.size(); ++i)
	{
		CHECK(same.nodes[i].boundsMin == built.nodes[i].boundsMin);
		CHECK(same.nodes[i].boundsMax == built.nodes[i].boundsMax);
		CHECK(same.nodes[i].leftFirst == built.nodes[i].leftFirst);
		CHECK(same.nodes[i].triCount == built.nodes[i].triCount);
	}

	// Moved: an edge row lifted 1.5 m, then every ray's closest hit must equal the brute force over the NEW triangles.
	std::vector<float> moved = pos;
	moveEdge(moved, n, 1.5f);
	HE::GiBvh fitted;
	REQUIRE(HE::refitGiBvh(built.nodes, source, moved.data(), moved.size() / 3, 3, idx.data(), idx.size(), fitted));
	const std::vector<glm::vec3> soup = expand(moved, idx);
	std::mt19937 rng(7);
	std::uniform_real_distribution<float> u(0.0f, float(n));
	int hits = 0;
	for (int i = 0; i < 400; ++i)
	{
		const glm::vec3 o(u(rng), 6.0f, u(rng));
		const glm::vec3 d = glm::normalize(glm::vec3(0.2f * (u(rng) / n - 0.5f), -1.0f, 0.2f * (u(rng) / n - 0.5f)));
		float tRef = 0.0f, tBvh = 0.0f; uint32_t tri = 0;
		const bool ref = bruteForce(soup, o, d, 0.0f, 100.0f, tRef);
		const bool got = HE::giBvhIntersect(fitted, o, d, 0.0f, 100.0f, false, tBvh, tri);
		CHECK(ref == got);
		if (ref && got) { CHECK(tBvh == doctest::Approx(tRef).epsilon(1e-4)); ++hits; }
	}
	CHECK(hits > 300);

	// A fit that does not match the tree is refused, not half-written.
	HE::GiBvh bad;
	std::vector<uint32_t> badSource = source; badSource[3] = uint32_t(idx.size());   // past the index buffer
	CHECK_FALSE(HE::refitGiBvh(built.nodes, badSource, pos.data(), pos.size() / 3, 3, idx.data(), idx.size(), bad));
	CHECK_FALSE(bad.valid());
}

TEST_CASE("GiBvhRefitCache: first rebuild builds, later ones refit, a changed topology rebuilds")
{
	const int n = 16;
	std::vector<float> pos; std::vector<uint32_t> idx;
	makeGrid(n, pos, idx);
	HE::GiBvhRefitCache cache;
	const HE::UUID id = HE::UUID::generate();
	const HE::UUID other = HE::UUID::generate();

	cache.build(id, pos.data(), pos.size() / 3, 3, idx.data(), idx.size());          // first sight: build, remember nothing
	CHECK(cache.refits() == 0);
	cache.build(id, pos.data(), pos.size() / 3, 3, idx.data(), idx.size());          // second: build, now remembered
	CHECK(cache.refits() == 0);
	CHECK(cache.rebuilds() == 2);

	moveEdge(pos, n, 1.0f);
	const HE::GiBvh viaCache = cache.build(id, pos.data(), pos.size() / 3, 3, idx.data(), idx.size());
	CHECK(cache.refits() == 1);
	// And it is a usable tree for the moved geometry: straight down through the lifted edge.
	float t = 0.0f; uint32_t tri = 0;
	CHECK(HE::giBvhIntersect(viaCache, glm::vec3(0.5f, 9.0f, 0.2f), glm::vec3(0, -1, 0), 0.0f, 50.0f, false, t, tri));
	CHECK(t < 8.5f);   // the edge now stands ~1 m up, it is hit before the old y ~ 0 surface

	// Same id, other triangles (a different level): rebuild, and the new tree is what is remembered.
	std::vector<float> pos2; std::vector<uint32_t> idx2;
	makeGrid(8, pos2, idx2);
	cache.build(id, pos2.data(), pos2.size() / 3, 3, idx2.data(), idx2.size());
	CHECK(cache.refits() == 1);
	moveEdge(pos2, 8, 0.5f);
	cache.build(id, pos2.data(), pos2.size() / 3, 3, idx2.data(), idx2.size());
	CHECK(cache.refits() == 2);

	// An id that never repeats costs the cache nothing.
	const size_t before = cache.rebuilds();
	cache.build(other, pos.data(), pos.size() / 3, 3, idx.data(), idx.size());
	CHECK(cache.rebuilds() == before + 1);
	CHECK(cache.refits() == 2);
}

TEST_CASE("GiBvhRefitCache: a tree that was refitted many times is rebuilt")
{
	const int n = 12;
	std::vector<float> pos; std::vector<uint32_t> idx;
	makeGrid(n, pos, idx);
	HE::GiBvhRefitCache cache;
	const HE::UUID id = HE::UUID::generate();
	cache.build(id, pos.data(), pos.size() / 3, 3, idx.data(), idx.size());
	cache.build(id, pos.data(), pos.size() / 3, 3, idx.data(), idx.size());
	for (int i = 0; i < 60; ++i)
	{
		moveEdge(pos, n, 0.01f);
		cache.build(id, pos.data(), pos.size() / 3, 3, idx.data(), idx.size());
	}
	CHECK(cache.refits() >= 40);          // most of them refit ...
	CHECK(cache.rebuilds() >= 4);         // ... but the tree is renewed every few dozen, 2 initial + at least 2 renewals
}
