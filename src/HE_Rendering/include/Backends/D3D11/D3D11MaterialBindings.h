#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// D3D11 graph-material per-draw bindings the renderer AND he_tests share.
//
// The D3D11 draw path binds a graph material's own resources (heTex0 t2,
// heTexP0..3 t4..t7, their linear-wrap samplers s2/s4..s7) and afterwards
// restores what the built-in scene shader expects. The lighting preamble's
// landscape weightmap (heLandscapeWeights, GLSL binding 14 → t14) was never
// part of that: GL and Metal bind it per draw, D3D11 did not (Thema 57), so a
// wired Landscape Layer Blend read an unbound SRV — zeros — and blended to
// black on D3D11 while the same material painted on every other backend.
//
// The register half is the part that is easy to get wrong. The weightmap's
// SAMPLER is pinned to s0 (MaterialShaderLibrary kHlslMaterialPins: the SM 5.0
// budget is full, s0 is the register heAO leaves dead by reading with
// texelFetch), and s0 is ALSO the built-in scene shader's albedo sampler
// (uSampler, linear WRAP), set once per pass, not per draw. So the material
// draw must set s0 to a linear CLAMP sampler before it draws — otherwise the
// weightmap is sampled with whatever the previous built-in pass left there —
// and must put the pass's sampler BACK afterwards, or every built-in draw that
// follows samples its albedo clamped. Both halves live here, in one place the
// renderer calls and the WARP test in test_material_graph.cpp drives against
// a real (software) device, so a later edit cannot fix one and forget the other.
// ─────────────────────────────────────────────────────────────────────────────
#if defined(_WIN32)
#include <d3d11.h>

namespace HE::d3d11mat
{
// heLandscapeWeights: SRV register t14 (its GLSL binding, SRVs keep their
// number), sampler register s0 (moved below the SM 5.0 cap, shared with the
// dead heAO sampler). Must match kHlslMaterialPins in MaterialShaderLibrary.cpp.
constexpr UINT kWeightmapSrvSlot     = 14;
constexpr UINT kWeightmapSamplerSlot = 0;

// The weightmap's sampler: linear + clamp, like GL's unit 13 / Metal's slot 13
// (m_linearSampler). Clamp, not wrap — a painted weight at the landscape's edge
// must not bleed in from the opposite side.
inline D3D11_SAMPLER_DESC WeightmapSamplerDesc()
{
    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    return sd;
}

// Before a graph-material draw: the object's weightmap on t14 (the chunk's
// landscape paint, or the 1x1 layer-0 default for anything that is not a
// landscape chunk — never null where a Landscape Layer Blend samples) and the
// clamp sampler on s0.
inline void BindLandscapeWeights(ID3D11DeviceContext* ctx,
                                 ID3D11ShaderResourceView* weightmap,
                                 ID3D11SamplerState* linearClamp)
{
    ctx->PSSetShaderResources(kWeightmapSrvSlot, 1, &weightmap);
    ctx->PSSetSamplers(kWeightmapSamplerSlot, 1, &linearClamp);
}

// After the draw: t14 off (the decal pass binds its own texture there and the
// built-in shaders never read it) and s0 back to the sampler the enclosing
// built-in pass set — the register must not leak into the next built-in draw.
inline void RestoreAfterMaterialDraw(ID3D11DeviceContext* ctx, ID3D11SamplerState* builtInS0)
{
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(kWeightmapSrvSlot, 1, &nullSrv);
    ctx->PSSetSamplers(kWeightmapSamplerSlot, 1, &builtInS0);
}
} // namespace HE::d3d11mat

#endif // _WIN32
