#include "HorizonScene/LandscapeTileTerrain.h"

#include "HorizonScene/WaterField.h"

#include <algorithm>
#include <set>

namespace HE
{

TerrainComponent landscapeTileTerrain(const LandscapeTile& tile, const LandscapeTileTerrainParams& params)
{
	TerrainComponent tc;
	const LandscapeGeometry& g = tile.geometry;
	tc.sizeX = tc.sizeZ = 0.0f;
	const uint32_t res = static_cast<uint32_t>(g.heightDim(0));
	if (tile.heights.empty() || res < 2 || res > kTerrainMaxResolution || tile.heights[0].size() != static_cast<size_t>(res) * res) return tc;

	tc.sizeX = tc.sizeZ = g.tileM;
	tc.resolution = res;
	tc.seed = 0;
	tc.heightScale = params.heightScaleM;
	tc.sculptHeights.resize(static_cast<size_t>(res) * res);
	const float k = params.heightScaleM / 65535.0f;
	for (size_t i = 0; i < tc.sculptHeights.size(); ++i) tc.sculptHeights[i] = params.heightMinM + static_cast<float>(tile.heights[0][i]) * k;

	// Weights: the tile's level 0, channel c into page c / 4, channel c % 4. A page is only there when a channel needs it.
	if (!tile.weights.empty() && g.channels > 0)
	{
		const uint32_t wr = static_cast<uint32_t>(g.weightDim(0));
		const int C = g.channels;
		const size_t texels = static_cast<size_t>(wr) * wr;
		if (tile.weights[0].size() == texels * static_cast<size_t>(C))
		{
			tc.weightRes = wr;
			std::vector<uint8_t>* pages[3] = { &tc.layerWeights, &tc.layerWeights2, &tc.layerWeights3 };
			const int usedPages = std::min(3, (C + 3) / 4);
			for (int p = 0; p < usedPages; ++p)
			{
				pages[p]->assign(texels * 4, 0);
				for (size_t t = 0; t < texels; ++t)
					for (int c = 0; c < 4; ++c)
						if (p * 4 + c < C) (*pages[p])[t * 4 + static_cast<size_t>(c)] = tile.weights[0][t * static_cast<size_t>(C) + static_cast<size_t>(p * 4 + c)];
			}
		}
	}
	tc.dirty = false;
	return tc;
}

bool landscapeRegionTerrain(const std::function<std::shared_ptr<const LandscapeTile>(LandscapeTileKey)>& tileAt, LandscapeTileKey origin, int n,
                            const LandscapeTileTerrainParams& params, const LandscapeWaterTable* waterTable, LandscapeRegionTerrain& out)
{
	out = LandscapeRegionTerrain{};
	if (n < 1 || !tileAt) return false;
	std::vector<std::shared_ptr<const LandscapeTile>> tiles(static_cast<size_t>(n) * n);
	for (int j = 0; j < n; ++j)
		for (int i = 0; i < n; ++i)
		{
			tiles[static_cast<size_t>(j) * n + i] = tileAt({ origin.x + i, origin.z + j });
			if (!tiles[static_cast<size_t>(j) * n + i] || tiles[static_cast<size_t>(j) * n + i]->heights.empty()) return false;
		}
	const LandscapeGeometry g = tiles[0]->geometry;
	for (const auto& t : tiles)
		if (!(t->geometry == g)) return false;
	const uint32_t cells = g.hCells, res = static_cast<uint32_t>(n) * cells + 1;
	if (res > kTerrainMaxResolution) return false;

	TerrainComponent& tc = out.terrain;
	tc.sizeX = tc.sizeZ = g.tileM * static_cast<float>(n);
	tc.resolution = res;
	tc.seed = 0;
	tc.heightScale = params.heightScaleM;
	tc.sculptHeights.assign(static_cast<size_t>(res) * res, 0.0f);
	const float k = params.heightScaleM / 65535.0f;
	const uint32_t dim = cells + 1;
	for (int tj = 0; tj < n; ++tj)
		for (int ti = 0; ti < n; ++ti)
		{
			const std::vector<uint16_t>& h = tiles[static_cast<size_t>(tj) * n + ti]->heights[0];
			for (uint32_t z = 0; z < dim; ++z)
				for (uint32_t x = 0; x < dim; ++x)
					tc.sculptHeights[(static_cast<size_t>(tj) * cells + z) * res + static_cast<size_t>(ti) * cells + x] =
					    params.heightMinM + static_cast<float>(h[static_cast<size_t>(z) * dim + x]) * k;
		}

	// weights: the tiles' level 0 side by side, de-interleaved into the pages
	const uint32_t wt = static_cast<uint32_t>(g.weightDim(0)), wr = static_cast<uint32_t>(n) * wt;
	const int C = g.channels;
	bool weightsOk = !tiles[0]->weights.empty() && C > 0;
	for (const auto& t : tiles) weightsOk = weightsOk && !t->weights.empty() && t->weights[0].size() == static_cast<size_t>(wt) * wt * static_cast<size_t>(C);
	if (weightsOk)
	{
		tc.weightRes = wr;
		std::vector<uint8_t>* pages[3] = { &tc.layerWeights, &tc.layerWeights2, &tc.layerWeights3 };
		const int usedPages = std::min(3, (C + 3) / 4);
		for (int p = 0; p < usedPages; ++p) pages[p]->assign(static_cast<size_t>(wr) * wr * 4, 0);
		for (int tj = 0; tj < n; ++tj)
			for (int ti = 0; ti < n; ++ti)
			{
				const std::vector<uint8_t>& w = tiles[static_cast<size_t>(tj) * n + ti]->weights[0];
				for (uint32_t z = 0; z < wt; ++z)
					for (uint32_t x = 0; x < wt; ++x)
					{
						const size_t dst = (static_cast<size_t>(tj) * wt + z) * wr + static_cast<size_t>(ti) * wt + x;
						for (int c = 0; c < C && c < 12; ++c)
							(*pages[c / 4])[dst * 4 + static_cast<size_t>(c % 4)] = w[(static_cast<size_t>(z) * wt + x) * static_cast<size_t>(C) + static_cast<size_t>(c)];
					}
			}
	}

	out.origin = origin;
	out.n = n;
	out.centre = { (origin.x + n * 0.5) * static_cast<double>(g.tileM), 0.0, (origin.z + n * 0.5) * static_cast<double>(g.tileM) };

	// water: one Field over the region
	if (waterTable)
	{
		// Any tile with water makes the Field; a tile without streams (an authored world has dry tiles beside a lake) is dry in it.
		const auto tileWater = [wt](const LandscapeTile& t) { return t.hasWater() && t.waterCoverage.size() == static_cast<size_t>(wt) * wt; };
		bool anyWater = false;
		for (const auto& t : tiles) anyWater = anyWater || tileWater(*t);
		if (anyWater)
		{
			water::Field& f = tc.water;
			f.res = wr;
			f.coverage.assign(static_cast<size_t>(wr) * wr, 0);
			f.owner.assign(static_cast<size_t>(wr) * wr, 0);
			std::set<uint16_t> present;
			for (int tj = 0; tj < n; ++tj)
				for (int ti = 0; ti < n; ++ti)
				{
					const LandscapeTile& t = *tiles[static_cast<size_t>(tj) * n + ti];
					if (!tileWater(t)) continue;
					for (uint32_t z = 0; z < wt; ++z)
						for (uint32_t x = 0; x < wt; ++x)
						{
							const size_t src = static_cast<size_t>(z) * wt + x, dst = (static_cast<size_t>(tj) * wt + z) * wr + static_cast<size_t>(ti) * wt + x;
							f.coverage[dst] = t.waterCoverage[src];
							f.owner[dst] = t.waterOwner[src];
							if (t.waterCoverage[src] != 0 && t.waterOwner[src] != 0) present.insert(t.waterOwner[src]);
						}
				}
			uint16_t maxId = 0;
			for (const uint16_t id : present)
			{
				const LandscapeWaterBody* wb = waterTable->find(id);
				if (!wb) continue;   // a cell names a body the pack does not know: its water stays in the raster, unowned in effect (no level)
				water::Body b;
				b.id = wb->id;
				b.level = wb->level;
				b.physics = wb->physics;
				b.underwaterFog = wb->underwaterFog;
				b.underwaterCaustics = wb->underwaterCaustics;
				b.tessellation = wb->tessellation;
				f.bodies.push_back(b);
				maxId = std::max(maxId, id);
			}
			f.nextBodyId = static_cast<uint16_t>(maxId + 1);
			f.clipToGround = waterTable->clipToGround;
			f.shoreOvershoot = waterTable->shoreOvershoot;
			f.tessellation = waterTable->tessellation;
			f.dirty = true;
			out.hasWater = !f.bodies.empty();
		}
	}
	tc.dirty = false;
	return true;
}

} // namespace HE
