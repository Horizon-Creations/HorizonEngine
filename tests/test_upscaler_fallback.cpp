#include "doctest.h"
#include <Renderer/IRenderer.h>
#include <cmath>

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

TEST_CASE("Upscale render size: display times scale, rounded, clamped to [1/4, 1], never 0")
{
    CHECK(IRenderer::UpscaleRenderSize(1920, 1.0f) == 1920);
    CHECK(IRenderer::UpscaleRenderSize(1920, 0.5f) == 960);
    CHECK(IRenderer::UpscaleRenderSize(1080, 1.0f / 1.5f) == 720);
    CHECK(IRenderer::UpscaleRenderSize(1080, 1.0f / 3.0f) == 360);
    CHECK(IRenderer::UpscaleRenderSize(1920, 2.0f) == 1920);   // no supersampling through an upscaler
    CHECK(IRenderer::UpscaleRenderSize(1920, 0.1f) == 480);    // floor 1/4
    CHECK(IRenderer::UpscaleRenderSize(1920, std::nanf("")) == 480);
    CHECK(IRenderer::UpscaleRenderSize(1, 0.25f) == 1);
}

TEST_CASE("Upscale mip bias: AMD's log2(render / display) - 1")
{
    CHECK(IRenderer::UpscaleMipBias(1920, 1920) == doctest::Approx(-1.0f));
    CHECK(IRenderer::UpscaleMipBias(960, 1920) == doctest::Approx(-2.0f));
    CHECK(IRenderer::UpscaleMipBias(1280, 1920) == doctest::Approx(-1.585f).epsilon(0.001));
    CHECK(IRenderer::UpscaleMipBias(0, 1920) == 0.0f);
}
