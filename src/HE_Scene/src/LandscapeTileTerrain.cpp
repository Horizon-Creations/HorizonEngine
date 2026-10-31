#include "HorizonScene/LandscapeTileTerrain.h"

#include <algorithm>

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

} // namespace HE
