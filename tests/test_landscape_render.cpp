// The backend-independent pieces the landscape renderer will sit on (Thema 326, step 2 part A): the one patch grid with
// its quadrant ranges and the morph rule that goes with its triangulation, and the atlas that tells the renderer which
// tile lives in which GPU layer. No GPU here: what a backend would upload and draw is checked as data.
#include "doctest.h"
#include <HorizonScene/LandscapeAtlas.h>
#include <HorizonScene/LandscapeGpuLayout.h>
#include <HorizonScene/LandscapePatchMesh.h>

#include <algorithm>
#include <array>
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

// ── GPU image layouts ────────────────────────────────────────────────────────

TEST_CASE("landscape gpu layout: every sample of a tile has its own texel, in a 386 x 258 image for the default tile")
{
	LandscapeGeometry g;   // 512 m, 257 heights, 128 x 12 weights
	const LandscapeTileImageLayout l = LandscapeTileImageLayout::forGeometry(g);
	CHECK(l.heightW == 257 + 129);
	CHECK(l.heightH == 258);                       // 129 + 65 + 33 + 17 + 9 + 5
	CHECK(l.heightRect.size() == 7);
	CHECK(l.weightRect.size() == 6);
	CHECK(l.weightPages == 3);
	CHECK(l.heightRect[0].x == 0);
	CHECK(l.heightRect[1].x == 257);
	CHECK(l.heightRect[2].y == 129);
	CHECK(l.heightRect[3].y == 129 + 65);
	// nothing overlaps, everything is inside
	const auto disjoint = [](const std::vector<LandscapeImageRect>& rs, int w, int h) {
		std::vector<uint8_t> used(static_cast<size_t>(w) * h, 0);
		for (const auto& r : rs)
		{
			REQUIRE(r.x >= 0);
			REQUIRE(r.y >= 0);
			REQUIRE(r.x + r.w <= w);
			REQUIRE(r.y + r.h <= h);
			for (int y = r.y; y < r.y + r.h; ++y)
				for (int x = r.x; x < r.x + r.w; ++x)
				{
					REQUIRE(used[static_cast<size_t>(y) * w + x] == 0);
					used[static_cast<size_t>(y) * w + x] = 1;
				}
		}
	};
	disjoint(l.heightRect, l.heightW, l.heightH);
	disjoint(l.weightRect, l.weightW, l.weightH);
	// the padding the layout costs
	size_t samples = 0;
	for (const auto& r : l.heightRect) samples += static_cast<size_t>(r.w) * r.h;
	const double waste = 1.0 - static_cast<double>(samples) / (static_cast<double>(l.heightW) * l.heightH);
	CHECK(waste < 0.15);
}

TEST_CASE("landscape gpu layout: a packed tile reads back sample by sample, weights spread over RGBA pages")
{
	ProceduralLandscapeParams p;
	p.tilesX = p.tilesZ = 4;
	const ProceduralLandscapeSource src(p);
	const LandscapeTile t = src.generate({ 2, 1 });
	const LandscapeTileImageLayout l = LandscapeTileImageLayout::forGeometry(t.geometry);
	std::vector<uint16_t> img;
	std::vector<std::vector<uint8_t>> pages;
	landscapePackTile(t, l, img, pages);
	REQUIRE(img.size() == static_cast<size_t>(l.heightW) * l.heightH);
	REQUIRE(pages.size() == 3);
	for (size_t k = 0; k < t.heights.size(); ++k)
	{
		const auto& r = l.heightRect[k];
		for (int z = 0; z < r.h; ++z)
			for (int x = 0; x < r.w; ++x) REQUIRE(img[static_cast<size_t>(r.y + z) * l.heightW + r.x + x] == t.heights[k][static_cast<size_t>(z) * r.w + x]);
	}
	for (size_t k = 0; k < t.weights.size(); ++k)
	{
		const auto& r = l.weightRect[k];
		for (int z = 0; z < r.h; ++z)
			for (int x = 0; x < r.w; ++x)
				for (int c = 0; c < 12; ++c)
					REQUIRE(pages[static_cast<size_t>(c / 4)][(static_cast<size_t>(r.y + z) * l.weightW + r.x + x) * 4 + static_cast<size_t>(c % 4)] ==
					        t.weights[k][(static_cast<size_t>(z) * r.w + x) * 12 + c]);
	}
	// a tile decoded with fewer levels leaves the others zero
	LandscapeTile one = t;
	one.heights.resize(1);
	one.weights.resize(1);
	landscapePackTile(one, l, img, pages);
	const auto& r1 = l.heightRect[1];
	CHECK(img[static_cast<size_t>(r1.y) * l.heightW + r1.x] == 0);
}

TEST_CASE("landscape gpu layout: the coarse world stacked in one image, each grid at its row offset")
{
	ProceduralLandscapeParams p;
	p.tilesX = 3;
	p.tilesZ = 2;
	const ProceduralLandscapeSource src(p);
	const LandscapeCoarseWorld* c = src.coarse();
	REQUIRE(c);
	const LandscapeCoarseImageLayout l = LandscapeCoarseImageLayout::forCoarse(*c);
	REQUIRE(l.grids.size() == c->heights.size());
	CHECK(l.width == c->heights[0].w);               // the widest grid is the first
	CHECK(l.grids[0].rowOffset == 0);
	int rows = 0;
	for (size_t i = 0; i < l.grids.size(); ++i)
	{
		CHECK(l.grids[i].rowOffset == rows);
		CHECK(l.grids[i].w == c->heights[i].w);
		rows += l.grids[i].h;
	}
	CHECK(l.height == rows);
	std::vector<uint16_t> img;
	std::vector<std::vector<uint8_t>> pages;
	landscapePackCoarse(*c, l, img, pages);
	for (size_t i = 0; i < c->heights.size(); ++i)
		for (int z = 0; z < c->heights[i].h; ++z)
			for (int x = 0; x < c->heights[i].w; ++x)
				REQUIRE(img[static_cast<size_t>(l.grids[i].rowOffset + z) * l.width + x] == c->heights[i].v[static_cast<size_t>(z) * c->heights[i].w + x]);
	REQUIRE(pages.size() == 3);
	CHECK(pages[0].size() == static_cast<size_t>(l.weightW) * l.weightH * 4);
	for (int z = 0; z < l.weightH; ++z)
		for (int x = 0; x < l.weightW; ++x)
			for (int ch = 0; ch < 12; ++ch)
				REQUIRE(pages[static_cast<size_t>(ch / 4)][(static_cast<size_t>(z) * l.weightW + x) * 4 + static_cast<size_t>(ch % 4)] ==
				        c->weights[(static_cast<size_t>(z) * c->weightsW + x) * 12 + ch]);
}

// The vertex shader's height lookup (LandscapeShaderSource.h, heightOfTile) done on the CPU against the packed images:
// the contract between the packer, the slot table and the shader, checked sample by sample.
TEST_CASE("landscape gpu layout: the shader's tile lookup, replayed on the CPU, finds every lattice sample, tile borders and the world's edge included")
{
	ProceduralLandscapeParams p;
	p.tilesX = 4;
	p.tilesZ = 3;
	const ProceduralLandscapeSource src(p);
	const LandscapeTileImageLayout lay = LandscapeTileImageLayout::forGeometry(p.geometry);
	const int window = 8, mask = window - 1;

	// what the renderer would hold: a layer per slot, the slot table, tiles in some order
	std::vector<std::vector<uint16_t>> layers;
	std::vector<uint16_t> table(static_cast<size_t>(window) * window, 0xFFFF);
	std::vector<LandscapeTile> tiles;
	for (int tz = 0; tz < 3; ++tz)
		for (int tx = 0; tx < 4; ++tx)
		{
			tiles.push_back(src.generate({ tx, tz }));
			std::vector<uint16_t> img;
			std::vector<std::vector<uint8_t>> pages;
			landscapePackTile(tiles.back(), lay, img, pages);
			table[static_cast<size_t>(tz & mask) * window + (tx & mask)] = static_cast<uint16_t>(layers.size());
			layers.push_back(std::move(img));
		}
	const auto tileAt = [&](int tx, int tz) -> const LandscapeTile& { return tiles[static_cast<size_t>(tz) * 4 + tx]; };

	const LandscapeCoarseWorld* coarse = src.coarse();
	REQUIRE(coarse);
	// heightOfTile(g, level), line by line (LandscapeShaderSource.h): the tile it lies in, the neighbours that share a border point,
	// then the coarse world's finest grid
	const auto shaderHeight = [&](int gx, int gz, int level) -> int {
		const int cpt = p.geometry.hCells >> level;
		const int tx = std::min(gx / cpt, 4 - 1), tz = std::min(gz / cpt, 3 - 1);
		const int lx = std::min(gx - tx * cpt, cpt), lz = std::min(gz - tz * cpt, cpt);
		for (int k = 0; k < 4; ++k)
		{
			const int sx = k & 1, sz = k >> 1;
			if ((sx && lx != 0) || (sz && lz != 0)) continue;
			const int ttx = tx - sx, ttz = tz - sz;
			if (ttx < 0 || ttz < 0) continue;
			const uint16_t slot = table[static_cast<size_t>(ttz & mask) * window + (ttx & mask)];
			if (slot == 0xFFFF) continue;
			const LandscapeImageRect& r = lay.heightRect[static_cast<size_t>(level)];
			return layers[slot][static_cast<size_t>(r.y + lz + sz * cpt) * lay.heightW + r.x + lx + sx * cpt];
		}
		return 100000 + coarse->heights[0].at(gx >> (3 + 1 - level), gz >> (3 + 1 - level));   // marks a fall back to the coarse world
	};

	std::mt19937 rng(5);
	for (int level = 0; level <= 3; ++level)
	{
		const int cpt = p.geometry.hCells >> level;
		const int maxGx = 4 * cpt, maxGz = 3 * cpt;       // the world's last lattice column / row
		for (int i = 0; i < 4000; ++i)
		{
			const int gx = static_cast<int>(rng() % (maxGx + 1)), gz = static_cast<int>(rng() % (maxGz + 1));
			const int tx = std::min(gx / cpt, 3), tz = std::min(gz / cpt, 2);
			const int dim = tileAt(tx, tz).geometry.heightDim(level);
			const int expect = tileAt(tx, tz).heights[static_cast<size_t>(level)][static_cast<size_t>(std::min(gz - tz * cpt, cpt)) * dim + std::min(gx - tx * cpt, cpt)];
			REQUIRE(shaderHeight(gx, gz, level) == expect);
		}
		// on a tile border the right/lower tile's first sample IS the left/upper tile's last sample: the lookup is continuous
		for (int tz = 0; tz < 3; ++tz)
			for (int tx = 1; tx < 4; ++tx)
				for (int k = 0; k <= cpt; ++k)
				{
					const LandscapeTile& left = tileAt(tx - 1, tz);
					const int dim = left.geometry.heightDim(level);
					REQUIRE(shaderHeight(tx * cpt, tz * cpt + k, level) ==
					        left.heights[static_cast<size_t>(level)][static_cast<size_t>(std::min(k, cpt)) * dim + cpt]);
				}
	}
	// The edge of the loaded disc: the tile to the right is missing, the border point is still the left tile's last sample, not the floor.
	const uint16_t keep = table[static_cast<size_t>(1 & mask) * window + (2 & mask)];
	table[static_cast<size_t>(1 & mask) * window + (2 & mask)] = 0xFFFF;                  // tile (2, 1) is not in the atlas
	for (int level = 0; level <= 3; ++level)
	{
		const int cpt = p.geometry.hCells >> level;
		const LandscapeTile& left = tileAt(1, 1);
		const int dim = left.geometry.heightDim(level);
		for (int k = 0; k <= cpt; ++k)
		{
			// (2 * cpt, cpt + k) lies on the border between tiles (1, 1) and (2, 1): the missing right one does not hide it
			REQUIRE(shaderHeight(2 * cpt, cpt + k, level) == left.heights[static_cast<size_t>(level)][static_cast<size_t>(k) * dim + cpt]);
		}
		// and so does the corner shared with the tile above: (2 * cpt, cpt) belongs to (1,0), (2,0), (1,1) and (2,1)
		const LandscapeTile& up = tileAt(1, 0);
		REQUIRE(shaderHeight(2 * cpt, cpt, level) == up.heights[static_cast<size_t>(level)][static_cast<size_t>(cpt) * dim + cpt]);
		// a point strictly inside the missing tile has no tile at all: the coarse world answers (marked by the +100000)
		REQUIRE(shaderHeight(2 * cpt + 3, cpt + 3, level) >= 100000);
		REQUIRE(shaderHeight(2 * cpt + 3, cpt + 3, level) - 100000 == coarse->heights[0].at((2 * cpt + 3) >> (4 - level), (cpt + 3) >> (4 - level)));
	}
	table[static_cast<size_t>(1 & mask) * window + (2 & mask)] = keep;
}
