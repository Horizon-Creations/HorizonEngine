#pragma once
#include <ContentManager/Assets.h>
#include <Types/Defines.h>
#include <string>

// "Simple texture-only material": a Surface material that shows ONE albedo texture
// (optionally tinted, with a tiling/offset) and nothing else — no normal map, no
// emissive, no opacity, no vertex displacement, no parameters. Those are the materials the
// export may bake onto the mesh (docs/mesh-texture-bake-2026-11-02.md, decision 4).
//
// Two shapes qualify:
//   * the built-in uber shader (no graph, no custom GLSL): texturePaths[0] × baseColor;
//   * a graph whose Output node has BaseColor driven by a Texture Sample (alone, or
//     Multiplied with a Color/Float constant), the sample's UV unconnected or fed by one
//     UV node (tiling/offset), and EVERY other Output pin unconnected.
//
// Opaque only: a Masked or Translucent material needs the texture's alpha, which a vertex
// colour or a resampled texture cannot carry, and a material instance has no graph of its
// own to read.
namespace HE
{

struct SimpleTextureMaterial
{
	std::string albedoPath;                    // content-relative path of the texture asset
	float       tint[3]     = { 1.0f, 1.0f, 1.0f };   // multiplies the texel (baseColor / the constant)
	float       uvScale[2]  = { 1.0f, 1.0f };         // UV node tiling (1,1 = none)
	float       uvOffset[2] = { 0.0f, 0.0f };         // UV node offset
	float       metallic    = 0.0f;
	float       roughness   = 0.5f;
	bool        doubleSided = false;
	bool        lit         = true;            // the Output node's lit flag (a graph's only)
	bool        fromGraph   = false;           // false = the uber shader

	// True when two simple materials can share ONE merged material: the same shading
	// constants and side-ness. The textures and the UV transform may differ (the bake
	// handles those), the tint only for the resampling bake (it is folded into the texels).
	bool sameShading(const SimpleTextureMaterial& o) const;
	bool sameTint(const SimpleTextureMaterial& o) const;
	// UV transform is the identity: the mesh UVs address the texture directly.
	bool identityUv() const;
};

// Classifies `mat`. On false, `whyNot` (if given) names the first reason in plain words
// ("has a normal map", "blend mode is not opaque"), for the export log. `out` is only
// meaningful when true.
HE_API bool classifySimpleTextureMaterial(const MaterialAsset& mat, SimpleTextureMaterial& out,
                                          std::string* whyNot = nullptr);

} // namespace HE
