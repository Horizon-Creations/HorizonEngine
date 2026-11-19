#pragma once
// The size of a material's exposed-parameter block (HeParams), in one light header every backend can include.

namespace HE
{
// Exposed parameters a single material graph may declare — the length of the HeParams UBO array
// (`uniform HeParams { vec4 v[kMatMaxParams]; }`, emitted by generateFragment and mirrored in
// MaterialShaderLibrary's WPO preamble). A graph with MORE distinct parameter names bakes the surplus
// ones as literal constants instead of indexing the UBO: the layout used to be clamped only AFTER
// emission, so the generated shader kept reading heParams.v[kMatMaxParams] and up — out of bounds in the
// declared array. Raised from 16 to 32 (the Desert landscape material had used all 16).
inline constexpr int kMatMaxParams = 32;
// The HeParams block every backend uploads per draw: kMatMaxParams vec4 as floats / bytes. One place, so a
// renderer never hard-codes the size again (it was 16 vec4 = 64 floats = 256 B in five backends). The
// per-draw rings of Vulkan and D3D12 use kMatParamBytes as their stride (a multiple of the 256 B alignment).
inline constexpr int kMatParamFloats = kMatMaxParams * 4;
inline constexpr int kMatParamBytes  = kMatParamFloats * 4;
static_assert(kMatParamBytes % 256 == 0, "the Vulkan / D3D12 rings need 256-byte aligned slots");
} // namespace HE
