#include "doctest.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstring>

#include <HorizonRendering/MotionBlurShared.h>
#include "../src/HE_Rendering/src/Backends/D3D_Shared/HlslSources.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

// Camera motion blur (IRenderer::MotionBlurSettings) on D3D11 / D3D12 / Vulkan: the reprojection every velocity pass takes.

namespace
{
// What a velocity pass does with one pixel (GL ndc in, the previous uv out): its motion in pixels on a w x h target.
float velocityPx(const glm::mat4& reproject, const glm::vec2& uv, float ndcZ, float w, float h)
{
    const glm::vec4 prev = reproject * glm::vec4(uv * 2.0f - 1.0f, ndcZ, 1.0f);
    const glm::vec2 prevUV = glm::vec2(prev) / prev.w * 0.5f + 0.5f;
    return glm::length((uv - prevUV) * glm::vec2(w, h));
}
// The editor camera of a tile world: 38 km from the origin, the 250 km far plane (EditorCamera.h).
glm::mat4 viewProjAt(const glm::vec3& eye, float yawDeg)
{
    const float y = glm::radians(yawDeg);
    const glm::vec3 fwd(std::sin(y) * std::cos(-0.2f), std::sin(-0.2f), -std::cos(y) * std::cos(-0.2f));
    const glm::mat4 view = glm::lookAt(eye, eye + fwd, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj = glm::perspectiveRH_NO(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 250000.0f);
    return proj * view;
}
} // namespace

TEST_CASE("motion blur: a still camera 38 km from the origin has no motion; one turned by a degree has, in the right direction")
{
    const glm::vec3 eye(38000.0f, 300.0f, 38000.0f);
    const glm::mat4 vp = viewProjAt(eye, 30.0f);
    const glm::mat4 still = HE::motionBlurReproject(vp, vp);
    float worstDouble = 0.0f, worstFloat = 0.0f;
    for (const float z : { -0.5f, 0.9f, 0.99999f })   // near, mid and far ground (GL ndc z)
        for (const glm::vec2 uv : { glm::vec2(0.1f, 0.2f), glm::vec2(0.5f), glm::vec2(0.93f, 0.81f) })
        {
            worstDouble = std::max(worstDouble, velocityPx(still, uv, z, 1920.0f, 1080.0f));
            worstFloat  = std::max(worstFloat,  velocityPx(vp * glm::inverse(vp), uv, z, 1920.0f, 1080.0f));
        }
    MESSAGE("still camera, worst motion: double ", worstDouble, " px, the float inverse ", worstFloat, " px");
    CHECK(worstDouble < 0.05f);   // the blur pass passes anything under half a pixel through untouched

    // A yaw to the right: the world moves left on screen, so a pixel came from further right (prevUV.x > uv.x).
    const glm::mat4 turned = HE::motionBlurReproject(viewProjAt(eye, 29.0f), vp);
    const glm::vec4 prev = turned * glm::vec4(0.0f, 0.0f, 0.9f, 1.0f);
    const float dxPx = (0.5f - (prev.x / prev.w * 0.5f + 0.5f)) * 1920.0f;
    CHECK(std::abs(dxPx) > 10.0f);
    CHECK(std::abs(prev.y / prev.w) < 0.05f);   // a yaw does not move the centre vertically

    const glm::vec4 p = HE::motionBlurParams(0.5f, 24.0f, 1920, 1080);
    CHECK(p.x == doctest::Approx(0.5f));
    CHECK(p.y == doctest::Approx(36.0f));   // 24 px at 720p, scaled to 1080p
    CHECK(HE::motionBlurParams(-1.0f, -5.0f, 1280, 720).x == 0.0f);
}

#ifdef _WIN32
TEST_CASE("motion blur HLSL (D3D11 / D3D12): the velocity and the smear compile on FXC as ps_5_0")
{
    for (const char* src : { HE::hlsl::kMbVelocityHLSL, HE::hlsl::kMbBlurHLSL })
    {
        Microsoft::WRL::ComPtr<ID3DBlob> out, err;
        const HRESULT hr = D3DCompile(src, std::strlen(src), "mb", nullptr, nullptr, "main", "ps_5_0", 0, 0, &out, &err);
        INFO("FXC: ", err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        CHECK(SUCCEEDED(hr));
    }
}
#endif
