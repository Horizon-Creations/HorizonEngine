// Auto landscape material (Thema 158, Schritt 5) — the engine's ready-made terrain
// material: rock on the slopes, grass and dirt on the flats, snow above a height,
// puddles in the low spots of flat ground, texture bombing against tiling.
//
// It is an ordinary MATERIAL GRAPH built from the standard node library — no
// node of its own, no backend code. That is the point: the same codegen runs on
// all five backends, the graph opens in the material editor like any other, and
// a Material Instance tunes it through its exposed parameters. The builder below
// is the single source; the generator (landscape_tex_gen --material) writes it as
// the engine asset kAutoLandscapeMaterialPath, the headless witness and the tests
// build it from here too.
//
// Textures: the three engine arrays of docs/auto-landscape-material-textures.md
// §8 (T_Landscape_{Albedo,Normal,Mask}_Array, slices Grass, Dirt, Rock, Snow,
// WetGround; Mask = R AO, G roughness, B height) in heTexP0..2. heTexP3 stays free.
// The material reads slices 0..3; slice 4 (WetGround) stays in the arrays but is
// not read — puddles are an overlay, not a layer (see below).
//
// What decides a layer — the AUTOMATIC rules (all per pixel, all procedural):
//   slope    = 1 − N.y of the GEOMETRIC normal (0 flat, 1 vertical)
//   ground   = Grass, with Dirt in large noise patches and in a belt just below
//              the rock slope (scree)
//   rock     = slope past "Rock Slope", over "Rock Blend" — rock is the bulk of
//              the automatic distribution on any relief
//   snow     = world height past "Snow Height", over "Snow Blend", not on faces
//              steeper than "Snow Max Slope" (rock shows through on cliffs)
//   puddles  = flat ground (slope < "Puddle Max Slope") in the low spots of a
//              world-space noise field (the field's minima are the "hollows"),
//              a wet rim around, standing water in the middle; never under snow.
//              An OVERLAY on the ground below, no texture layer: the rim darkens
//              and smooths what lies there, the water darkens it further and
//              turns mirror-smooth and level (its normal points straight up).
//              The slope does not multiply the water away: it lowers the water
//              level in the noise field, so a puddle shrinks into the deepest part
//              of its hollow as the ground tilts and ends on a noise contour, not
//              on a slope contour. The damp film around it fades over a wider
//              slope band (up to twice "Puddle Max Slope"), so water, damp
//              ground and dry ground are a gradient and not one line.
//              The shader has no terrain curvature, so a hollow is a basin of
//              that field — not a dip of the terrain mesh. Measured hollows need
//              a cavity channel (see docs §10, open).
//
// PAINTING on top of the automatic rules: the material declares six paint layers
// (kAutoLandscapePaintLayerNames) and the Landscape paint tool lists them.
//   Auto     what an unpainted terrain reads (the engine's 1x1 (1,0,0,0) default
//            weightmap) — the automatic rules above, unchanged. Paint it back to
//            hand a spot over to the rules again.
//   Grass / Dirt / Rock / Snow   that texture, as painted
//   Puddles  the strength of the water overlay — a painted puddle is water on any
//            slope; the automatic puddles keep to flat ground
// A texel's weights sum to 1, so the surface is Auto x automatic + Σ layer x
// texture, and the automatic puddles fade out by what has been painted over them.
// Rock stays mainly automatic: it is placed by slope with no paint needed, painting
// Rock only adds to it.
//
// Every transition is biased by the layers' height maps ("Height Blend"): rock
// texels poke through grass before the slope threshold, water fills the low
// texels of the ground below first.
//
// Tiling runs in WORLD space (world XZ / tile size), not over the terrain's
// 0..1 UV, so a texel is the same size on a 100 m and on a 4 km landscape.
// Rock, Grass, Dirt and Snow are read through texture bombing (hex tiling, one grid
// per layer shared by its albedo/normal/mask, own seed per layer). Snow used to be
// read plainly to save the reads (docs §9.4) — on a painted or high field of it the
// single tile showed as a grid of identical marks. The static switch "Texture
// Bombing" turns the bombed reads into plain ones at compile time (instance
// override → own permutation; the witness uses it as the negative control).
#pragma once

#include <MaterialGraph/MaterialGraph.h>
#include <Types/Defines.h>
#include <Types/UUID.h>

#include <cstdint>
#include <string_view>

namespace HE
{
// The engine asset the generator writes. hi 0x412 continues landscape_tex_gen's
// 0x400 block (textures 0x400..0x411).
inline constexpr char kAutoLandscapeMaterialPath[] = "Engine/Materials/M_AutoLandscape.hasset";
inline constexpr UUID kAutoLandscapeMaterialId     = { 0x0000000000000412ULL, 0x0000000000000001ULL };

inline constexpr char kAutoLandscapeAlbedoArray[] = "Engine/Textures/Landscape/T_Landscape_Albedo_Array.hasset";
inline constexpr char kAutoLandscapeNormalArray[] = "Engine/Textures/Landscape/T_Landscape_Normal_Array.hasset";
inline constexpr char kAutoLandscapeMaskArray[]   = "Engine/Textures/Landscape/T_Landscape_Mask_Array.hasset";

// The paint layers, in weightmap-channel order (a Landscape Layer Blend node's
// names, '\n'-separated → MaterialAsset::graphLayerNames → the Landscape tool's
// layer list). Auto is layer 0 on purpose: an unpainted landscape resolves to it.
inline constexpr char kAutoLandscapePaintLayerNames[] = "Auto\nGrass\nDirt\nRock\nSnow\nPuddles";
enum class AutoLandscapePaintLayer : int { Auto = 0, Grass = 1, Dirt = 2, Rock = 3, Snow = 4, Puddles = 5 };
inline constexpr int kAutoLandscapePaintLayerCount = 6;

// Slice order of the three arrays (landscape_tex_gen kLayers). WetGround is slice
// 4 of the arrays only; the material does not read it (puddles are an overlay).
// It stays so the array layout and its texture ids (0x40F..0x411) do not move.
enum class AutoLandscapeLayer : int { Grass = 0, Dirt = 1, Rock = 2, Snow = 3, WetGround = 4 };

// Variants per layer (Thema 180, Schritt 3). A "role" is one of the four painted layers (Grass, Dirt,
// Rock, Snow; "texture group" below is the terrain theme and a different thing). The three arrays hold
// kAutoLandscapeRoleCount * V + 1
// slices: role g's variant v is slice g * V + v (g = Grass..Snow), the unread WetGround slice
// comes last. V is not stored anywhere: the shader takes depth / 4, so the same material renders
// the 5-slice placeholder arrays (V = 1) and a 13-slice set (V = 3) alike.
inline constexpr int kAutoLandscapeRoleCount = 4;
inline constexpr int autoLandscapeArrayDepth(int variants) { return kAutoLandscapeRoleCount * variants + 1; }
inline constexpr int autoLandscapeVariantCount(int depth)
{ return depth / kAutoLandscapeRoleCount > 1 ? depth / kAutoLandscapeRoleCount : 1; }
// The variant a hex draws: the low 8 bits of its hash's first word, scaled to 0..V-1 in integers
// (the shader's `((h.x & 255u) * uint(V)) >> 8u`, which every backend computes bit for bit).
inline constexpr int autoLandscapeVariantOf(uint32_t hashX, int variants)
{ return static_cast<int>(((hashX & 255u) * static_cast<uint32_t>(variants)) >> 8u); }
// Slice of layer `group`'s variant `variant` in an array `depth` slices deep; clamped into the
// array like the shader does, so a shorter array can never read outside itself.
inline constexpr int autoLandscapeSliceOf(int group, int variant, int depth)
{
    const int s = group * autoLandscapeVariantCount(depth) + variant;
    return s < 0 ? 0 : (s > depth - 1 ? depth - 1 : s);
}

// The inverse, for whoever assembles the arrays: which (layer, variant) slice `slice` holds.
// Layers are AutoLandscapeLayer values; the last slice (4 * V) is WetGround, variant 0.
inline constexpr void autoLandscapeSliceSource(int slice, int variants, int& layer, int& variant)
{
    if (slice >= kAutoLandscapeRoleCount * variants) { layer = kAutoLandscapeRoleCount; variant = 0; return; }
    layer = slice / variants;
    variant = slice % variants;
}

// ── Texture groups (Thema 233) ────────────────────────────────────────────────
// A texture group is a terrain THEME: one landscape material with its own three arrays, chosen per
// terrain through MaterialComponent::materialAssetId (the material id IS the group). Every group is
// the same graph skeleton — roles, slice layout (4 * V + 1), the Groups pin, the 13 parameters and the
// six paint-layer channels do not change; a group differs by the files its roles read, the names it
// gives the paint layers, and the default values of a few parameters. Mountains is the original
// material, byte for byte (the members above are its row).
enum class AutoLandscapeGroupId : int { Mountains = 0, Forest = 1, Grassland = 2 };
inline constexpr int kAutoLandscapeGroupCount = 3;

struct AutoLandscapeGroup
{
    AutoLandscapeGroupId id;
    const char* name;
    UUID        materialId;
    const char* materialPath;
    const char* arrayPaths[3];      // Albedo, Normal, Mask: the heTexP0..2 order of the graph
    const char* paintLayerNames;    // '\n'-separated, kAutoLandscapePaintLayerNames' channel order
    const char* roleLabels[4];      // the group's name for role Grass, Dirt, Rock, Snow
    // The default value of the parameters a group tunes (the others keep the Mountains value).
    float rockSlope, dirtAmount, snowHeight;
};

// Ids, in landscape_tex_gen's 0x400 block: 0x412 is Mountains, 0x413 / 0x414 are the weather
// functions (WeatherMaterialFunctions.h — the ContentManager keeps ONE entry per UUID, so a clash
// loads the group's material as a function), a group takes its material id and the three ids after
// it for its arrays (0x415..0x418 Forest, 0x419..0x41C Grassland). 0x41D..0x41F are free.
inline constexpr AutoLandscapeGroup kAutoLandscapeGroups[kAutoLandscapeGroupCount] = {
    { AutoLandscapeGroupId::Mountains, "Mountains",
      kAutoLandscapeMaterialId, kAutoLandscapeMaterialPath,
      { kAutoLandscapeAlbedoArray, kAutoLandscapeNormalArray, kAutoLandscapeMaskArray },
      kAutoLandscapePaintLayerNames, { "Grass", "Dirt", "Rock", "Snow" },
      0.12f, 0.35f, 60.0f },
    { AutoLandscapeGroupId::Forest, "Forest",
      { 0x0000000000000415ULL, 0x0000000000000001ULL }, "Engine/Materials/M_AutoLandscape_Forest.hasset",
      { "Engine/Textures/Landscape/T_Landscape_Forest_Albedo_Array.hasset",
        "Engine/Textures/Landscape/T_Landscape_Forest_Normal_Array.hasset",
        "Engine/Textures/Landscape/T_Landscape_Forest_Mask_Array.hasset" },
      "Auto\nForest Floor\nRoots\nMossy Rock\nSnow\nPuddles",
      { "Forest Floor", "Roots", "Mossy Rock", "Snow" },
      0.10f, 0.50f, 2000.0f },
    { AutoLandscapeGroupId::Grassland, "Grassland",
      { 0x0000000000000419ULL, 0x0000000000000001ULL }, "Engine/Materials/M_AutoLandscape_Grassland.hasset",
      { "Engine/Textures/Landscape/T_Landscape_Grassland_Albedo_Array.hasset",
        "Engine/Textures/Landscape/T_Landscape_Grassland_Normal_Array.hasset",
        "Engine/Textures/Landscape/T_Landscape_Grassland_Mask_Array.hasset" },
      "Auto\nMeadow\nBare Soil\nStones\nSnow\nPuddles",
      { "Meadow", "Bare Soil", "Stones", "Snow" },
      0.20f, 0.20f, 2000.0f },
};

inline constexpr const AutoLandscapeGroup& autoLandscapeGroup(AutoLandscapeGroupId id)
{ return kAutoLandscapeGroups[static_cast<int>(id)]; }

// The group whose material is `id`, or -1 (a custom landscape material, a scene saved before groups
// that names none, ...). A terrain bound to Mountains' id is the Mountains group like it always was.
inline constexpr int autoLandscapeGroupOfMaterial(const UUID& id)
{
    for (int i = 0; i < kAutoLandscapeGroupCount; ++i)
        if (kAutoLandscapeGroups[i].materialId == id) return i;
    return -1;
}

// Which group (and map: 0 Albedo, 1 Normal, 2 Mask) a texture path is an array of; -1 if none.
// `path` is compared the way graphTexturePaths store it ("Engine/Textures/...").
inline int autoLandscapeGroupOfArray(const char* path, int* map = nullptr)
{
    if (!path) return -1;
    for (int i = 0; i < kAutoLandscapeGroupCount; ++i)
        for (int m = 0; m < 3; ++m)
            if (std::string_view(path) == kAutoLandscapeGroups[i].arrayPaths[m])
            {
                if (map) *map = m;
                return i;
            }
    return -1;
}

// Exposed parameter names (HeParams slots, Material Instance overrides) and the
// static switch. Kept here so the witness and the tests cannot drift from the graph.
inline constexpr char kAutoLandscapeParamGroundTile[]     = "Ground Tile Size";
inline constexpr char kAutoLandscapeParamRockTile[]       = "Rock Tile Size";
inline constexpr char kAutoLandscapeParamBombCell[]       = "Bombing Cell";
inline constexpr char kAutoLandscapeParamRockSlope[]      = "Rock Slope";
inline constexpr char kAutoLandscapeParamRockBlend[]      = "Rock Blend";
inline constexpr char kAutoLandscapeParamHeightBlend[]    = "Height Blend";
inline constexpr char kAutoLandscapeParamDirtAmount[]     = "Dirt Amount";
inline constexpr char kAutoLandscapeParamDirtSize[]       = "Dirt Patch Size";
inline constexpr char kAutoLandscapeParamSnowHeight[]     = "Snow Height";
inline constexpr char kAutoLandscapeParamSnowBlend[]      = "Snow Blend";
inline constexpr char kAutoLandscapeParamSnowMaxSlope[]   = "Snow Max Slope";
inline constexpr char kAutoLandscapeParamPuddleAmount[]   = "Puddle Amount";
inline constexpr char kAutoLandscapeParamPuddleMaxSlope[] = "Puddle Max Slope";
inline constexpr char kAutoLandscapeSwitchBombing[]       = "Texture Bombing";
inline constexpr int  kAutoLandscapeParamCount            = 13;   // "Puddle Size" moved to the Weather panel

// What the Output node shows. Lit is the material. The mask views are UNLIT
// debug views over the very same mask nodes (the witness's numeric oracle):
//   MasksRockSnowWater  R = rock, G = snow, B = standing water
//   MasksDirtWet        R = dirt, G = wet ground (puddle rim + water), B = flat
//   Normal              the final world normal × 0.5 + 0.5 (the Normal pin's input)
//   Surface             R = AO, G = roughness, B = 0 (the AO / Roughness pins' inputs)
enum class AutoLandscapeView : uint8_t { Lit, MasksRockSnowWater, MasksDirtWet, Normal, Surface };

struct AutoLandscapeGraph
{
    MaterialGraph graph;
    // Node ids of the AUTOMATIC masks (Float on pin 0), for debug wiring and tests —
    // the painted weights are not in them (the witness reads them on an unpainted
    // terrain, where Auto = 1 and painted = 0, so they are also what is drawn there).
    int output = 0, slope = 0, dirtMask = 0, rockMask = 0, snowMask = 0,
        flatMask = 0, wetMask = 0, waterMask = 0;
};

// `group` picks the three arrays the roles read, the paint-layer names and the defaults of the tuned
// parameters; the default (Mountains) builds exactly the graph this material always had.
HE_API AutoLandscapeGraph buildAutoLandscapeGraph(AutoLandscapeView view = AutoLandscapeView::Lit,
                                                  AutoLandscapeGroupId group = AutoLandscapeGroupId::Mountains);
} // namespace HE
