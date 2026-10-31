#include "HorizonScene/LandscapeGpuLayout.h"

#include <algorithm>
#include <cstring>

namespace HE
{
namespace
{
// Level 0 at (0, 0); every further level in a column to its right, one below the other.
std::vector<LandscapeImageRect> mipChain(const std::vector<int>& dims, int& totalW, int& totalH)
{
	std::vector<LandscapeImageRect> r(dims.size());
	totalW = totalH = 0;
	if (dims.empty()) return r;
	r[0] = { 0, 0, dims[0], dims[0] };
	totalW = dims[0];
	totalH = dims[0];
	int y = 0, col = 0;
	for (size_t k = 1; k < dims.size(); ++k)
	{
		r[k] = { dims[0], y, dims[k], dims[k] };
		y += dims[k];
		col = std::max(col, dims[k]);
	}
	totalW = dims[0] + col;
	totalH = std::max(dims[0], y);
	return r;
}
} // namespace

LandscapeTileImageLayout LandscapeTileImageLayout::forGeometry(const LandscapeGeometry& g)
{
	LandscapeTileImageLayout l;
	std::vector<int> hd, wd;
	for (int k = 0; k < g.heightLevels(); ++k) hd.push_back(g.heightDim(k));
	for (int k = 0; k < g.weightLevels(); ++k) wd.push_back(g.weightDim(k));
	l.heightRect = mipChain(hd, l.heightW, l.heightH);
	l.weightRect = mipChain(wd, l.weightW, l.weightH);
	l.weightPages = (g.channels + 3) / 4;
	return l;
}

void landscapePackTile(const LandscapeTile& tile, const LandscapeTileImageLayout& layout, std::vector<uint16_t>& heights,
                       std::vector<std::vector<uint8_t>>& pages)
{
	heights.assign(static_cast<size_t>(layout.heightW) * layout.heightH, 0);
	pages.assign(static_cast<size_t>(layout.weightPages), std::vector<uint8_t>(static_cast<size_t>(layout.weightW) * layout.weightH * 4, 0));
	const LandscapeGeometry& g = tile.geometry;
	for (size_t k = 0; k < tile.heights.size() && k < layout.heightRect.size(); ++k)
	{
		const LandscapeImageRect& r = layout.heightRect[k];
		for (int z = 0; z < r.h; ++z)
			std::memcpy(&heights[static_cast<size_t>(r.y + z) * layout.heightW + r.x], &tile.heights[k][static_cast<size_t>(z) * r.w],
			            static_cast<size_t>(r.w) * 2);
	}
	for (size_t k = 0; k < tile.weights.size() && k < layout.weightRect.size(); ++k)
	{
		const LandscapeImageRect& r = layout.weightRect[k];
		const int C = g.channels;
		for (int z = 0; z < r.h; ++z)
			for (int x = 0; x < r.w; ++x)
				for (int c = 0; c < C; ++c)
					pages[static_cast<size_t>(c / 4)][(static_cast<size_t>(r.y + z) * layout.weightW + r.x + x) * 4 + static_cast<size_t>(c % 4)] =
					    tile.weights[k][(static_cast<size_t>(z) * r.w + x) * C + c];
	}
}

LandscapeCoarseImageLayout LandscapeCoarseImageLayout::forCoarse(const LandscapeCoarseWorld& c)
{
	LandscapeCoarseImageLayout l;
	int row = 0;
	for (const LandscapeGrid16& g : c.heights)
	{
		l.grids.push_back({ g.w, g.h, row });
		row += g.h;
		l.width = std::max(l.width, g.w);
	}
	l.height = row;
	l.weightW = c.weightsW;
	l.weightH = c.weightsH;
	l.weightPages = (c.channels + 3) / 4;
	return l;
}

void landscapePackCoarse(const LandscapeCoarseWorld& c, const LandscapeCoarseImageLayout& layout, std::vector<uint16_t>& heights,
                         std::vector<std::vector<uint8_t>>& pages)
{
	heights.assign(static_cast<size_t>(layout.width) * layout.height, 0);
	for (size_t i = 0; i < c.heights.size() && i < layout.grids.size(); ++i)
	{
		const auto& gr = layout.grids[i];
		for (int z = 0; z < gr.h; ++z)
			std::memcpy(&heights[static_cast<size_t>(gr.rowOffset + z) * layout.width], &c.heights[i].v[static_cast<size_t>(z) * gr.w], static_cast<size_t>(gr.w) * 2);
	}
	pages.assign(static_cast<size_t>(layout.weightPages), std::vector<uint8_t>(static_cast<size_t>(layout.weightW) * layout.weightH * 4, 0));
	for (int z = 0; z < layout.weightH; ++z)
		for (int x = 0; x < layout.weightW; ++x)
			for (int ch = 0; ch < c.channels; ++ch)
				pages[static_cast<size_t>(ch / 4)][(static_cast<size_t>(z) * layout.weightW + x) * 4 + static_cast<size_t>(ch % 4)] =
				    c.weights[(static_cast<size_t>(z) * c.weightsW + x) * c.channels + ch];
}

} // namespace HE
