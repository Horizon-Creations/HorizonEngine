#pragma once
#include <ContentManager/Assets.h>
#include <Types/Defines.h>
#include <string>
#include <vector>

// "Simple texture material": an opaque Surface material that is a plain PBR texture set and
// nothing else. These are the materials the export may bake onto the mesh
// (docs/mesh-texture-bake-2026-11-02.md, decision 4). What an imported glTF looks like:
// Base Color, Metallic, Roughness and Ambient Occlusion each a constant or a texture
// (a channel of one, times a factor), Specular a constant, an optional Normal Map,
// an optional Emissive, no Opacity, no vertex offset, no displacement, no parameters.
//
// Two shapes qualify:
//   * the built-in uber shader (no graph, no custom GLSL): texturePaths[0] x baseColor
//     plus the material's metallic/roughness constants;
//   * a graph whose Output pins are wired ONLY in the forms below. Anything computed
//     (a Panner, a Fresnel, a parameter, a switch) is refused with a reason.
//
// Opaque only: Masked and Translucent need the texture's alpha, which neither a vertex
// colour nor a resampled texture carries, and a material instance has no graph of its own.
namespace HE
{

// One texture read: which texture and through which UV transform (a UV node's tiling and
// offset; identity when the sample reads the mesh UV as it is).
struct HE_API TextureRef
{
	std::string path;                      // content-relative texture asset; empty = no texture
	float       uvScale[2]  = { 1.0f, 1.0f };
	float       uvOffset[2] = { 0.0f, 0.0f };

	bool empty() const { return path.empty(); }
	bool identityUv() const;
	// Same texture read the same way: two roles that agree can share one bake.
	bool sameSampling(const TextureRef& o) const;
};

// Base Color and Emissive: texture RGB times `factor`; with no texture, `factor` IS the colour.
struct ColorRole
{
	TextureRef tex;
	float      factor[3] = { 1.0f, 1.0f, 1.0f };
};

// Metallic, Roughness, Ambient Occlusion: one channel (0 R .. 3 A) of a texture times
// `factor`; with no texture, `factor` IS the value. For Ambient Occlusion `factor` is the
// strength: ao = 1 + factor * (sample - 1).
struct ScalarRole
{
	TextureRef tex;
	int        channel = 0;
	float      factor  = 1.0f;
};

struct HE_API SimpleTextureMaterial
{
	ColorRole  baseColor;                                   // default: flat white
	ScalarRole metallic  = { {}, 0, 0.0f };
	ScalarRole roughness = { {}, 0, 0.5f };
	ScalarRole ao        = { {}, 0, 1.0f };
	ColorRole  emissive  = { {}, { 0.0f, 0.0f, 0.0f } };    // default: none
	TextureRef normal;                                      // empty = the mesh normal
	float      normalStrength = 1.0f;
	float      specular       = 0.5f;                       // constant; the Output pin's default
	bool       doubleSided = false;
	bool       lit         = true;                          // the Output node's lit flag
	bool       fromGraph   = false;                         // false = the uber shader

	// Every distinct texture this material reads, in role order. Never empty for a
	// material the classifier accepts (a material that reads no texture is nothing to bake).
	std::vector<std::string> texturePaths() const;

	// True when two simple materials can share ONE merged material: the same specular,
	// lit flag and side-ness (the bake cannot fold those into texels). Everything else
	// (textures, UV transforms, factors, which roles exist) the bake resolves.
	bool sameShading(const SimpleTextureMaterial& o) const;
	// True when every factor, constant and the normal strength agree, so the lossless
	// merge may copy texels unchanged; only the resampling bake can fold differing ones in.
	bool sameFactors(const SimpleTextureMaterial& o) const;
};

// Classifies `mat`. On false, `whyNot` (if given) names the first reason in plain words
// ("its graph drives Opacity", "a texture sample has no UV connected"), for the export
// log. `out` is only meaningful when true.
HE_API bool classifySimpleTextureMaterial(const MaterialAsset& mat, SimpleTextureMaterial& out,
                                          std::string* whyNot = nullptr);

} // namespace HE
