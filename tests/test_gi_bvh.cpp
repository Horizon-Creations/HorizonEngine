#include "doctest.h"
#include <HorizonRendering/GiBvh.h>

#include <glm/glm.hpp>
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
