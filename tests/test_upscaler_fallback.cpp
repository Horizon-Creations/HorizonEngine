#include "doctest.h"
#include <Renderer/IRenderer.h>

// AAMethod::FSR (docs/upscalers-fsr-dlss-xess-plan-2026-11-27.md): the one fallback chain every backend uses.
TEST_CASE("AA fallback: FSR without the FSR runtime becomes MetalFX on Metal, else TAA, else SMAA; out of range is FXAA")
{
    using HE::AAMethod;
    IRenderer::Capabilities caps;
    caps.supportsTemporalAA = true;
    caps.supportsFSR = true;
    CHECK(IRenderer::ResolveAAMethod(int(AAMethod::FSR), caps) == AAMethod::FSR);
    caps.supportsFSR = false;
    CHECK(IRenderer::ResolveAAMethod(int(AAMethod::FSR), caps) == AAMethod::TAA);
    caps.supportsMetalFX = true;
    CHECK(IRenderer::ResolveAAMethod(int(AAMethod::FSR), caps) == AAMethod::MetalFX);
    caps.supportsMetalFX = false;
    caps.supportsTemporalAA = false;
    CHECK(IRenderer::ResolveAAMethod(int(AAMethod::FSR), caps) == AAMethod::SMAA);
    CHECK(IRenderer::ResolveAAMethod(int(AAMethod::XeSS) + 1, caps) == AAMethod::FXAA);
    // DLSS / XeSS without their runtime: FSR where it runs, else FSR's own chain.
    caps.supportsTemporalAA = true;
    caps.supportsFSR = true;
    CHECK(IRenderer::ResolveAAMethod(int(AAMethod::DLSS), caps) == AAMethod::FSR);
    CHECK(IRenderer::ResolveAAMethod(int(AAMethod::XeSS), caps) == AAMethod::FSR);
    caps.supportsFSR = false;
    CHECK(IRenderer::ResolveAAMethod(int(AAMethod::DLSS), caps) == AAMethod::TAA);
    caps.supportsDLSS = caps.supportsXeSS = true;
    CHECK(IRenderer::ResolveAAMethod(int(AAMethod::DLSS), caps) == AAMethod::DLSS);
    CHECK(IRenderer::ResolveAAMethod(int(AAMethod::XeSS), caps) == AAMethod::XeSS);
    CHECK(IRenderer::ResolveAAMethod(-1, caps) == AAMethod::FXAA);
}
