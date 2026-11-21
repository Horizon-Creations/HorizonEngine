// The render window of a tile landscape (Thema 326): a world with more tiles a side than LandscapeRuntimeConfig::renderWindowTiles is
// handed to the renderer as a window of tiles around the camera, in window coordinates, and everything always in memory for the far
// levels is the window's. A world that fits is untouched (that half is what every other landscape test keeps checking).
#include "doctest.h"
#include <HorizonScene/LandscapeFarBake.h>
#include <HorizonScene/LandscapeFarMap.h>
#include <HorizonScene/LandscapeRuntime.h>
#include <HorizonScene/LandscapeSource.h>

#include <glm/gtc/matrix_transform.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

using namespace HE;

namespace
{
// A world of any size that costs nothing: heights are a formula of the absolute position, all paint on layer 0. It has NO coarse()
// (nobody could hold one for 500 km): the far levels come from coarseWindow(), computed for the window asked for.
class FormulaSource final : public LandscapeTileSource
{
public:
	explicit FormulaSource(int tiles) : m_tiles(tiles)
	{
		ProceduralLandscapeParams p;
		p.tilesX = p.tilesZ = 1;
		m_base = ProceduralLandscapeSource(p).generate({ 0, 0 });
		const int ch = m_base.geometry.channels;
		for (auto& level : m_base.weights)
			for (size_t t = 0; t + static_cast<size_t>(ch) <= level.size(); t += static_cast<size_t>(ch))
				for (int c = 0; c < ch; ++c) level[t + static_cast<size_t>(c)] = c == 0 ? 255 : 0;
	}
	static uint16_t heightAt(double x, double z) { return static_cast<uint16_t>(std::lround(30000.0 + 8000.0 * std::sin(x / 3000.0) * std::cos(z / 4100.0))); }

	const LandscapeGeometry& geometry() const override { return m_base.geometry; }
	int  tilesX() const override { return m_tiles; }
	int  tilesZ() const override { return m_tiles; }
	bool heightRange(float& minM, float& scaleM) const override { minM = 0.0f; scaleM = 3000.0f; return true; }
	bool load(LandscapeTileKey key, int maxLevels, LandscapeTile& out) const override
	{
		if (key.x < 0 || key.z < 0 || key.x >= m_tiles || key.z >= m_tiles) return false;
		out = m_base;
		const double spacing = out.geometry.tileM / out.geometry.hCells;
		for (size_t level = 0; level < out.heights.size(); ++level)
		{
			const int dim = out.geometry.heightDim(static_cast<int>(level));
			for (int j = 0; j < dim; ++j)
				for (int i = 0; i < dim; ++i)
					out.heights[level][static_cast<size_t>(j) * static_cast<size_t>(dim) + static_cast<size_t>(i)] =
					    heightAt((static_cast<double>(key.x) * out.geometry.hCells + static_cast<double>(i) * (1 << level)) * spacing,
					             (static_cast<double>(key.z) * out.geometry.hCells + static_cast<double>(j) * (1 << level)) * spacing);
		}
		if (maxLevels >= 0)
		{
			out.heights.resize(std::min<size_t>(out.heights.size(), static_cast<size_t>(maxLevels)));
			out.weights.resize(std::min<size_t>(out.weights.size(), static_cast<size_t>(maxLevels)));
		}
		return true;
	}
	std::shared_ptr<const LandscapeCoarseWorld> coarseWindow(LandscapeTileKey origin, int tx, int tz) const override
	{
		++windowsMade;
		const LandscapeGeometry& g = m_base.geometry;
		const LandscapeCoarseLayout layout = LandscapeCoarseLayout::forGeometry(g, 4);
		LandscapeCoarseWorld c;
		c.tilesX = tx;
		c.tilesZ = tz;
		c.tileM = g.tileM;
		c.heightLevel = layout.heightLevel;
		c.cellsPerTile = g.hCells >> layout.heightLevel;
		c.weightLevel = layout.weightLevel;
		c.texelsPerTile = g.wTexels >> layout.weightLevel;
		c.channels = g.channels;
		const double cell = g.tileM / c.cellsPerTile;
		LandscapeGrid16 g0;
		g0.w = tx * c.cellsPerTile + 1;
		g0.h = tz * c.cellsPerTile + 1;
		g0.v.resize(static_cast<size_t>(g0.w) * g0.h);
		for (int z = 0; z < g0.h; ++z)
			for (int x = 0; x < g0.w; ++x)
				g0.v[static_cast<size_t>(z) * g0.w + x] = heightAt((static_cast<double>(origin.x) * c.cellsPerTile + x) * cell, (static_cast<double>(origin.z) * c.cellsPerTile + z) * cell);
		c.heights.push_back(std::move(g0));
		c.weightsW = tx * c.texelsPerTile;
		c.weightsH = tz * c.texelsPerTile;
		c.weights.assign(static_cast<size_t>(c.weightsW) * c.weightsH * c.channels, 0);
		for (size_t t = 0; t < c.weights.size(); t += c.channels) c.weights[t] = 255;
		// The grids above grid 0, by the library's own rule (a crop of everything is a copy with the chain built).
		return std::make_shared<const LandscapeCoarseWorld>(landscapeCoarseCrop(c, { 0, 0 }, tx, tz));
	}
	mutable std::atomic<int> windowsMade{ 0 };

private:
	int           m_tiles;
	LandscapeTile m_base;
};

bool settle(LandscapeRuntime& rt, const glm::dvec3& cam, int ms = 30000)
{
	const auto t0 = std::chrono::steady_clock::now();
	for (;;)
	{
		rt.updateAt(cam, glm::vec3(0.0f), 5.0);
		if (rt.store()->settled() && rt.atlas()->stats().used == rt.store()->residentKeys().size()) return true;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > ms) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

glm::dmat4 viewProj(const glm::dvec3& camAbs, const glm::dvec3& origin)
{
	const glm::dvec3 eye = camAbs - origin;
	return glm::perspective(glm::radians(70.0), 16.0 / 9.0, 1.0, 40000.0) * glm::lookAt(eye, eye + glm::dvec3(1, -0.1, 0.2), glm::dvec3(0.0, 1.0, 0.0));
}
} // namespace

TEST_CASE("landscapeCoarseCrop: a window of the coarse world holds the whole world's own samples, on every grid")
{
	ProceduralLandscapeParams p;
	p.tilesX = p.tilesZ = 12;
	const ProceduralLandscapeSource src(p);
	const LandscapeCoarseWorld* whole = src.coarse();
	REQUIRE(whole);
	const LandscapeTileKey origin{ 4, 8 };
	const LandscapeCoarseWorld win = landscapeCoarseCrop(*whole, origin, 4, 3);
	REQUIRE(win.valid());
	CHECK(win.tilesX == 4);
	CHECK(win.tilesZ == 3);
	const int cpt = whole->cellsPerTile, tpt = whole->texelsPerTile;
	CHECK(win.heights[0].w == 4 * cpt + 1);
	CHECK(win.heights[0].h == 3 * cpt + 1);
	// 4 and 8 tiles are 64 and 128 cells: a multiple of 2^k for the first six grids, so each of those is the whole world's own sampling.
	for (size_t k = 0; k < win.heights.size() && k < 6; ++k)
	{
		const int ox = (origin.x * cpt) >> k, oz = (origin.z * cpt) >> k;
		int bad = 0;
		for (int z = 0; z < win.heights[k].h; ++z)
			for (int x = 0; x < win.heights[k].w; ++x) bad += win.heights[k].at(x, z) != whole->heights[k].at(ox + x, oz + z) ? 1 : 0;
		CHECK_MESSAGE(bad == 0, "grid " << k);
	}
	int badW = 0;
	for (int z = 0; z < win.weightsH; ++z)
		for (int x = 0; x < win.weightsW; ++x)
			for (int c = 0; c < win.channels; ++c)
				badW += win.weights[(static_cast<size_t>(z) * win.weightsW + x) * win.channels + c]
				        != whole->weights[(static_cast<size_t>(origin.z * tpt + z) * whole->weightsW + origin.x * tpt + x) * whole->channels + c] ? 1 : 0;
	CHECK(badW == 0);
	// A window past the world's end is clipped to it; the whole world as a window is the source's own object, not a copy.
	CHECK(landscapeCoarseCrop(*whole, { 10, 10 }, 8, 8).tilesX == 2);
	CHECK(src.coarseWindow({ 0, 0 }, 12, 12).get() == whole);
	CHECK(src.coarseWindow({ 4, 8 }, 4, 3).get() != whole);
}

TEST_CASE("LandscapeFarTable::assemble: a window is the whole image's own texels, across region borders and at the world's edge")
{
	ProceduralLandscapeParams p;
	p.geometry.hCells = 32;
	p.geometry.wTexels = 16;
	p.geometry.channels = 4;
	p.tilesX = p.tilesZ = 5;
	const ProceduralLandscapeSource src(p);
	LandscapeFarPaint paint;
	paint.layerAo.fill(1.0f);
	for (size_t i = 0; i < 12; ++i) paint.layerColor[i] = glm::vec3(0.1f + 0.07f * static_cast<float>(i), 0.5f, 0.9f - 0.07f * static_cast<float>(i));
	LandscapeFarBakeOptions o;
	o.regionTiles = 2;   // 3 x 3 regions, the last ones hang over the world's edge
	o.painter = landscapeFarWeightPainter(paint);
	LandscapeFarTable table;
	LandscapeFarBakeStats st;
	bakeLandscapeFar(src, {}, o, nullptr, table, st);
	REQUIRE(st.baked == 9);
	int w = 0, h = 0, ww = 0, wh = 0;
	std::vector<std::vector<uint8_t>> whole, win;
	table.assemble(w, h, whole);
	const int tpt = table.layout.texelsPerTile;
	// Tiles 1..4 in x (crosses two region borders, ends with the world), tiles 3..4 in z.
	table.assemble(ww, wh, win, 1, 3, 4, 2);
	REQUIRE(ww == 4 * tpt);
	REQUIRE(wh == 2 * tpt);
	int bad = 0;
	for (int z = 0; z < wh; ++z)
		for (int x = 0; x < ww; ++x)
			for (int c = 0; c < 4; ++c)
				bad += win[0][(static_cast<size_t>(z) * ww + x) * 4 + c] != whole[0][(static_cast<size_t>(3 * tpt + z) * w + tpt + x) * 4 + c] ? 1 : 0;
	CHECK(bad == 0);
	CHECK(win.size() > 1);   // with its own mip chain
	// Asked past the world: clipped.
	table.assemble(ww, wh, win, 3, 3, 100, 100);
	CHECK(ww == 2 * tpt);
	CHECK(wh == 2 * tpt);
}

TEST_CASE("landscape runtime: a world larger than the render window is drawn as a window around the camera, in window coordinates")
{
	const auto src = std::make_shared<FormulaSource>(1000);   // 512 km a side
	REQUIRE(src->coarse() == nullptr);                        // nothing of it is ever held whole
	LandscapeRuntime rt;
	LandscapeRuntimeConfig cfg;
	std::string err;
	REQUIRE_MESSAGE(rt.begin(src, cfg, &err), err);
	CHECK(rt.windowed());
	const double tileM = 512.0;
	const glm::dvec3 cam(585.5 * tileM, 2200.0, 585.5 * tileM), origin(585.0 * tileM, 0.0, 585.0 * tileM);
	REQUIRE(settle(rt, cam));
	const LandscapeTileKey win = rt.windowTile();
	CHECK(win.x == 448);                                      // a multiple of the slot table's 64, the camera in the middle band
	CHECK(win.z == 448);
	LandscapeFrame f;
	rt.buildFrame(viewProj(cam, origin), origin, false, f);
	REQUIRE(f.active);
	CHECK(f.windowTile == win);
	CHECK(f.tilesX == 256);
	CHECK(f.tilesZ == 256);
	CHECK(f.worldMaxX == doctest::Approx(256.0 * tileM));
	const glm::dvec3 corner(win.x * tileM, 0.0, win.z * tileM);
	CHECK(glm::length(f.origin - (origin - corner)) < 1e-6);            // the frame's origin and camera are the window's
	CHECK(glm::length(f.cameraAbsolute - (cam - corner)) < 1e-6);
	CHECK(glm::length(f.uvOrigin - origin) < 1e-6);                     // the texture phase keeps the absolute origin
	REQUIRE(f.coarse);
	CHECK(f.coarse->tilesX == 256);
	CHECK(f.coarse->heights[0].w == 256 * f.coarse->cellsPerTile + 1);  // the window's coarse world, 4097 samples a side, not 16001
	// What the coarse world holds is the ground at the window's place: a sample, against the formula at its absolute position.
	const double cell = f.coarse->cellM(0);
	CHECK(f.coarse->heights[0].at(1000, 2000) == FormulaSource::heightAt(corner.x + 1000 * cell, corner.z + 2000 * cell));

	// Every patch lies inside the window, in window coordinates; fine ones are there (the atlas was asked with the world's keys).
	REQUIRE_FALSE(f.patches.empty());
	int outside = 0, fine = 0;
	for (const LandscapeFrame::Patch& p : f.patches)
	{
		const double x0 = p.x0 + f.origin.x, z0 = p.z0 + f.origin.z;
		outside += (x0 < -1.0 || z0 < -1.0 || x0 + p.size > 256.0 * tileM + 1.0 || z0 + p.size > 256.0 * tileM + 1.0) ? 1 : 0;
		fine += !p.coarse ? 1 : 0;
		CHECK(p.ix >= 0);
		CHECK(p.iz >= 0);
	}
	CHECK(outside == 0);
	CHECK(fine > 0);
	// The slot table: a tile's entry by its world key is its entry by its window key (the corner is a multiple of the table's size).
	REQUIRE_FALSE(f.uploads.empty());
	const int D = f.tableWindow;
	for (const LandscapeAtlas::Upload& u : f.uploads)
	{
		CHECK(std::abs(u.key.x - 585) < 40);   // the world's keys, around the camera
		CHECK(((u.key.z & (D - 1)) * D + (u.key.x & (D - 1))) == (((u.key.z - win.z) & (D - 1)) * D + ((u.key.x - win.x) & (D - 1))));
	}
	const uint64_t generation = f.coarseGeneration;
	const int windows = src->windowsMade.load();

	// 10 km on: still the middle band, nothing moves and nothing is made again.
	const glm::dvec3 cam2 = cam + glm::dvec3(10000.0, 0.0, 0.0);
	REQUIRE(settle(rt, cam2));
	rt.buildFrame(viewProj(cam2, origin), origin, false, f);
	CHECK(rt.windowTile() == win);
	CHECK(f.coarseGeneration == generation);
	CHECK(src->windowsMade.load() == windows);

	// 40 km on: the window moves by whole table sizes, and to a renderer that is another landscape (its images are made again).
	const glm::dvec3 cam3 = cam + glm::dvec3(40000.0, 0.0, 0.0);
	rt.updateAt(cam3, glm::vec3(0.0f), 5.0);
	CHECK(rt.windowTile().x == 512);
	CHECK(rt.windowTile().z == 448);
	CHECK(rt.atlas()->stats().used <= rt.config().maxUploadsPerFrame);   // the atlas began again: at most one frame's uploads are in it
	REQUIRE(settle(rt, cam3));
	const glm::dvec3 origin3(663.0 * tileM, 0.0, 585.0 * tileM);
	rt.buildFrame(viewProj(cam3, origin3), origin3, false, f);
	REQUIRE(f.active);
	CHECK(f.coarseGeneration != generation);
	CHECK(f.windowTile.x == 512);
	CHECK(f.coarse->heights[0].at(1000, 2000) == FormulaSource::heightAt(512 * tileM + 1000 * cell, 448 * tileM + 2000 * cell));
	fine = 0;
	for (const LandscapeFrame::Patch& p : f.patches) fine += !p.coarse ? 1 : 0;
	CHECK(fine > 0);

	// At the world's far corner the window ends with the world: fewer tiles, never past it.
	const glm::dvec3 cam4(990.5 * tileM, 2200.0, 990.5 * tileM), origin4(990.0 * tileM, 0.0, 990.0 * tileM);
	REQUIRE(settle(rt, cam4));
	rt.buildFrame(viewProj(cam4, origin4), origin4, false, f);
	CHECK(rt.windowTile().x == 768);
	CHECK(f.tilesX == 232);
	CHECK(f.worldMaxX == doctest::Approx(232.0 * tileM));
}

TEST_CASE("landscape runtime: a world that fits the window is its own window, and the window's size has to fit the slot table")
{
	// 300 tiles (154 km) with the window switched off: the whole world, however large.
	{
		const auto src = std::make_shared<FormulaSource>(300);
		LandscapeRuntime rt;
		LandscapeRuntimeConfig cfg;
		cfg.renderWindowTiles = 0;
		std::string err;
		REQUIRE_MESSAGE(rt.begin(src, cfg, &err), err);
		CHECK_FALSE(rt.windowed());
		const glm::dvec3 cam(250.5 * 512.0, 2200.0, 250.5 * 512.0), origin(0.0);
		rt.updateAt(cam, glm::vec3(0.0f), 5.0);
		CHECK(rt.windowTile() == LandscapeTileKey{ 0, 0 });
		LandscapeFrame f;
		rt.buildFrame(viewProj(cam, origin), origin, false, f);
		CHECK(f.tilesX == 300);
		CHECK(glm::length(f.origin - origin) < 1e-9);
	}
	// 200 tiles with the default window of 256: fits, nothing is windowed.
	{
		const auto src = std::make_shared<FormulaSource>(200);
		LandscapeRuntime rt;
		REQUIRE(rt.begin(src, LandscapeRuntimeConfig{}));
		CHECK_FALSE(rt.windowed());
		rt.updateAt({ 90000.0, 2200.0, 90000.0 }, glm::vec3(0.0f), 5.0);
		CHECK(rt.windowTile() == LandscapeTileKey{ 0, 0 });
	}
	// A window that is not whole slot tables, or fewer than four of them, is refused: a tile's table entry would differ between the two spaces.
	for (const int bad : { 100, 64, 128, 300 })
	{
		const auto src = std::make_shared<FormulaSource>(1000);
		LandscapeRuntime rt;
		LandscapeRuntimeConfig cfg;
		cfg.renderWindowTiles = bad;
		std::string err;
		CHECK_FALSE(rt.begin(src, cfg, &err));
		CHECK(err.find("renderWindowTiles") != std::string::npos);
	}
}
