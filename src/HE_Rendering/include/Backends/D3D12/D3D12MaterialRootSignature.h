#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// D3D12 graph-material root signature — the ONE description D3D12Renderer's
// createMaterialResources serialises and he_tests builds a WARP PSO against.
//
// Why a header of its own: CreateGraphicsPipelineState validates every register
// the pixel shader binds against the root signature and rejects the PSO
// (E_INVALIDARG, "not compatible with root signature") when one is missing.
// MaterialShaderLibrary::fragment(HLSL) pins the shared lighting preamble to
// t2/t4..t7/t10..t18/t31..t33 with samplers s1..s15 — ALL of them referenced
// statically, because every gate (heLight.fog.z/w, giProbe.y, ssr.x,
// cloudShadowB.x, csmSplits.w, giParams.z) is a runtime uniform FXC cannot fold.
// s0 is the one register the preamble leaves dead (heAO is read with
// texelFetch, its SamplerState is declared but never used); a Landscape Layer
// Blend's heLandscapeWeights (t14) samples through it, so it is covered too.
// The renderer used to declare only t2/t4..t7/t10..t13 + s2/s4..s7/s10..s13
// (Thema 56): every lit graph material's PSO would have failed on real
// hardware and the draw fallen back to built-in PBR. Keeping the description
// here, shared with the test, means the two cannot drift apart again.
//
// Layout of one per-draw SRV block (kSrvPerDraw consecutive heap slots, the
// root table's order):
//   [0]      heTex0          t2    material base texture
//   [1..4]   heTexP0..3      t4..7 graph Texture Sample nodes
//   [5]      heGIShadow      t10   ray-traced sun visibility mask
//   [6]      heGILocal       t11   ray-traced local-light mask
//   [7]      heCsm           t12   cascade shadow array (Texture2DArray)
//   [8]      heLocalShadow   t13   local point/spot atlas (Texture2DArray)
//   [9]      heLandscapeWeights t14 (landscape domain only; null otherwise)
//   [10]     heSkyEnv        t15   sky cubemap (TextureCube!)
//   [11]     heAO            t16   screen-space AO
//   [12]     heGIIrradiance  t17   DDGI irradiance atlas
//   [13]     heGIVisibility  t18   DDGI visibility atlas
//   [14]     heSSRFwd        t31   forward SSR result
//   [15]     heGIReflFwd     t32   forward GI-reflection result
//   [16]     heCloudShadow   t33   cloud-shadow transmittance
// Slots 9..16 are null views in the template today: the D3D12 fill
// (fillMatLight) leaves their gates at 0, so they are declared, never
// sampled. Wiring the real AO / DDGI atlases through them is the D3D11-parity
// job, not this fix — this header only makes the PSO legal.
// ─────────────────────────────────────────────────────────────────────────────
#if defined(_WIN32)
#include <d3d12.h>

namespace HE::d3d12mat
{
constexpr UINT kSrvPerDraw          = 17;
constexpr UINT kSlotTex0            = 0;  // t2
constexpr UINT kSlotTexP0           = 1;  // t4..t7 → slots 1..4
constexpr UINT kSlotGIShadow        = 5;  // t10
constexpr UINT kSlotGILocal         = 6;  // t11
constexpr UINT kSlotCsm             = 7;  // t12
constexpr UINT kSlotLocalShadow     = 8;  // t13
constexpr UINT kSlotLandscapeWeights= 9;  // t14
constexpr UINT kSlotSkyEnv          = 10; // t15 (cube)
constexpr UINT kSlotAO              = 11; // t16
constexpr UINT kSlotGIIrradiance    = 12; // t17
constexpr UINT kSlotGIVisibility    = 13; // t18
constexpr UINT kSlotSSRFwd          = 14; // t31
constexpr UINT kSlotGIReflFwd       = 15; // t32
constexpr UINT kSlotCloudShadow     = 16; // t33

// Root parameter indices (SetGraphicsRootConstantBufferView / DescriptorTable).
constexpr UINT kRootLightCB   = 0; // b0 HeLighting (FS)
constexpr UINT kRootObjectCB  = 1; // b1 U (VS)
constexpr UINT kRootParamsCB  = 2; // b3 HeParams (FS)
constexpr UINT kRootLightCBVS = 3; // b8 HeLighting (WPO VS)
constexpr UINT kRootParamsCBVS= 4; // b9 HeParams   (WPO VS)
constexpr UINT kRootSrvTable  = 5; // the per-draw SRV block

constexpr UINT kParamCount    = 6;
constexpr UINT kRangeCount    = 7;
constexpr UINT kSamplerCount  = 16;
// What the signature covered BEFORE Thema 56 (t2, t4..t7, t10..t11, t12, t13 /
// s2, s4..s7, s10..s13): the first five ranges and nine samplers. he_tests
// truncates the builder to these counts as the negative control — a PSO built
// against that signature must be rejected, or the whole premise is wrong.
constexpr UINT kLegacyRangeCount   = 5;
constexpr UINT kLegacySamplerCount = 9;

// Storage the D3D12_ROOT_SIGNATURE_DESC points into: keep it alive until
// D3D12SerializeRootSignature has run.
struct MaterialRootSignature
{
    D3D12_ROOT_PARAMETER      params[kParamCount]{};
    D3D12_DESCRIPTOR_RANGE    ranges[kRangeCount]{};
    D3D12_STATIC_SAMPLER_DESC samplers[kSamplerCount]{};
    D3D12_ROOT_SIGNATURE_DESC desc{};
};

// Fills `out`. rangeCount / samplerCount default to the full signature; the
// legacy counts reproduce the pre-fix one (test negative control only).
inline void DescribeMaterialRootSignature(MaterialRootSignature& out,
                                          UINT rangeCount   = kRangeCount,
                                          UINT samplerCount = kSamplerCount)
{
    out = MaterialRootSignature{};
    auto cbv = [&](UINT i, UINT reg) {
        out.params[i].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
        out.params[i].Descriptor       = { reg, 0 };
        out.params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    };
    cbv(kRootLightCB,    0); // b0 HeLighting (FS)
    cbv(kRootObjectCB,   1); // b1 U (VS)
    cbv(kRootParamsCB,   3); // b3 HeParams (FS)
    cbv(kRootLightCBVS,  8); // b8 HeLighting (WPO VS)
    cbv(kRootParamsCBVS, 9); // b9 HeParams   (WPO VS)

    // Registers match SPIRV-Cross HLSL (binding → register, shader_model=50,
    // the pins in MaterialShaderLibrary::fragment). t3 is intentionally skipped
    // (SPIRV-Cross leaves it unused for the mesh path); t8/t9 belong to the WPO
    // custom-vertex UBOs. Two gaps in the block (t14..t18 → slots 9..13,
    // t31..t33 → slots 14..16) so a range stays one contiguous register run.
    auto srv = [&](UINT i, UINT baseReg, UINT count, UINT slot) {
        out.ranges[i].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        out.ranges[i].NumDescriptors                    = count;
        out.ranges[i].BaseShaderRegister                = baseReg;
        out.ranges[i].RegisterSpace                     = 0;
        out.ranges[i].OffsetInDescriptorsFromTableStart = slot;
    };
    srv(0,  2, 1, kSlotTex0);             // t2       heTex0
    srv(1,  4, 4, kSlotTexP0);            // t4..t7   heTexP0..3
    srv(2, 10, 2, kSlotGIShadow);         // t10..t11 GI masks
    srv(3, 12, 1, kSlotCsm);              // t12      heCsm (Texture2DArray)
    srv(4, 13, 1, kSlotLocalShadow);      // t13      heLocalShadow (Texture2DArray)
    srv(5, 14, 5, kSlotLandscapeWeights); // t14..t18 landscape weights, sky cube, AO, DDGI atlases
    srv(6, 31, 3, kSlotSSRFwd);           // t31..t33 forward SSR / GI-refl, cloud shadow

    out.params[kRootSrvTable].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    out.params[kRootSrvTable].DescriptorTable.NumDescriptorRanges = rangeCount;
    out.params[kRootSrvTable].DescriptorTable.pDescriptorRanges   = out.ranges;
    out.params[kRootSrvTable].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static samplers, one per pinned sampler register. The first nine are the
    // pre-fix set: s2 + s4..s7 linear-wrap (tiling material textures), s10/s11
    // linear-clamp (screen-space GI masks must not wrap at the viewport edge),
    // s12/s13 POINT-clamp (heCsmShadow's / heLocalShadowFactor's PCF taps
    // compare single texels, like the built-in scene shader's s0). Then the
    // seven the preamble pins below s16 (MaterialShaderLibrary kHlslMaterialPins):
    // s0 heLandscapeWeights linear-clamp (a [0,1] weightmap; heAO's dead
    // SamplerState sits on the same register and is never used — texelFetch),
    // s1/s3 DDGI atlases and s8/s9 forward SSR / GI-refl linear-clamp (built-in
    // s3), s14 heCloudShadow linear-clamp (Metal's constexpr linear sampler),
    // s15 heSkyEnv linear-clamp (a cube; the address mode is moot).
    struct S { UINT reg; D3D12_FILTER filter; D3D12_TEXTURE_ADDRESS_MODE addr; };
    static constexpr S kSamplers[kSamplerCount] = {
        {  2, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP  },
        {  4, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP  },
        {  5, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP  },
        {  6, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP  },
        {  7, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP  },
        { 10, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP },
        { 11, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP },
        { 12, D3D12_FILTER_MIN_MAG_MIP_POINT,  D3D12_TEXTURE_ADDRESS_MODE_CLAMP },
        { 13, D3D12_FILTER_MIN_MAG_MIP_POINT,  D3D12_TEXTURE_ADDRESS_MODE_CLAMP },
        // ── added for the preamble's moved samplers (Thema 56) ──
        {  0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP }, // heLandscapeWeights
        {  1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP }, // heGIIrradiance
        {  3, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP }, // heGIVisibility
        {  8, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP }, // heSSRFwd
        {  9, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP }, // heGIReflFwd
        { 14, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP }, // heCloudShadow
        { 15, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP }, // heSkyEnv
    };
    for (UINT i = 0; i < kSamplerCount; ++i)
    {
        D3D12_STATIC_SAMPLER_DESC& s = out.samplers[i];
        s.Filter           = kSamplers[i].filter;
        s.AddressU = s.AddressV = s.AddressW = kSamplers[i].addr;
        s.ShaderRegister   = kSamplers[i].reg;
        s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        s.MaxLOD           = D3D12_FLOAT32_MAX;
    }

    out.desc.NumParameters     = kParamCount;
    out.desc.pParameters       = out.params;
    out.desc.NumStaticSamplers = samplerCount;
    out.desc.pStaticSamplers   = out.samplers;
    out.desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
}
} // namespace HE::d3d12mat
#endif // _WIN32
