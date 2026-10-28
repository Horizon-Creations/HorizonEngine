#pragma once
#include "RenderWorld.h"
#include <algorithm>
#include <cstddef>
#include <vector>

// ─── The height-field pyramid the GI shadow kernels march (all backends) ─────────
// Level 0 is the landscape's height field itself; level k texel (i,j) is the MAX of level k-1
// (2i..2i+3, 2j..2j+3). A cell of level k stands for the vertices [i*2^k, (i+1)*2^k] and the kernels
// advance a ray at most half a cell at a time, so the surface it can touch between two samples is
// interpolated from vertices up to (i+1)*2^k + 2^(k-1); the 4-wide blocks cover that (level 1 needs
// 2i..2i+3 exactly, higher levels have room to spare). A ray that stays above a cell's maximum
// therefore cannot touch the surface during the step it takes from inside that cell.
// Level sizes follow the usual mip rule (max(1, s >> 1)), so the levels can go straight into a
// mip-mapped texture. The chain stops when a level is smaller than 8 texels.
namespace HE
{

struct GiHeightPyramid
{
	std::vector<std::vector<float>> levels;   // levels[0] = the field (res x res, row-major)
	std::vector<int>                dims;     // texels per axis of each level
	float                           maxHeight = 0.0f;
};

inline GiHeightPyramid BuildGiHeightPyramid(const GiHeightfield& hf)
{
	GiHeightPyramid p;
	if (!hf.data || hf.res < 8 || hf.data->size() < size_t(hf.res) * hf.res) return p;
	const int res = static_cast<int>(hf.res);
	p.levels.emplace_back(hf.data->begin(), hf.data->begin() + size_t(res) * res);
	p.dims.push_back(res);
	while (p.dims.back() >= 8)
	{
		const int s = p.dims.back(), n = std::max(1, s >> 1);
		std::vector<float> nxt(size_t(n) * n);
		const std::vector<float>& prev = p.levels.back();
		for (int j = 0; j < n; ++j)
			for (int i = 0; i < n; ++i)
			{
				float m = -3.4e38f;
				for (int dj = 0; dj < 4; ++dj)
					for (int di = 0; di < 4; ++di)
						m = std::max(m, prev[size_t(std::min(2 * j + dj, s - 1)) * s + std::min(2 * i + di, s - 1)]);
				nxt[size_t(j) * n + i] = m;
			}
		p.levels.push_back(std::move(nxt));
		p.dims.push_back(n);
	}
	p.maxHeight = *std::max_element(p.levels[0].begin(), p.levels[0].end());
	return p;
}

} // namespace HE
