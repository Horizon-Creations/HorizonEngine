#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <Types/UUID.h>
#include "ContentManager/Assets.h"

// Cross-card impostor bake (Thema 331, step 4): the far detail level of a foliage
// layer, a card that shows a picture of the plant instead of its geometry.
//
// Why a CPU bake and not a render of the plant: the card is an ordinary lit mesh at
// run time, so its texture has to be the plant's ALBEDO and its cut-out, and every
// renderer path that reads a frame back (RenderSceneImage, the thumbnails) returns
// the lit, tone-mapped, sky-backed picture without an alpha channel — and the one
// path with raw output exists on Metal only. Rasterising the plant's triangles here
// against its own base-colour textures gives the same picture on every backend, with
// a real alpha channel, and runs in a unit test without a GPU.
//
// What it writes, next to the plant's mesh (names fixed, so a second bake replaces
// the first in place and every reference to them survives):
//   <stem>_Impostor.hasset      the atlas (a texture)
//   <stem>_ImpostorMat.hasset   a Masked, double-sided graph material over it
//   <stem>_ImpostorCard.hasset  the card: two crossed quads like the engine's
//                               CrossCard, sized to the plant
namespace Impostor
{

// One material slot of the plant, as the rasteriser needs it.
struct SectionSource
{
    // Base-colour texture, RGBA8, rows BOTTOM-UP (the order TextureAsset stores).
    // Null = a flat colour (`tint`), opaque.
    const uint8_t* rgba   = nullptr;
    int            width  = 0;
    int            height = 0;
    bool           srgb   = true;
    // Multiplies the texture (the material's base colour), linear 0..1.
    float          tint[3] = { 1.0f, 1.0f, 1.0f };
    // Texels with alpha below `cutoff` are not there. False = opaque material.
    bool           alphaTest = true;
    float          cutoff    = 0.5f;
};

struct Settings
{
    int size       = 512;   // edge of ONE view in texels; the atlas is 2 x size by size
    int supersample = 3;    // samples per texel edge: alpha gets supersample^2 + 1 steps
    int dilate     = 8;     // texels the colour is bled into the transparent border
};

struct Baked
{
    std::string          error;       // non-empty: nothing was baked
    int                  width  = 0;  // 2 * size
    int                  height = 0;  // size
    // Atlas, RGBA8, rows bottom-up. Left half: the plant seen along -Z (the card plane
    // whose axis is X), right half: seen along +X (the plane whose axis is Z). Colour is
    // bled into transparent texels so filtering at the border does not darken the edge.
    std::vector<uint8_t> rgba;
    // The card that matches: spans x,z in [-halfWidth, halfWidth], y in [y0, y1].
    float                halfWidth = 0.0f;
    float                y0 = 0.0f, y1 = 0.0f;
    // Share of texels with alpha > 0 per view (a sanity number, logged).
    float                coverage[2] = { 0.0f, 0.0f };

    bool ok() const { return error.empty() && !rgba.empty(); }
};

// Bakes `mesh` (SoA form; a cooked mesh is refused). `sections` is index-parallel to
// the mesh's section table, or holds one entry for a mesh without one.
Baked bake(const StaticMeshAsset& mesh, const std::vector<SectionSource>& sections,
           const Settings& settings = {});

// The card for `baked`: two upright quads crossed at right angles, both faces of each
// (16 vertices, 8 triangles), the same shape as the engine's CrossCard (mesh_gen) but
// sized to the plant. Plane A (axis X) maps the left half of the atlas, plane B (axis Z)
// the right half; v = 1 at the top. One section, no material of its own.
StaticMeshAsset makeCard(float halfWidth, float y0, float y1, bool doubleFaced = true);

// One axis-aligned piece of a card plane, in texels of its atlas half ([x0,x1) by [y0,y1), y up,
// the atlas' own row order). The union of a plane's pieces covers every texel that can pass the
// material's cutoff, so cutting the card to them changes no pixel, only how many fragments the
// card launches: a plant is 1 to 22 % opaque inside its rectangle, and a discarded fragment costs
// the GPU almost what a lit one does (thema 331 step 14: all fragments discarded = same Scene time).
struct CardRect { int x0 = 0, x1 = 0, y0 = 0, y1 = 0; };

// The pieces of plane `view` (0 = the left atlas half, plane A; 1 = plane B), a handful (four
// bands of up to two runs each, equal neighbours joined). One piece covering the whole view
// when the atlas is unusable or the view has nothing to cover.
std::vector<CardRect> cardRects(const Baked& baked, int view);

// The same card as above but each plane trimmed to cardRects: a few quads per plane instead of
// one, the same positions and UVs for every texel they cover.
StaticMeshAsset makeCard(const Baked& baked, bool doubleFaced = true);

struct Result
{
    std::string error;
    std::string meshPath, materialPath, texturePath;   // content-relative
    HE::UUID    mesh, material, texture;
    bool ok() const { return error.empty(); }
};

// Writes the three assets for the plant at `plantPath` (content-relative) under
// `contentRoot`. Existing files are replaced in place, keeping their UUIDs.
Result write(const Baked& baked, const std::string& plantPath, const std::filesystem::path& contentRoot);

// `<folder of plantPath>/<stem>_Impostor<suffix>.hasset`, forward slashes.
std::string pathFor(const std::string& plantPath, const char* suffix);

} // namespace Impostor
