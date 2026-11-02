#pragma once
#include <Hpak/SimpleTextureMaterial.h>
#include <Types/Defines.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The export-time texture bake for a static mesh (docs/mesh-texture-bake-2026-11-02.md):
// the sections of a mesh whose materials are plain PBR texture sets (SimpleTextureMaterial)
// are merged into ONE section with ONE material whose textures are atlases. The mesh draws
// in one call with one set of texture binds instead of one per material.
//
// This file is the algorithm only: loose geometry and decoded images in, new geometry and
// images out. Reading assets and writing the derived ones is the exporter's business.
//
// Two modes, one packer:
//   Merge     lossless. Every source texel is copied unchanged into the atlas and the
//             shared factors stay factors of the merged material. Refused (with a reason)
//             when that is not possible: a UV transform, tiling, differing factors, or
//             a section whose textures have different sizes.
//   Resample  any simple material. Tiling is baked out (the section's UV box is drawn as
//             many times as it repeats, so the atlas can be larger than the sources, up
//             to Options::maxSize per side, then everything shrinks together), factors are
//             folded into the texels, and a role one section lacks is filled with its flat value.
//
// The merged material has four texture groups: base (Base Color), orm (R occlusion,
// G roughness, B metallic), normal and emissive. A group is a CONSTANT when no section
// textures it and every section agrees on its value; otherwise it is an atlas.
namespace HE::MeshBake
{

// One decoded texture, level 0, RGBA8.
struct Image
{
	uint32_t             width  = 0;
	uint32_t             height = 0;
	bool                 srgb   = false;     // sample as sRGB (colour) vs linear (data)
	std::vector<uint8_t> rgba;               // width * height * 4

	bool valid() const { return width > 0 && height > 0 && rgba.size() == size_t(width) * height * 4; }
};

using ImageLookup = std::function<const Image*(const std::string& path)>;

struct SectionInput
{
	uint32_t              indexOffset = 0;
	uint32_t              indexCount  = 0;
	bool                  simple      = false;   // true: `material` is valid and may be baked
	SimpleTextureMaterial material;
};

struct MeshInput
{
	std::vector<float>        vertices;     // xyz per vertex
	std::vector<float>        normals;      // xyz per vertex (may be empty)
	std::vector<float>        uvs;          // uv per vertex (required)
	std::vector<uint32_t>     indices;
	std::vector<SectionInput> sections;     // contiguous, covering `indices`
};

enum class Mode { Merge, Resample };

struct Options
{
	Mode     mode    = Mode::Resample;
	uint32_t maxSize = 4096;      // longest atlas side
	float    minScale = 0.25f;    // refuse rather than shrink the texel density below this
	uint32_t padding = 8;         // texels around each rect, filled from the repeating texture
};

// One group of the merged material.
struct Group
{
	bool  atlas = false;          // false: constant (`value`)
	Image image;                  // the atlas, when atlas
	float value[3] = { 0, 0, 0 }; // the constant, when not
};

struct Result
{
	bool        ok = false;
	std::string why;              // when !ok: the first reason, in plain words

	// The rewritten mesh. Vertices are duplicated per section where sections shared them.
	// Sections the bake did not take (not simple) keep their index range and come AFTER the
	// merged one; `keptSections` maps each to its index in MeshInput::sections.
	std::vector<float>    vertices, normals, uvs;
	std::vector<uint32_t> indices;
	uint32_t              mergedIndexCount = 0;   // the merged section is [0, mergedIndexCount)
	struct Kept { uint32_t inputSection = 0; uint32_t indexOffset = 0; uint32_t indexCount = 0; };
	std::vector<Kept>     keptSections;

	// The merged material, expressed as a SimpleTextureMaterial whose texture paths are
	// "baked:base", "baked:orm", "baked:normal", "baked:emissive" for the atlas groups. The
	// exporter swaps those names for the derived assets it writes.
	SimpleTextureMaterial material;
	Group base, orm, normal, emissive;
	bool  lossless = false;       // every atlas texel is a source texel, untouched
	uint32_t mergedSections = 0;  // how many input sections became the merged one
};

inline constexpr const char* kBakedBase     = "baked:base";
inline constexpr const char* kBakedOrm      = "baked:orm";
inline constexpr const char* kBakedNormal   = "baked:normal";
inline constexpr const char* kBakedEmissive = "baked:emissive";

HE_API Result bake(const MeshInput& mesh, const ImageLookup& images, const Options& options);

} // namespace HE::MeshBake
