// The procedural surface material functions — snow and sand as something ANY material calls.
//
// The auto landscape used to read its snow (and, in the Desert group, its sand) from texture
// arrays. These two functions make the same surfaces from noise, in world space, so they
// have no tile, no hex seam and no texture to ship, and a material that has nothing to do
// with the landscape can use them too: wire a world position, a colour and the knobs, and
// read Albedo / Normal / Roughness / AO / Height back (the five channels of a surface, the
// last one for a height blend). Like every engine function they are FLAT (no nested calls),
// every input declares a default, and they are generated assets (matfn_gen into
// MaterialFunctions/Surface/) — the builders in ProceduralSurfaceFunctions.cpp are the single
// source, the tests and landscape_tex_gen build from them too.
//
//   MF_ProceduralSnow   In:  Position       world XZ in metres (a Vec2: wire world position x/z)
//                            Cover          0..1 how much snow lies here — the weather's Snow
//                                           Cover slider (the Weather node's Snow Cover pin, or
//                                           MF_WeatherSnow's Cover output): deep snow smooths
//                                           and drifts more. Wired by the CALLER, the function
//                                           reads no weather itself
//                            Color          albedo (a Vec3: unwired it is a neutral white-grey)
//                            Grain          0..1 coarse crystal grain: micro relief and speckle
//                            Drift          0..1 wind-blown ridges, long along the wind
//                            Wind Direction Vec2 on the ground plane (any length; zero = +X)
//                            Glitter        0..1 sparkling crystals
//                            Softness       0..1 powder (1) .. crust (0): smooths the relief,
//                                           matte against glossy
//                            Melt           0..1 meltwater: darker, glossier, smoother
//                            Disturbance    0..1 RESERVED for trails (footprints, tracks); the
//                                           input exists so a material can wire it today, the
//                                           function does not read it yet
//                       Out: Albedo, Normal (world space, the geometric normal bent by the
//                            detail), Roughness, AO, Height (0..1, the structure of the surface
//                            for a height blend — grain and drifts only: it does NOT move with
//                            Cover, Softness, Melt or Disturbance, so a mask built from it stays
//                            independent of the weather)
//
//   MF_ProceduralSand   In:  Position, Color (the dry sand), Grain Size (metres), Ripple (0..1
//                            wind ripples across the wind), Ripple Size (wavelength in metres),
//                            Wind Direction, Glitter (mica / quartz), Wetness (0..1: wet sand is
//                            darker and glossier), Disturbance (reserved, as above)
//                       Out: the same five.
//
// Both fade their fine detail out with the distance from the camera (a grain a few
// centimetres wide would only shimmer from far away).
#pragma once

#include <MaterialGraph/MaterialGraph.h>
#include <Types/Defines.h>
#include <Types/UUID.h>

namespace HE
{
// Content-relative paths a calling graph stores in its FunctionCall node.
inline constexpr char kProceduralSnowFunctionPath[] = "Engine/MaterialFunctions/Surface/MF_ProceduralSnow.hasset";
inline constexpr char kProceduralSandFunctionPath[] = "Engine/MaterialFunctions/Surface/MF_ProceduralSand.hasset";
// Fixed UUIDs in landscape_tex_gen's 0x400 block: 0x412 is the auto landscape material, 0x413 /
// 0x414 the weather functions, 0x415..0x41C Forest and Grassland (material + three arrays each).
// 0x41D / 0x41E were the first two of the three ids left below the loose maps (0x420..); 0x41F
// stays free.
inline constexpr UUID kProceduralSnowFunctionId = { 0x000000000000041DULL, 0x0000000000000001ULL };
inline constexpr UUID kProceduralSandFunctionId = { 0x000000000000041EULL, 0x0000000000000001ULL };

// Pin indices a caller wires by (links are stored by index; the order is part of the asset).
enum ProceduralSnowIn : int
{
    kSnowInPosition = 0, kSnowInCover, kSnowInColor, kSnowInGrain, kSnowInDrift, kSnowInWindDirection,
    kSnowInGlitter, kSnowInSoftness, kSnowInMelt, kSnowInDisturbance, kSnowInCount
};
enum ProceduralSandIn : int
{
    kSandInPosition = 0, kSandInColor, kSandInGrainSize, kSandInRipple, kSandInRippleSize,
    kSandInWindDirection, kSandInGlitter, kSandInWetness, kSandInDisturbance, kSandInCount
};
// Both functions answer with the same five outputs.
enum ProceduralSurfaceOut : int
{
    kSurfaceOutAlbedo = 0, kSurfaceOutNormal, kSurfaceOutRoughness, kSurfaceOutAO, kSurfaceOutHeight,
    kSurfaceOutCount
};

HE_API MaterialGraph buildProceduralSnowFunction();
HE_API MaterialGraph buildProceduralSandFunction();
} // namespace HE
