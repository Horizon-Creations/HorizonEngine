// CPU side of the auto-landscape entries of the GI landscape table — see
// GiLandscape.h (AUTO landscapes) for what the kernels do with them.
#include "HorizonRendering/GiLandscape.h"

#include <ContentManager/Assets.h>
#include <ContentManager/TextureArrayBuild.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>

#include <algorithm>
#include <cmath>

namespace HE
{

int giAutoLandscapeParams(const MaterialAsset& ma,
                          const std::function<bool(const std::string& name, float& value)>& overrideValue,
                          GiLandscape& out, float puddleScale)
{
	// Live value of a named parameter: the per-entity override, else the
	// asset's slot (an instance carries its parent's full slot list with its
	// own overrides merged in, ContentManager::syncMaterialInstance).
	auto value = [&](const char* name, float& v) -> bool
	{
		if (overrideValue && overrideValue(name, v)) return true;
		for (size_t s = 0; s < ma.graphParamNames.size(); ++s)
			if (ma.graphParamNames[s] == name)
			{
				if (ma.shaderParamData.size() < s * 4 + 1) return false;
				v = ma.shaderParamData[s * 4];
				return true;
			}
		return false;
	};
	float rockSlope, rockBlend, dirtAmt, snowH, snowBlend, snowSlope, pudAmt, pudSlope;
	if (!value(kAutoLandscapeParamRockSlope, rockSlope)       ||
	    !value(kAutoLandscapeParamRockBlend, rockBlend)       ||
	    !value(kAutoLandscapeParamDirtAmount, dirtAmt)        ||
	    !value(kAutoLandscapeParamSnowHeight, snowH)          ||
	    !value(kAutoLandscapeParamSnowBlend, snowBlend)       ||
	    !value(kAutoLandscapeParamSnowMaxSlope, snowSlope)    ||
	    !value(kAutoLandscapeParamPuddleAmount, pudAmt)       ||
	    !value(kAutoLandscapeParamPuddleMaxSlope, pudSlope))
		return -1;

	int slot = -1;
	for (size_t i = 0; i < ma.graphTexturePaths.size(); ++i)
		if (ma.graphTexturePaths[i] == kAutoLandscapeAlbedoArray) { slot = static_cast<int>(i); break; }
	if (slot < 0 && ma.graphTexturePaths.empty() && !ma.graphTextureIds.empty()) slot = 0; // packed: ids only
	if (slot < 0) return -1;

	out.layerCount = kGiLandAuto;
	// Widths floored at the parameters' own minimums, so no smoothstep in the
	// kernels ever divides by zero.
	out.autoSlope = { rockSlope, std::max(rockBlend, 0.01f), std::clamp(dirtAmt, 0.0f, 1.0f),
	                  std::max(pudSlope, 0.002f) };
	out.autoSnow  = { snowH, std::max(snowBlend, 0.1f), snowSlope, 0.0f };
	// Puddle share of the flat ground, after the parameter's own description:
	// Puddle Amount 0.32 ≈ 1/5, 0.5 ≈ half, and nothing below ~0.2 (the noise
	// field seldom dips that low).
	out.autoWet.w = std::clamp((pudAmt * std::clamp(puddleScale, 0.0f, 1.0f) - 0.2f) / 0.6f, 0.0f, 1.0f);
	return slot;
}

bool giAutoLandscapeSliceMeans(const TextureAsset& t, GiLandscape& out)
{
	if (t.layers < 5 || !textureArrayPayloadValid(t)) return false;
	// Smallest level with both sides ≤ 16 (or the last one stored).
	uint32_t level = 0, w = t.width, h = t.height;
	while (level + 1 < t.mipLevels && (w > 16 || h > 16))
	{
		++level;
		w = std::max<uint32_t>(1, w >> 1);
		h = std::max<uint32_t>(1, h >> 1);
	}
	// Level 0 of a mip-less 4k array is still 16M texels: walk a ≤ 16×16 grid.
	const uint32_t sx = std::max<uint32_t>(1, w / 16), sy = std::max<uint32_t>(1, h / 16);
	auto decode = [&](uint8_t b)
	{
		const float c = b / 255.0f;
		if (!t.srgb) return c;
		return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
	};
	glm::vec3 mean[5];
	for (uint32_t s = 0; s < 5; ++s)
	{
		const uint8_t* px = t.data.data() + textureArrayOffset(t.width, t.height, t.mipLevels, s, level);
		glm::vec3 sum(0.0f);
		uint32_t  n = 0;
		for (uint32_t y = 0; y < h; y += sy)
			for (uint32_t x = 0; x < w; x += sx, ++n)
			{
				const uint8_t* p = px + (static_cast<size_t>(y) * w + x) * 4;
				sum += glm::vec3(decode(p[0]), decode(p[1]), decode(p[2]));
			}
		mean[s] = sum / static_cast<float>(std::max<uint32_t>(n, 1));
	}
	for (int i = 0; i < 4; ++i) out.layerColor[i] = glm::vec4(mean[i], 0.0f);
	// A puddle is half wet rim, half standing water (the graph darkens water
	// to 0.35 of the wet ground): 0.5 × (1 + 0.35).
	const glm::vec3 wet = mean[static_cast<int>(AutoLandscapeLayer::WetGround)] * 0.675f;
	out.autoWet = glm::vec4(wet, out.autoWet.w);
	return true;
}

} // namespace HE
