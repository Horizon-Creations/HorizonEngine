// The weather material functions — what lets a MATERIAL react to the scene's weather.
//
// The scene's Weather entity carries three sliders for materials (Weather details panel
// ▸ Surface): Puddles and Snow Cover, 0..1 each, and Puddle Size in metres. They travel to the shader in the
// lighting block (heLight.weather z / w; x / y are the wetness and snow amount every
// lit surface already answers to) and a material reads all four through the Weather
// node. The two functions below wrap that node into the thing a surface actually wants:
//
//   MF_WeatherPuddles   In:  Max Water Level   water level of a puddle noise field when
//                                              the Puddles slider is at 1
//                       Out: Water Level       = slider × Max Water Level — wired where a
//                                              puddle field used to take a fixed amount
//                            Puddles           the raw slider
//                            Wetness           the weather's wetness
//                            Size              puddle size in metres (the panel's Puddle
//                                              Size slider, at least 0.5)
//
//   MF_WeatherSnow      In:  Slope             1 − normal.y of the surface (0 flat)
//                            Max Slope         steepest slope that holds snow
//                            Height Bias       −1..1; positive lets snow settle here first
//                       Out: Snow              coverage 0..1 — the slider, thinned out by
//                                              slope and bias, 0 whenever the slider is 0
//                            Cover             the raw slider
//
// Like every engine function they are FLAT (no nested calls), every input declares a
// default, and they are generated assets (matfn_gen) — the builders here are the single
// source, the tests and the landscape generator build from them too.
#pragma once

#include <MaterialGraph/MaterialGraph.h>
#include <Types/Defines.h>
#include <Types/UUID.h>

namespace HE
{
// Content-relative paths a calling graph stores in its FunctionCall node.
inline constexpr char kWeatherPuddlesFunctionPath[] = "Engine/MaterialFunctions/Weather/MF_WeatherPuddles.hasset";
inline constexpr char kWeatherSnowFunctionPath[]    = "Engine/MaterialFunctions/Weather/MF_WeatherSnow.hasset";
// Fixed UUIDs, continuing landscape_tex_gen's 0x400 block (textures 0x400..0x411, the
// auto landscape material 0x412).
inline constexpr UUID kWeatherPuddlesFunctionId = { 0x0000000000000413ULL, 0x0000000000000001ULL };
inline constexpr UUID kWeatherSnowFunctionId    = { 0x0000000000000414ULL, 0x0000000000000001ULL };

HE_API MaterialGraph buildWeatherPuddlesFunction();
HE_API MaterialGraph buildWeatherSnowFunction();

// A loader that answers the two paths above from the builders (and nothing else) — for
// tests and generators that compile a graph calling them without a ContentManager.
HE_API MatFunctionLoader weatherFunctionLoader();
} // namespace HE
