#include "doctest.h"
#include <HorizonRendering/GiBlasStore.h>

#include <glm/glm.hpp>
#include <cstring>
#include <vector>

// The per-mesh BLAS cache of the compute-GI backends (GL, D3D11, D3D12, Vulkan).
// Thema 221: a single rebuilt mesh used to drop the whole cache, and the terrain
// seam stitching rebuilds chunk copies on every LOD change, so a camera flight
// rebuilt every BVH of the landscape each frame (120 → 20 FPS). These pin down
// that one mesh's rebuild leaves the others alone, that uploads stay
// tail-only between rewrites, and that a compaction MOVES BVHs intact.

namespace
{
HE::UUID idOf(uint64_t n) { return HE::UUID{ 0x221, n }; }

// A flat n×n-quad grid at height y, x/z in [x0, x0 + n] × [0, n].
HE::GiBvh gridBvh(int n, float x0, float y)
{
	std::vector<float>    pos;
	std::vector<uint32_t> idx;
	for (int z = 0; z <= n; ++z)
		for (int x = 0; x <= n; ++x)
			pos.insert(pos.end(), { x0 + static_cast<float>(x), y, static_cast<float>(z) });
	const uint32_t row = static_cast<uint32_t>(n + 1);
	for (uint32_t z = 0; z < static_cast<uint32_t>(n); ++z)
		for (uint32_t x = 0; x < static_cast<uint32_t>(n); ++x)
		{
			const uint32_t a = z * row + x, b = a + 1, c = a + row, d = c + 1;
			idx.insert(idx.end(), { a, b, c, b, d, c });
		}
	return HE::buildGiBvh(pos.data(), pos.size() / 3, 3, idx.data(), idx.size());
}

// The BLAS `r` points at, cut back out of the concatenated arrays.
HE::GiBvh sliceOf(const HE::GiBlasStore& s, const HE::GiBlasRange& r)
{
	HE::GiBvh b;
	b.nodes.assign(s.nodes().begin() + r.nodeOffset, s.nodes().begin() + r.nodeOffset + r.nodeCount);
	b.triangles.assign(s.tris().begin() + r.triOffset, s.tris().begin() + r.triOffset + r.triCount);
	return b;
}

bool sameBvh(const HE::GiBvh& a, const HE::GiBvh& b)
{
	return a.nodes.size() == b.nodes.size() && a.triangles.size() == b.triangles.size()
	    && std::memcmp(a.nodes.data(), b.nodes.data(), a.nodes.size() * sizeof(HE::GiBvhNode)) == 0
	    && std::memcmp(a.triangles.data(), b.triangles.data(),
	                   a.triangles.size() * sizeof(HE::GiBvhTriangle)) == 0;
}
} // namespace

TEST_CASE("GiBlasStore: erasing one mesh leaves every other BLAS where it was")
{
	HE::GiBlasStore s;
	const HE::GiBvh a = gridBvh(4, 0.0f, 0.0f), b = gridBvh(8, 10.0f, 1.0f), c = gridBvh(2, 30.0f, 2.0f);
	const HE::GiBlasRange ra = s.add(idOf(1), a);
	const HE::GiBlasRange rb = s.add(idOf(2), b);
	const HE::GiBlasRange rc = s.add(idOf(3), c);
	REQUIRE(ra.valid); REQUIRE(rb.valid); REQUIRE(rc.valid);
	CHECK(rb.nodeOffset == ra.nodeCount);
	CHECK(rc.triOffset  == ra.triCount + rb.triCount);

	CHECK(s.erase(idOf(2)));
	CHECK_FALSE(s.erase(idOf(2)));        // already gone
	CHECK(s.find(idOf(2)) == nullptr);
	CHECK(s.entries() == 2);
	CHECK(s.deadTris()  == b.triangles.size());
	CHECK(s.deadNodes() == b.nodes.size());

	// The survivors: same offsets, same bytes — no rebuild, nothing moved.
	REQUIRE(s.find(idOf(1)) != nullptr);
	REQUIRE(s.find(idOf(3)) != nullptr);
	CHECK(s.find(idOf(3))->triOffset == rc.triOffset);
	CHECK(sameBvh(sliceOf(s, *s.find(idOf(1))), a));
	CHECK(sameBvh(sliceOf(s, *s.find(idOf(3))), c));
}

TEST_CASE("GiBlasStore: uploads only the tail between rewrites")
{
	HE::GiBlasStore s;
	CHECK_FALSE(s.dirty());
	s.add(idOf(1), gridBvh(4, 0.0f, 0.0f));
	CHECK(s.dirty());
	CHECK(s.uploadFromTris() == 0);
	s.markUploaded();
	CHECK_FALSE(s.dirty());

	const size_t trisBefore = s.tris().size(), nodesBefore = s.nodes().size();
	s.erase(idOf(1));                     // dead space alone changes nothing on the GPU
	CHECK_FALSE(s.dirty());
	s.add(idOf(1), gridBvh(6, 0.0f, 0.0f)); // the rebuilt mesh: appended
	CHECK(s.dirty());
	CHECK(s.uploadFromTris()  == trisBefore);
	CHECK(s.uploadFromNodes() == nodesBefore);
	CHECK(s.find(idOf(1))->triOffset == static_cast<int32_t>(trisBefore));

	// add() over a cached id replaces it the same way (old range turns dead).
	s.markUploaded();
	const size_t dead = s.deadTris();
	s.add(idOf(1), gridBvh(2, 0.0f, 0.0f));
	CHECK(s.deadTris() == dead + 72);     // the 6×6 grid's 72 triangles
	CHECK(s.entries() == 1);
}

TEST_CASE("GiBlasStore: a mesh without geometry is cached as invalid, not retried")
{
	HE::GiBlasStore s;
	const HE::GiBlasRange r = s.add(idOf(7), HE::GiBvh{});
	CHECK_FALSE(r.valid);
	REQUIRE(s.find(idOf(7)) != nullptr);
	CHECK_FALSE(s.find(idOf(7))->valid);
	CHECK(s.erase(idOf(7)));
	CHECK(s.deadTris() == 0);
}

TEST_CASE("GiBlasStore: compaction moves the live BLASes intact once dead space outweighs them")
{
	HE::GiBlasStore s;
	// 128×128 quads = 32768 triangles per big grid: three of them rebuilt away
	// is well past the 64 Ki-triangle floor and past the live data.
	const HE::GiBvh small = gridBvh(4, 0.0f, 0.0f);
	const HE::GiBvh keep  = gridBvh(16, 200.0f, 5.0f);
	s.add(idOf(1), gridBvh(128, 0.0f, 1.0f));
	s.add(idOf(2), small);
	s.add(idOf(3), gridBvh(128, 0.0f, 2.0f));
	s.add(idOf(4), gridBvh(128, 0.0f, 3.0f));
	s.add(idOf(5), keep);
	s.markUploaded();

	s.erase(idOf(1));
	CHECK_FALSE(s.compactIfWasteful());   // 32 Ki dead: under the floor
	s.erase(idOf(3));
	s.erase(idOf(4));
	CHECK(s.deadTris() == 3u * 32768u);
	REQUIRE(s.compactIfWasteful());

	CHECK(s.deadTris() == 0);
	CHECK(s.tris().size()  == small.triangles.size() + keep.triangles.size());
	CHECK(s.nodes().size() == small.nodes.size() + keep.nodes.size());
	// Every offset moved: the next upload is a full rewrite.
	CHECK(s.dirty());
	CHECK(s.uploadFromTris() == 0);
	CHECK(s.uploadFromNodes() == 0);

	// Same order, packed from 0, byte-identical BVHs.
	const HE::GiBlasRange* r2 = s.find(idOf(2));
	const HE::GiBlasRange* r5 = s.find(idOf(5));
	REQUIRE(r2 != nullptr); REQUIRE(r5 != nullptr);
	CHECK(r2->nodeOffset == 0);
	CHECK(r2->triOffset  == 0);
	CHECK(r5->nodeOffset == r2->nodeCount);
	CHECK(r5->triOffset  == r2->triCount);
	CHECK(sameBvh(sliceOf(s, *r2), small));
	CHECK(sameBvh(sliceOf(s, *r5), keep));

	// And the moved BVH still traverses: its indices are offset-relative.
	float t = 0.0f; uint32_t tri = 0;
	CHECK(HE::giBvhIntersect(sliceOf(s, *r5), glm::vec3(205.5f, 10.0f, 3.5f), glm::vec3(0, -1, 0),
	                         0.001f, 100.0f, false, t, tri));
	CHECK(t == doctest::Approx(5.0f));

	s.markUploaded();
	CHECK_FALSE(s.dirty());
	CHECK_FALSE(s.compactIfWasteful());   // nothing dead any more
}

TEST_CASE("GiBlasStore: clear() forces a full rewrite")
{
	HE::GiBlasStore s;
	s.add(idOf(1), gridBvh(4, 0.0f, 0.0f));
	s.markUploaded();
	s.clear();
	CHECK(s.entries() == 0);
	CHECK(s.dirty());
	CHECK(s.uploadFromTris() == 0);
	s.add(idOf(1), gridBvh(4, 0.0f, 0.0f));
	CHECK(s.find(idOf(1))->triOffset == 0);
}
