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
//
// What decides a layer (all per pixel, all procedural — no weightmap channel):
//   slope    = 1 − N.y of the GEOMETRIC normal (0 flat, 1 vertical)
//   ground   = Grass, with Dirt in large noise patches and in a belt just below
//              the rock slope (scree)
//   rock     = slope past "Rock Slope", over "Rock Blend" — rock is the bulk of
//              the automatic distribution on any relief
//   snow     = world height past "Snow Height", over "Snow Blend", not on faces
//              steeper than "Snow Max Slope" (rock shows through on cliffs)
//   puddles  = flat ground (slope < "Puddle Max Slope") in the low spots of a
//              world-space noise field (the field's minima are the "hollows"),
//              wet ground around, standing water in the middle; never under snow.
//              The shader has no terrain curvature, so a hollow is a basin of
//              that field — not a dip of the terrain mesh. Measured hollows need
//              a cavity channel (see docs §10, open).
// Every transition is biased by the layers' height maps ("Height Blend"): rock
// texels poke through grass before the slope threshold, water fills the low
// texels of the wet ground first.
//
// Tiling runs in WORLD space (world XZ / tile size), not over the terrain's
// 0..1 UV, so a texel is the same size on a 100 m and on a 4 km landscape.
// Rock, Grass and Dirt are read through texture bombing (hex tiling, one grid
// per layer shared by its albedo/normal/mask, own seed per layer); Snow and Wet
// Ground are read plainly — they are rarely large enough to show tiling, and
// bombing triples the reads (docs §9.4). The static switch "Texture Bombing"
// turns the bombed reads into plain ones at compile time (instance override →
// own permutation; the witness uses it as the negative control).
#pragma once

#include <MaterialGraph/MaterialGraph.h>
#include <Types/Defines.h>
#include <Types/UUID.h>

#include <cstdint>

namespace HE
{
// The engine asset the generator writes. hi 0x412 continues landscape_tex_gen's
// 0x400 block (textures 0x400..0x411).
inline constexpr char kAutoLandscapeMaterialPath[] = "Engine/Materials/M_AutoLandscape.hasset";
inline constexpr UUID kAutoLandscapeMaterialId     = { 0x0000000000000412ULL, 0x0000000000000001ULL };

inline constexpr char kAutoLandscapeAlbedoArray[] = "Engine/Textures/Landscape/T_Landscape_Albedo_Array.hasset";
inline constexpr char kAutoLandscapeNormalArray[] = "Engine/Textures/Landscape/T_Landscape_Normal_Array.hasset";
inline constexpr char kAutoLandscapeMaskArray[]   = "Engine/Textures/Landscape/T_Landscape_Mask_Array.hasset";

// Slice order of the three arrays (landscape_tex_gen kLayers).
enum class AutoLandscapeLayer : int { Grass = 0, Dirt = 1, Rock = 2, Snow = 3, WetGround = 4 };

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
inline constexpr char kAutoLandscapeParamPuddleSize[]     = "Puddle Size";
inline constexpr char kAutoLandscapeParamPuddleMaxSlope[] = "Puddle Max Slope";
inline constexpr char kAutoLandscapeSwitchBombing[]       = "Texture Bombing";
inline constexpr int  kAutoLandscapeParamCount            = 14;

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
    // Node ids of the masks (Float on pin 0), for debug wiring and tests.
    int output = 0, slope = 0, dirtMask = 0, rockMask = 0, snowMask = 0,
        flatMask = 0, wetMask = 0, waterMask = 0;
};

HE_API AutoLandscapeGraph buildAutoLandscapeGraph(AutoLandscapeView view = AutoLandscapeView::Lit);
} // namespace HE
