#pragma once
#include "RenderWorld.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
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

// ─── Closest-hit march (Thema 326, step 16: reflections against the tile landscape) ─────────────
// CPU reference of hfMarch/hfClosestHit (kGiHeightfieldGLSL, OpenGLRenderer.cpp), kept in sync by
// hand across GLSL/HLSL copies exactly like HE::giAutoLandscapeAlbedo (GiLandscape.h) mirrors its
// three kernel copies. hfAnyHit (the GI shadow march, all three heightfield backends) already finds
// this same first crossing and throws it away by returning a bare bool; this is that same march
// with the hit kept. oL/dL are the ray already in the field's LOCAL space; for the tile landscape's
// clipmap that space equals world space (worldToLocal is translation-only, see
// RenderExtractor::extractLandscapeGiHeightfield), so a caller with a translation-only transform can
// skip transforming the hit back. Returns the first (closest) crossing's ray parameter and a local-
// space normal from central differences of the bilinear-interpolated field; false on a miss or a
// disabled field (res < 2, no pyramid).
inline bool GiHeightPyramidClosestHitRef(const GiHeightPyramid& pyr, const glm::vec3& oL, const glm::vec3& dL,
                                          float tMin, float tMax, float invSizeX, float invSizeZ,
                                          float& outT, glm::vec3& outNormalL, int maxSteps = 512)
{
	if (pyr.levels.empty()) return false;
	const int   resM1i = pyr.dims[0] - 1;
	const float resM1  = static_cast<float>(resM1i);
	const int   topLvl = static_cast<int>(pyr.levels.size()) - 1;

	auto bilinear = [&](glm::vec2 tp) -> float
	{
		tp = glm::clamp(tp, glm::vec2(0.0f), glm::vec2(resM1));
		const int x0 = static_cast<int>(std::floor(tp.x)), y0 = static_cast<int>(std::floor(tp.y));
		const int x1 = std::min(x0 + 1, resM1i), y1 = std::min(y0 + 1, resM1i);
		const float fx = tp.x - static_cast<float>(x0), fy = tp.y - static_cast<float>(y0);
		const std::vector<float>& l0 = pyr.levels[0];
		const int s = pyr.dims[0];
		const float h00 = l0[size_t(y0) * s + x0], h10 = l0[size_t(y0) * s + x1];
		const float h01 = l0[size_t(y1) * s + x0], h11 = l0[size_t(y1) * s + x1];
		return (h00 * (1.0f - fx) + h10 * fx) * (1.0f - fy) + (h01 * (1.0f - fx) + h11 * fx) * fy;
	};
	// texelFetch(uHf, cell, lvl): the conservative-max texel this mip level holds for `cell`.
	auto texelMax = [&](int cx, int cz, int lvl) -> float
	{
		const int s    = pyr.dims[lvl];
		const int last = std::max(1, (resM1i + 1) >> lvl) - 1;
		cx = std::clamp(cx, 0, last);
		cz = std::clamp(cz, 0, last);
		return pyr.levels[lvl][size_t(cz) * s + cx];
	};

	const glm::vec2 texPerLocal(invSizeX * resM1, invSizeZ * resM1);
	const glm::vec2 tp0((oL.x * invSizeX + 0.5f) * resM1, (oL.z * invSizeZ + 0.5f) * resM1);
	const glm::vec2 tpd(dL.x * texPerLocal.x, dL.z * texPerLocal.y);
	const float texLen = std::max(glm::length(tpd), 1e-6f);

	float t0 = tMin, t1 = tMax;
	const glm::vec2 inv(1.0f / (std::fabs(tpd.x) + 1e-9f) * (tpd.x < 0.0f ? -1.0f : 1.0f),
	                     1.0f / (std::fabs(tpd.y) + 1e-9f) * (tpd.y < 0.0f ? -1.0f : 1.0f));
	const glm::vec2 ta = (glm::vec2(0.0f) - tp0) * inv, tb = (glm::vec2(resM1) - tp0) * inv;
	const glm::vec2 tlo = glm::min(ta, tb), thi = glm::max(ta, tb);
	t0 = std::max(t0, std::max(tlo.x, tlo.y));
	t1 = std::min(t1, std::min(thi.x, thi.y));
	const float maxH = pyr.maxHeight;
	if (std::fabs(dL.y) > 1e-9f)
	{
		const float tH = (maxH - oL.y) / dL.y;
		if (dL.y < 0.0f) t0 = std::max(t0, tH); else t1 = std::min(t1, tH);
	}
	else if (oL.y > maxH) return false;
	if (t0 >= t1) return false;

	const glm::vec2 tpS = tp0 + tpd * t0;
	const float     hS  = bilinear(tpS);
	const float     yS  = oL.y + dL.y * t0;
	const float     lift = std::max(0.0f, hS + 0.15f - yS);

	int       lvl = std::min(2, topLvl);
	float     t   = t0;
	glm::vec2 hitTp{};
	bool      hit = false;
	for (int it = 0; it < maxSteps; ++it)
	{
		if (t >= t1) break;
		const glm::vec2 tp = tp0 + tpd * t;
		const float     y  = oL.y + dL.y * t + lift;
		const int cx = static_cast<int>(std::floor(tp.x)) >> lvl, cz = static_cast<int>(std::floor(tp.y)) >> lvl;
		const float m        = texelMax(cx, cz, lvl);
		const float cellTex  = static_cast<float>(1 << lvl);
		const float dt       = 0.5f * cellTex / texLen;
		const float yEnd     = y + dL.y * dt;
		if (std::min(y, yEnd) > m + 0.02f)
		{
			t += dt;
			lvl = std::min(lvl + 1, topLvl);
		}
		else if (lvl > 0)
		{
			lvl -= 1;
		}
		else
		{
			if (y < bilinear(tp) - 0.02f) { outT = t; hitTp = tp; hit = true; break; }
			t += 0.5f / texLen;
		}
	}
	if (!hit) return false;

	const float hx1 = bilinear(hitTp + glm::vec2(1.0f, 0.0f)), hx0 = bilinear(hitTp - glm::vec2(1.0f, 0.0f));
	const float hz1 = bilinear(hitTp + glm::vec2(0.0f, 1.0f)), hz0 = bilinear(hitTp - glm::vec2(0.0f, 1.0f));
	const float dHdx = (hx1 - hx0) * 0.5f * texPerLocal.x;
	const float dHdz = (hz1 - hz0) * 0.5f * texPerLocal.y;
	outNormalL = glm::normalize(glm::vec3(-dHdx, 1.0f, -dHdz));
	return true;
}

} // namespace HE
