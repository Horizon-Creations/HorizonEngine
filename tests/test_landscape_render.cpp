// The backend-independent pieces the landscape renderer will sit on (Thema 326, step 2 part A): the one patch grid with
// its quadrant ranges and the morph rule that goes with its triangulation, and the atlas that tells the renderer which
// tile lives in which GPU layer. No GPU here: what a backend would upload and draw is checked as data.
#include "doctest.h"
#include <HorizonScene/LandscapeAtlas.h>
#include <HorizonScene/LandscapePatchMesh.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <random>
#include <set>
#include <thread>

using namespace HE;

namespace
{
LandscapeGeometry smallGeometry()
{
	LandscapeGeometry g;
	g.hCells = 32;
	g.wTexels = 16;
	g.channels = 4;
	return g;
}
} // namespace

TEST_CASE("landscape patch mesh: a watertight 64 x 64 grid, four quadrant ranges, one winding")
{
	const LandscapePatchMesh m = LandscapePatchMesh::build(64);
	CHECK(m.vertexCount() == 65 * 65);
	REQUIRE(m.indices.size() == 64u * 64u * 6u);
	CHECK(m.indices.size() % 3 == 0);
	for (uint16_t i : m.indices) REQUIRE(i < m.vertexCount());

	// quadrant ranges follow each other, cover everything, each triangle lies inside its quadrant
	uint32_t next = 0;
	for (int q = 0; q < 4; ++q)
	{
		CHECK(m.quadrantFirst[q] == next);
		CHECK(m.quadrantCount[q] == 32u * 32u * 6u);
		next += m.quadrantCount[q];
		const int i0 = (q & 1) * 32, j0 = (q >> 1) * 32;
		for (uint32_t k = m.quadrantFirst[q]; k < m.quadrantFirst[q] + m.quadrantCount[q]; ++k)
		{
			const int i = m.indices[k] % 65, j = m.indices[k] / 65;
			REQUIRE(i >= i0);
			REQUIRE(i <= i0 + 32);
			REQUIRE(j >= j0);
			REQUIRE(j <= j0 + 32);
		}
	}
	CHECK(next == m.indices.size());

	// every undirected edge is in two triangles, except the 4 * 64 on the border; same winding everywhere (+y normal)
	std::map<std::pair<int, int>, int> edges;
	std::set<std::array<int, 3>> triangles;
	for (size_t t = 0; t < m.indices.size(); t += 3)
	{
		const int v[3] = { m.indices[t], m.indices[t + 1], m.indices[t + 2] };
		std::array<int, 3> sorted = { v[0], v[1], v[2] };
		std::sort(sorted.begin(), sorted.end());
		REQUIRE(triangles.insert(sorted).second);                  // no triangle twice
		for (int e = 0; e < 3; ++e)
		{
			const int a = v[e], b = v[(e + 1) % 3];
			++edges[{ std::min(a, b), std::max(a, b) }];
		}
		// normal.y of (v1 - v0) x (v2 - v0) with x = i, z = j
		const double ax = v[1] % 65 - v[0] % 65, az = v[1] / 65 - v[0] / 65, bx = v[2] % 65 - v[0] % 65, bz = v[2] / 65 - v[0] / 65;
		REQUIRE(az * bx - ax * bz > 0.0);
	}
	int border = 0;
	for (const auto& [e, n] : edges)
	{
		const bool onBorder = [&] {
			const int ai = e.first % 65, aj = e.first / 65, bi = e.second % 65, bj = e.second / 65;
			return (ai == 0 && bi == 0) || (ai == 64 && bi == 64) || (aj == 0 && bj == 0) || (aj == 64 && bj == 64);
		}();
		if (onBorder) { REQUIRE(n == 1); ++border; }
		else REQUIRE(n == 2);
	}
	CHECK(border == 4 * 64);

	CHECK(LandscapePatchMesh::build(63).indices.empty());      // odd: no grid
	CHECK(LandscapePatchMesh::build(256).indices.empty());     // too big for the byte positions
}

TEST_CASE("landscape patch mesh: fully morphed, every fine triangle lies in the plane of one coarse triangle")
{
	const int N = 64;
	const LandscapePatchMesh m = LandscapePatchMesh::build(N);
	std::mt19937 rng(17);
	std::uniform_real_distribution<double> rnd(0.0, 1000.0);
	std::vector<double> h((N + 1) * (N + 1));
	for (double& v : h) v = rnd(rng);
	const auto H = [&](int i, int j) { return h[static_cast<size_t>(j) * (N + 1) + i]; };
	const auto morphed = [&](int i, int j) {
		const LandscapeMorphSource s = landscapeMorphSource(i, j);
		if (s.n == 0) return H(i, j);
		REQUIRE(s.aI >= 0);
		REQUIRE(s.bI <= N);
		REQUIRE(s.aJ >= 0);
		REQUIRE(s.bJ <= N);
		return 0.5 * (H(s.aI, s.aJ) + H(s.bI, s.bJ));
	};
	for (size_t t = 0; t < m.indices.size(); t += 3)
	{
		// the coarse cell of the triangle's centroid, and which half of it ("\" diagonal: below it when x > z locally)
		double cx = 0.0, cz = 0.0;
		for (int k = 0; k < 3; ++k) { cx += m.indices[t + k] % (N + 1); cz += m.indices[t + k] / (N + 1); }
		cx /= 3.0;
		cz /= 3.0;
		const int ci = static_cast<int>(std::floor(cx / 2.0)) * 2, cj = static_cast<int>(std::floor(cz / 2.0)) * 2;
		const bool lower = (cx - ci) > (cz - cj);        // T1 = (0,0), (2,0), (2,2); else T2 = (0,0), (0,2), (2,2)
		const double h00 = H(ci, cj), h22 = H(ci + 2, cj + 2), hOther = lower ? H(ci + 2, cj) : H(ci, cj + 2);
		for (int k = 0; k < 3; ++k)
		{
			const int i = m.indices[t + k] % (N + 1), j = m.indices[t + k] / (N + 1);
			const double u = (i - ci) / 2.0, v = (j - cj) / 2.0;
			// plane through the coarse triangle: lower: h00 + u * (hOther - h00) + v * (h22 - hOther); upper: h00 + v * (hOther - h00) + u * (h22 - hOther)
			const double plane = lower ? h00 + u * (hOther - h00) + v * (h22 - hOther) : h00 + v * (hOther - h00) + u * (h22 - hOther);
			REQUIRE(morphed(i, j) == doctest::Approx(plane).epsilon(1e-12));
		}
	}
	// vertices with no odd index are untouched; the parity classes
	CHECK(landscapeMorphSource(4, 6).n == 0);
	CHECK(landscapeMorphSource(5, 6).aI == 4);
	CHECK(landscapeMorphSource(4, 7).bJ == 8);
	CHECK(landscapeMorphSource(5, 7).aI == 4);
	CHECK(landscapeMorphSource(5, 7).bJ == 8);
}

TEST_CASE("landscape atlas: slots follow the store, the lookup table follows the slots")
{
	ProceduralLandscapeParams p;
	p.geometry = smallGeometry();
	p.tilesX = p.tilesZ = 60;
	auto src = std::make_shared<ProceduralLandscapeSource>(p);
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 2000.0f;
	LandscapeStore store(src, cfg);
	LandscapeAtlas atlas(store.stats().maxTiles, 16);

	std::set<std::pair<int, int>> onGpu;                 // what the renderer would hold
	std::vector<uint16_t> shadowTable(static_cast<size_t>(atlas.window()) * atlas.window(), LandscapeAtlas::kNoSlot);
	std::vector<LandscapeAtlas::Upload> up;
	std::vector<LandscapeAtlas::Release> rel;
	std::vector<LandscapeAtlas::TableUpdate> tab;
	size_t totalUploads = 0;

	const auto check = [&] {
		for (const auto& u : tab) shadowTable[u.index] = u.slot;
		REQUIRE(shadowTable == atlas.table());
		std::set<uint16_t> slots;
		for (const LandscapeTileKey& k : store.residentKeys())
		{
			if (!atlas.contains(k)) continue;
			const uint16_t s = atlas.slotOf(k);
			REQUIRE(s < atlas.capacity());
			REQUIRE(slots.insert(s).second);              // no slot twice
			REQUIRE(atlas.lookup(k.x, k.z) == s);
		}
		REQUIRE(atlas.stats().used <= atlas.capacity());
	};

	for (double d = 3000.0; d <= 27000.0; d += 120.0)
	{
		store.update({ { glm::dvec3(d, 0.0, d * 0.8) } }, 1.0);
		atlas.sync(store, up, rel, tab, 3);
		REQUIRE(up.size() <= 3);
		for (const auto& r : rel) REQUIRE(onGpu.erase({ r.key.x, r.key.z }) == 1);
		for (const auto& u : up)
		{
			REQUIRE(onGpu.insert({ u.key.x, u.key.z }).second);        // a tile is uploaded once until it was released
			REQUIRE(u.tile);
			REQUIRE(*u.tile == *store.tile(u.key));
			++totalUploads;
		}
		check();
		if (static_cast<int>((d - 3000.0) / 120.0) % 6 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	// let the store settle, then the atlas catches up at three tiles a frame
	const std::vector<LandscapeAnchor> last = { { glm::dvec3(27000.0, 0.0, 21600.0) } };
	for (int i = 0; i < 4000 && !store.settled(); ++i) { store.update(last, 5.0); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
	REQUIRE(store.settled());
	for (int i = 0; i < 400; ++i)
	{
		atlas.sync(store, up, rel, tab, 3);
		for (const auto& r : rel) REQUIRE(onGpu.erase({ r.key.x, r.key.z }) == 1);
		for (const auto& u : up) REQUIRE(onGpu.insert({ u.key.x, u.key.z }).second);
		check();
		if (up.empty() && rel.empty()) break;
	}
	for (const LandscapeTileKey& k : store.residentKeys()) REQUIRE(atlas.contains(k));
	CHECK(atlas.stats().used == store.residentKeys().size());
	CHECK(onGpu.size() == store.residentKeys().size());
	CHECK(atlas.stats().aliased == 0);
	CHECK(atlas.stats().full == 0);
	CHECK(totalUploads > 100);
	MESSAGE("atlas: " << totalUploads << " uploads, " << atlas.stats().deferred << " tile-syncs deferred by the three-a-frame cap");   // timing dependent: not asserted
	// a tile that is not there has no slot
	CHECK(atlas.lookup(59, 59) == LandscapeAtlas::kNoSlot);
}

TEST_CASE("landscape atlas: a window too small for the loaded disc is counted, never mixes two tiles up")
{
	ProceduralLandscapeParams p;
	p.geometry = smallGeometry();
	p.tilesX = p.tilesZ = 60;
	auto src = std::make_shared<ProceduralLandscapeSource>(p);
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 2000.0f;
	LandscapeStore store(src, cfg);
	LandscapeAtlas atlas(store.stats().maxTiles, 4);        // 4 x 4 entries for a disc about 9 tiles wide
	const std::vector<LandscapeAnchor> at = { { glm::dvec3(15000.0, 0.0, 15000.0) } };
	for (int i = 0; i < 4000 && !store.settled(); ++i) { store.update(at, 5.0); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
	REQUIRE(store.settled());
	std::vector<LandscapeAtlas::Upload> up;
	std::vector<LandscapeAtlas::Release> rel;
	std::vector<LandscapeAtlas::TableUpdate> tab;
	atlas.sync(store, up, rel, tab, 1000);
	CHECK(atlas.stats().aliased > 0);
	CHECK(atlas.stats().used <= 16);                         // one tile per table entry at most
	for (const LandscapeTileKey& k : store.residentKeys())
		if (atlas.contains(k)) REQUIRE(atlas.lookup(k.x, k.z) == atlas.slotOf(k));
}
