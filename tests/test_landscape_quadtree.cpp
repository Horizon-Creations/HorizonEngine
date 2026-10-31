// HE::selectLandscapePatches (Thema 326, step 2 part A): the quadtree that decides which patches of a 100 km
// landscape are drawn, at which level. Pure CPU: coverage without holes or overlaps, neighbouring levels at most one
// apart, no crack once the vertex shader's morph is applied (recomputed here on the CPU), tiles that are not in memory.
#include "doctest.h"
#include <HorizonScene/LandscapeQuadtree.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using namespace HE;

namespace
{
// A quadrant (or the whole node) as drawn: a rectangle of the world with the level it is drawn at.
struct Rect
{
	double x0, z0, size;      // the rectangle
	int    level;
	double nodeX0, nodeZ0;    // the node the grid belongs to
	double cell;
};

std::vector<Rect> rectsOf(const LandscapeQuadtreeConfig& cfg, const std::vector<LandscapePatch>& patches)
{
	std::vector<Rect> out;
	for (const LandscapePatch& p : patches)
		for (int q = 0; q < 4; ++q)
			if (p.quadrants & (1 << q))
				out.push_back({ p.x0 + (q & 1) * p.size * 0.5, p.z0 + (q >> 1) * p.size * 0.5, p.size * 0.5, p.level, p.x0, p.z0,
				                static_cast<double>(cfg.cellSize(p.level)) });
	return out;
}

int coverCount(const std::vector<Rect>& rects, double x, double z)
{
	int n = 0;
	for (const Rect& r : rects)
		if (x >= r.x0 && x < r.x0 + r.size && z >= r.z0 && z < r.z0 + r.size) ++n;
	return n;
}

// The synthetic ground: smooth hills with detail at every scale the grids resolve.
double ground(double x, double z)
{
	return 1200.0 + 700.0 * std::sin(x / 1300.0) * std::cos(z / 900.0) + 90.0 * std::sin(x / 37.0 + z / 53.0) + 12.0 * std::sin(x / 5.3) * std::sin(z / 4.1);
}

LandscapeQuadtreeConfig defaultConfig()
{
	return LandscapeQuadtreeConfig{};
}

double worldMeters(const LandscapeQuadtreeConfig& cfg) { return cfg.tilesX * static_cast<double>(cfg.tileM); }
} // namespace

TEST_CASE("landscape quadtree: the default ranges and the morph")
{
	const LandscapeQuadtreeConfig cfg = defaultConfig();
	CHECK(cfg.patchSize(0) == doctest::Approx(128.0f));
	CHECK(cfg.patchSize(3) == doctest::Approx(1024.0f));
	CHECK(cfg.cellSize(4) == doctest::Approx(32.0f));
	CHECK(cfg.topLevel() == 10);                       // 131 km covers the 100.35 km world
	CHECK(cfg.rangeFactor * cfg.morphStart >= 1.4143f); // what the seams need (see the header)
	for (int l = 0; l < 8; ++l)
	{
		CHECK(landscapeMorph(cfg, l, cfg.rangeOf(l - 1)) == doctest::Approx(0.0f));
		CHECK(landscapeMorph(cfg, l, cfg.rangeOf(l)) == doctest::Approx(1.0f));
		CHECK(landscapeMorph(cfg, l, cfg.rangeOf(l) * 3.0f) == doctest::Approx(1.0f));
		float prev = 0.0f;
		for (float d = 0.0f; d <= cfg.rangeOf(l) * 1.2f; d += cfg.rangeOf(l) / 50.0f)
		{
			const float m = landscapeMorph(cfg, l, d);
			CHECK(m >= prev);
			prev = m;
		}
	}
}

TEST_CASE("landscape quadtree: without culling every point of the world is covered exactly once")
{
	const LandscapeQuadtreeConfig cfg = defaultConfig();
	std::mt19937 rng(11);
	std::uniform_real_distribution<double> world(0.0, worldMeters(cfg) - 1.0), height(50.0, 2500.0);
	for (int round = 0; round < 12; ++round)
	{
		const glm::dvec3 cam(world(rng), height(rng), world(rng));
		const auto patches = selectLandscapePatches(cfg, cam);
		const auto rects = rectsOf(cfg, patches);
		REQUIRE_FALSE(patches.empty());
		for (int i = 0; i < 1500; ++i)
		{
			const double x = world(rng), z = world(rng);
			INFO("camera " << cam.x << " " << cam.z << " point " << x << " " << z);
			REQUIRE(coverCount(rects, x, z) == 1);
		}
		// also right under the camera and in the world's corners
		for (const auto& [x, z] : { std::pair{ cam.x, cam.z }, std::pair{ 0.5, 0.5 }, std::pair{ worldMeters(cfg) - 0.5, worldMeters(cfg) - 0.5 } })
			REQUIRE(coverCount(rects, x, z) == 1);
	}
}

TEST_CASE("landscape quadtree: a patch is drawn only inside its level's distance band")
{
	const LandscapeQuadtreeConfig cfg = defaultConfig();
	const glm::dvec3 cam(50000.0, 800.0, 50000.0);
	const auto patches = selectLandscapePatches(cfg, cam);
	int maxLevel = 0;
	for (const LandscapePatch& p : patches)
	{
		const double dx = std::max({ p.x0 - cam.x, 0.0, cam.x - (p.x0 + p.size) });
		const double dz = std::max({ p.z0 - cam.z, 0.0, cam.z - (p.z0 + p.size) });
		const double dy = std::max({ cfg.minY - cam.y, 0.0, cam.y - cfg.maxY });
		const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
		CHECK(d <= cfg.rangeOf(p.level) + 1e-6);
		if (p.level > 0 && p.quadrants == 15) CHECK(d > cfg.rangeOf(p.level - 1) - 1e-6);   // not split, so no finer level was due
		maxLevel = std::max(maxLevel, p.level);
	}
	CHECK(maxLevel >= 6);                                   // the far edge of the world is drawn coarse (level 7 from the middle)
	// the level grows with the distance: the nearest patch is the finest
	int nearestLevel = -1;
	double nearest = 1e18;
	for (const LandscapePatch& p : patches)
	{
		const double d = std::hypot(p.x0 + p.size * 0.5 - cam.x, p.z0 + p.size * 0.5 - cam.z);
		if (d < nearest) { nearest = d; nearestLevel = p.level; }
	}
	CHECK(nearestLevel == 0);
}

TEST_CASE("landscape quadtree: neighbouring rectangles differ by at most one level")
{
	const LandscapeQuadtreeConfig cfg = defaultConfig();
	std::mt19937 rng(5);
	std::uniform_real_distribution<double> world(0.0, worldMeters(cfg) - 1.0), height(50.0, 2500.0);
	for (int round = 0; round < 6; ++round)
	{
		const glm::dvec3 cam(world(rng), height(rng), world(rng));
		const auto rects = rectsOf(cfg, selectLandscapePatches(cfg, cam));
		int checked = 0;
		for (size_t i = 0; i < rects.size(); ++i)
			for (size_t j = i + 1; j < rects.size(); ++j)
			{
				const Rect &a = rects[i], &b = rects[j];
				const double eps = 1e-6;
				const bool touchX = std::abs(a.x0 + a.size - b.x0) < eps || std::abs(b.x0 + b.size - a.x0) < eps;
				const bool touchZ = std::abs(a.z0 + a.size - b.z0) < eps || std::abs(b.z0 + b.size - a.z0) < eps;
				const bool overlapX = std::min(a.x0 + a.size, b.x0 + b.size) - std::max(a.x0, b.x0) > eps;
				const bool overlapZ = std::min(a.z0 + a.size, b.z0 + b.size) - std::max(a.z0, b.z0) > eps;
				if ((touchX && overlapZ) || (touchZ && overlapX))
				{
					REQUIRE(std::abs(a.level - b.level) <= 1);
					++checked;
				}
			}
		CHECK(checked > 100);
	}
}

TEST_CASE("landscape quadtree: with the morph applied, no crack opens between neighbours of different levels")
{
	const LandscapeQuadtreeConfig cfg = defaultConfig();
	std::mt19937 rng(23);
	std::uniform_real_distribution<double> world(5000.0, worldMeters(cfg) - 5000.0), height(100.0, 5000.0);   // up to well above the slab
	size_t compared = 0;
	for (int round = 0; round < 4; ++round)
	{
		const glm::dvec3 cam(world(rng), height(rng), world(rng));
		const auto rects = rectsOf(cfg, selectLandscapePatches(cfg, cam));

		// Morphed height of the vertex at (x, z) of the grid that starts at (nodeX0, nodeZ0), cell c, level L; `alongX`
		// says the edge runs along x (so the vertex index along it is the x index).
		const auto morphed = [&](const Rect& r, double x, double z, bool alongX) {
			const double own = ground(x, z);
			const double along = alongX ? (x - r.nodeX0) : (z - r.nodeZ0);
			const long k = std::lround(along / r.cell);
			if (k % 2 == 0) return own;
			const double dir = alongX ? 1.0 : 0.0;
			const double coarse = 0.5 * (ground(x - dir * r.cell, z - (1.0 - dir) * r.cell) + ground(x + dir * r.cell, z + (1.0 - dir) * r.cell));
			const double m = landscapeMorph(cfg, r.level, landscapeMorphDistance(cfg, cam, x, z));
			return own + (coarse - own) * m;
		};

		for (size_t i = 0; i < rects.size(); ++i)
			for (size_t j = 0; j < rects.size(); ++j)
			{
				const Rect &a = rects[i], &b = rects[j];   // a: the finer one, b one level coarser
				if (b.level != a.level + 1) continue;
				const double eps = 1e-6;
				// shared vertical edge (x = const) or horizontal edge (z = const)
				for (int axis = 0; axis < 2; ++axis)
				{
					const bool alongX = axis == 1;   // axis 0: edge at constant x, running along z
					const double aEdge = alongX ? a.z0 : a.x0, aEdgeFar = aEdge + a.size, bEdge = alongX ? b.z0 : b.x0, bEdgeFar = bEdge + b.size;
					const double aLo = alongX ? a.x0 : a.z0, bLo = alongX ? b.x0 : b.z0;
					const double lo = std::max(aLo, bLo), hi = std::min(aLo + a.size, bLo + b.size);
					if (hi - lo <= eps) continue;
					for (double edge : { aEdge, aEdgeFar })
					{
						if (std::abs(edge - bEdge) > eps && std::abs(edge - bEdgeFar) > eps) continue;
						// the edge lies on both rectangles: compare the finer side's vertices with the coarser side's line
						const double cA = a.cell, cB = b.cell;
						const double origin = alongX ? a.nodeX0 : a.nodeZ0;
						for (double t = origin + std::ceil((lo - origin) / cA - 1e-9) * cA; t <= hi + eps; t += cA)
						{
							const double x = alongX ? t : edge, z = alongX ? edge : t;
							const double hA = morphed(a, x, z, alongX);
							// the coarser side's two vertices around t
							const double base = (alongX ? b.nodeX0 : b.nodeZ0);
							const double q1 = base + std::floor((t - base) / cB + 1e-9) * cB, q2 = q1 + cB;
							const double h1 = morphed(b, alongX ? q1 : edge, alongX ? edge : q1, alongX);
							const double h2 = morphed(b, alongX ? q2 : edge, alongX ? edge : q2, alongX);
							const double u = (t - q1) / cB;
							const auto morphOf = [&](const Rect& r, double xx, double zz) {
								return landscapeMorph(cfg, r.level, landscapeMorphDistance(cfg, cam, xx, zz));
							};
							INFO("A level " << a.level << " vertex m=" << morphOf(a, x, z) << " B level " << b.level << " m1=" << morphOf(b, alongX ? q1 : edge, alongX ? edge : q1)
							     << " m2=" << morphOf(b, alongX ? q2 : edge, alongX ? edge : q2) << " alongX " << alongX << " t " << t << " cam " << cam.x << " " << cam.y << " " << cam.z);
							REQUIRE(std::abs(hA - (h1 + (h2 - h1) * u)) < 1e-6);
							++compared;
						}
					}
				}
			}
	}
	CHECK(compared > 500);
}

TEST_CASE("landscape quadtree: a frustum keeps what is in it covered and drops the rest")
{
	const LandscapeQuadtreeConfig cfg = defaultConfig();
	const glm::dvec3 cam(50000.0, 700.0, 50000.0);
	const glm::dmat4 view = glm::lookAt(cam, cam + glm::dvec3(1.0, -0.1, 0.3), glm::dvec3(0.0, 1.0, 0.0));
	const glm::dmat4 proj = glm::perspective(glm::radians(70.0), 16.0 / 9.0, 1.0, 60000.0);
	const LandscapeFrustum fr = LandscapeFrustum::fromViewProjection(proj * view);
	const auto all = selectLandscapePatches(cfg, cam);
	const auto seen = selectLandscapePatches(cfg, cam, &fr);
	CHECK(seen.size() < all.size());
	MESSAGE("patches: " << all.size() << " without culling, " << seen.size() << " in a 70 degree view");
	CHECK(seen.size() > 20);
	CHECK(seen.size() < 3000);

	const auto rects = rectsOf(cfg, seen);
	std::mt19937 rng(2);
	std::uniform_real_distribution<double> around(-30000.0, 30000.0);
	int inside = 0;
	for (int i = 0; i < 20000; ++i)
	{
		const double x = cam.x + around(rng), z = cam.z + around(rng);
		const double y = ground(x, z);
		bool in = true;
		for (const glm::dvec4& p : fr.planes) in = in && (p.x * x + p.y * y + p.z * z + p.w >= 0.0);
		if (!in) continue;
		++inside;
		REQUIRE(coverCount(rects, x, z) == 1);
	}
	CHECK(inside > 500);
}

TEST_CASE("landscape quadtree: a tile that is not in memory is covered by the coarse pyramid, never left as a hole")
{
	const LandscapeQuadtreeConfig cfg = defaultConfig();
	const glm::dvec3 cam(40000.0, 600.0, 40000.0);
	const auto inDisc = [&](int tx, int tz) {
		const double dx = std::max({ tx * 512.0 - cam.x, 0.0, cam.x - (tx + 1) * 512.0 });
		const double dz = std::max({ tz * 512.0 - cam.z, 0.0, cam.z - (tz + 1) * 512.0 });
		return std::sqrt(dx * dx + dz * dz) <= 3000.0;
	};

	const auto patches = selectLandscapePatches(cfg, cam, nullptr, inDisc);
	const auto rects = rectsOf(cfg, patches);
	size_t fine = 0;
	for (const LandscapePatch& p : patches)
	{
		if (p.level > cfg.tileLevels) continue;
		++fine;
		// every tile under a fine patch is in memory
		const int tx0 = static_cast<int>(std::floor(p.x0 / 512.0)), tx1 = static_cast<int>(std::ceil((p.x0 + p.size) / 512.0)) - 1;
		const int tz0 = static_cast<int>(std::floor(p.z0 / 512.0)), tz1 = static_cast<int>(std::ceil((p.z0 + p.size) / 512.0)) - 1;
		for (int tz = tz0; tz <= tz1; ++tz)
			for (int tx = tx0; tx <= tx1; ++tx) REQUIRE(inDisc(tx, tz));
	}
	CHECK(fine > 0);

	std::mt19937 rng(8);
	std::uniform_real_distribution<double> world(0.0, worldMeters(cfg) - 1.0);
	for (int i = 0; i < 4000; ++i) REQUIRE(coverCount(rects, world(rng), world(rng)) == 1);

	// nothing in memory: only the coarse levels, still the whole world
	const auto none = selectLandscapePatches(cfg, cam, nullptr, [](int, int) { return false; });
	const auto noneRects = rectsOf(cfg, none);
	for (const LandscapePatch& p : none) CHECK(p.level > cfg.tileLevels);
	for (int i = 0; i < 2000; ++i) REQUIRE(coverCount(noneRects, world(rng), world(rng)) == 1);

	// more tiles in memory never means fewer fine patches
	size_t fineNone = 0;
	for (const LandscapePatch& p : none) fineNone += p.level <= cfg.tileLevels;
	CHECK(fineNone == 0);
}

TEST_CASE("landscape quadtree: a small world and a camera outside it")
{
	LandscapeQuadtreeConfig cfg;
	cfg.tilesX = 6;
	cfg.tilesZ = 3;                            // 3072 x 1536 m
	const glm::dvec3 cam(-20000.0, 900.0, 800.0);   // far outside, to the west
	const auto rects = rectsOf(cfg, selectLandscapePatches(cfg, cam));
	REQUIRE_FALSE(rects.empty());
	std::mt19937 rng(4);
	std::uniform_real_distribution<double> px(0.0, 3071.0), pz(0.0, 1535.0);
	for (int i = 0; i < 1000; ++i) REQUIRE(coverCount(rects, px(rng), pz(rng)) == 1);
	for (const Rect& r : rects) CHECK(r.x0 < 3072.0);   // nothing is drawn beyond the world
}

TEST_CASE("landscape quadtree: the config follows the source's geometry and says how far the store has to load")
{
	LandscapeGeometry g;   // 512 m tile, 256 cells
	const LandscapeQuadtreeConfig cfg = LandscapeQuadtreeConfig::fromGeometry(g, 196, 196);
	CHECK(cfg.tileM == doctest::Approx(512.0f));
	CHECK(cfg.cell0M == doctest::Approx(2.0f));
	CHECK(cfg.cellSize(cfg.tileLevels + 1) == doctest::Approx(32.0f));   // the first coarse level is the coarse world's raster
	CHECK(cfg.minLoadRadius() == doctest::Approx(6144.0f + 1.4143f * 1024.0f));
	CHECK(cfg.fitsStore(8000.0f));                                       // the store's default radius
	CHECK_FALSE(cfg.fitsStore(5000.0f));
}

TEST_CASE("landscape quadtree: with the store's disc loaded, neighbouring levels still differ by one at most")
{
	const LandscapeQuadtreeConfig cfg = defaultConfig();
	std::mt19937 rng(31);
	std::uniform_real_distribution<double> world(12000.0, worldMeters(cfg) - 12000.0), height(100.0, 2500.0);
	const auto maxJump = [&](float radius, const glm::dvec3& cam) {
		const auto inDisc = [&](int tx, int tz) {
			const double dx = std::max({ tx * 512.0 - cam.x, 0.0, cam.x - (tx + 1) * 512.0 });
			const double dz = std::max({ tz * 512.0 - cam.z, 0.0, cam.z - (tz + 1) * 512.0 });
			return std::sqrt(dx * dx + dz * dz) <= radius;
		};
		const auto rects = rectsOf(cfg, selectLandscapePatches(cfg, cam, nullptr, inDisc));
		int jump = 0;
		for (size_t i = 0; i < rects.size(); ++i)
			for (size_t j = i + 1; j < rects.size(); ++j)
			{
				const Rect &a = rects[i], &b = rects[j];
				const double eps = 1e-6;
				const bool touchX = std::abs(a.x0 + a.size - b.x0) < eps || std::abs(b.x0 + b.size - a.x0) < eps;
				const bool touchZ = std::abs(a.z0 + a.size - b.z0) < eps || std::abs(b.z0 + b.size - a.z0) < eps;
				const bool overlapX = std::min(a.x0 + a.size, b.x0 + b.size) - std::max(a.x0, b.x0) > eps;
				const bool overlapZ = std::min(a.z0 + a.size, b.z0 + b.size) - std::max(a.z0, b.z0) > eps;
				if ((touchX && overlapZ) || (touchZ && overlapX)) jump = std::max(jump, std::abs(a.level - b.level));
			}
		return jump;
	};
	int worstFit = 0, worstShort = 0;
	for (int round = 0; round < 5; ++round)
	{
		const glm::dvec3 cam(world(rng), height(rng), world(rng));
		worstFit = std::max(worstFit, maxJump(8000.0f, cam));          // the store's default radius, fits
		worstShort = std::max(worstShort, maxJump(3000.0f, cam));      // too small: the disc edge cuts the fine levels
	}
	MESSAGE("largest level jump between neighbours: " << worstFit << " with a 8 km store, " << worstShort << " with a 3 km store");
	CHECK(worstFit <= 1);
	CHECK(worstShort >= 1);
}
