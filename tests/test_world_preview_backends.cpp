#include "doctest.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <HorizonRendering/ClipSpace.h>
#include <HorizonRendering/WorldPreviewFrame.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include "../src/HE_Rendering/src/Backends/D3D_Shared/HlslSources.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

// ═══ Thema 78, Schritt 4: RenderWorldPreview on D3D11 / D3D12 / Vulkan ═══
// The draw paths themselves need a GPU and a window; what can drift without one
// is checked here:
//   * the depth convention. Those three backends compile with
//     GLM_FORCE_DEPTH_ZERO_TO_ONE, the extractor and the editor do not, so the
//     preview reads the convention off the matrix (HE::toNegOneToOneDepth)
//     instead of assuming one. Applied to the wrong kind of matrix it would
//     halve the depth range or push the near half of the scene out of clip.
//   * the preview pixel shaders the two D3D backends compile at first use —
//     through the real FXC on Windows, both entry points and the D3D12 register
//     variant, so a typo is a red test here instead of a "no 3D preview on
//     this backend" line in somebody's editor.

namespace
{
float ndcZ(const glm::mat4& proj, float viewZ)
{
	const glm::vec4 c = proj * glm::vec4(0.0f, 0.0f, viewZ, 1.0f);
	return c.z / c.w;
}

bool nearlyEqual(const glm::mat4& a, const glm::mat4& b)
{
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			if (std::abs(a[c][r] - b[c][r]) > 1e-4f * std::max(1.0f, std::abs(b[c][r])))
				return false;
	return true;
}
} // namespace

TEST_CASE("toNegOneToOneDepth leaves a GL projection alone and undoes a zero-to-one one")
{
	const float n = 0.1f, f = 5000.0f;
	// This TU is built WITHOUT GLM_FORCE_DEPTH_ZERO_TO_ONE: glm gives GL depth.
	const glm::mat4 glPersp = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, n, f);
	REQUIRE(ndcZ(glPersp, -n) == doctest::Approx(-1.0f));
	// What the same call yields in a zero-to-one translation unit.
	const glm::mat4 zoPersp = HE::kZeroToOneDepthClipFix * glPersp;
	REQUIRE(ndcZ(zoPersp, -n) == doctest::Approx(0.0f).epsilon(1e-4));

	CHECK(nearlyEqual(HE::toNegOneToOneDepth(glPersp, -n), glPersp));
	CHECK(nearlyEqual(HE::toNegOneToOneDepth(zoPersp, -n), glPersp));
	// Idempotent: a converted matrix is GL and stays as it is.
	CHECK(nearlyEqual(HE::toNegOneToOneDepth(HE::toNegOneToOneDepth(zoPersp, -n), -n), glPersp));

	// The far plane lands where each backend expects it after its own fix.
	const glm::mat4 d3d = HE::kD3DClipFix * HE::toNegOneToOneDepth(zoPersp, -n);
	CHECK(ndcZ(d3d, -n) == doctest::Approx(0.0f).epsilon(1e-4));
	CHECK(ndcZ(d3d, -f) == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("toNegOneToOneDepth handles the orthographic preview camera (near plane behind it)")
{
	// worldPreviewProjection / the extractor put an ortho view's near plane a
	// whole far distance BEHIND the camera, so its view-space z is +far.
	const float f = 5000.0f, oh = 8.0f, aspect = 2.0f;
	const glm::mat4 glOrtho = glm::ortho(-aspect * oh, aspect * oh, -oh, oh, -f, f);
	REQUIRE(ndcZ(glOrtho, f) == doctest::Approx(-1.0f));
	const glm::mat4 zoOrtho = HE::kZeroToOneDepthClipFix * glOrtho;

	CHECK(nearlyEqual(HE::toNegOneToOneDepth(glOrtho, f), glOrtho));
	CHECK(nearlyEqual(HE::toNegOneToOneDepth(zoOrtho, f), glOrtho));

	// Vulkan: y flipped, depth 0..1, from either input.
	const glm::mat4 vk = HE::kVulkanClipFix * HE::toNegOneToOneDepth(zoOrtho, f);
	const glm::vec4 up = vk * glm::vec4(0.0f, oh, 0.0f, 1.0f);
	CHECK(up.y / up.w == doctest::Approx(-1.0f));
	CHECK(ndcZ(vk, f) == doctest::Approx(0.0f).epsilon(1e-4));
	CHECK(ndcZ(vk, -f) == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("buildWorldPreviewFrame reports exactly the matrix the editor rebuilds for its gizmos")
{
	// The panel draws gizmos, colliders and its pick ray with
	// worldPreviewProjection(camera, aspect) * view, built in ITS translation
	// unit (GL depth). The backends take the projection out of their snapshot
	// instead (Hor+ narrowed into the fov before the extract); both roads must
	// arrive at the same matrix, or the handles sit next to the object.
	HorizonWorld world;
	const Entity e = world.createEntity("Cube");
	world.addComponent(e, TransformComponent{});
	world.addComponent(e, MeshComponent{});

	EditorCameraOverride cam;
	cam.position = glm::vec3(3.0f, 2.0f, 6.0f);
	cam.view     = glm::lookAt(cam.position, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	cam.fovDegrees = 60.0f;

	for (const float aspect : { 1.0f, 16.0f / 9.0f, 3.0f }) // 3:1 is past the Hor+ cap
	{
		HE::WorldPreviewFrame frame;
		HE::buildWorldPreviewFrame(nullptr, world, cam, WorldPreviewEnv{}, aspect, frame);
		const glm::mat4 editorSide = worldPreviewProjection(cam, aspect) * cam.view;
		INFO("aspect ", aspect);
		CHECK(nearlyEqual(frame.viewProj, editorSide));
		CHECK(frame.camPos == cam.position);
		CHECK(frame.sun.w == 0.0f); // no sky → the studio light
		CHECK(frame.snapshot.objects.size() == 1u);
	}

	cam.orthographic    = true;
	cam.orthoHalfHeight = 12.0f;
	HE::WorldPreviewFrame ortho;
	HE::buildWorldPreviewFrame(nullptr, world, cam, WorldPreviewEnv{}, 2.0f, ortho);
	CHECK(nearlyEqual(ortho.viewProj, worldPreviewProjection(cam, 2.0f) * cam.view));

	// With a sky the preview lights from the extracted sun (armed, noon above
	// the horizon) and never drops below the ambient floor.
	cam.orthographic = false;
	WorldPreviewEnv noon;
	noon.sky       = true;
	noon.timeOfDay = 0.5f;
	HE::WorldPreviewFrame lit;
	HE::buildWorldPreviewFrame(nullptr, world, cam, noon, 1.0f, lit);
	CHECK(lit.sun.w > 0.0f);
	CHECK(lit.sun.y > 0.0f);
	CHECK(lit.ambient.x >= 0.10f);
	CHECK(lit.sky.skyEnabled);

	CHECK(HE::worldPreviewGridExtent(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f)) == 10.0f);
	CHECK(HE::worldPreviewGridExtent(glm::vec3(30.0f, 0.0f, 0.0f), glm::vec3(0.0f)) == 60.0f);
	CHECK(HE::worldPreviewGridExtent(glm::vec3(5000.0f, 0.0f, 0.0f), glm::vec3(0.0f)) == 200.0f);
}

TEST_CASE("The world-preview pixel shaders keep their entry points and bindings")
{
	const std::string src = HE::hlsl::kWorldPreviewPSHLSL;
	CHECK(src.find("float4 PSPreviewMesh(VSOut i)") != std::string::npos);
	CHECK(src.find("float4 PSPreviewSkinned(VSOut i)") != std::string::npos);
	// b0 = the scene's PerObject block the reused vertex shaders read, b1 = the
	// preview light the backends fill with 4 × float4.
	CHECK(src.find("cbuffer PerObject : register(b0)") != std::string::npos);
	CHECK(src.find("cbuffer PreviewLight : register(b1)") != std::string::npos);
	CHECK(src.find("register(HE_PREVIEW_TEX_REG)") != std::string::npos);
}

#ifdef _WIN32
TEST_CASE("The world-preview pixel shaders compile under FXC (ps_5_0), both albedo registers")
{
	using Microsoft::WRL::ComPtr;
	const char* src = HE::hlsl::kWorldPreviewPSHLSL;
	const D3D_SHADER_MACRO t1[] = { { "HE_PREVIEW_TEX_REG", "t1" }, { nullptr, nullptr } };
	for (const D3D_SHADER_MACRO* defines : { static_cast<const D3D_SHADER_MACRO*>(nullptr), t1 })
		for (const char* entry : { "PSPreviewMesh", "PSPreviewSkinned" })
		{
			ComPtr<ID3DBlob> code, err;
			const HRESULT hr = D3DCompile(src, std::strlen(src), "worldpreview", defines, nullptr,
			                              entry, "ps_5_0", 0, 0, &code, &err);
			INFO("entry ", entry, defines ? " (t1)" : " (t0)", ": ",
			     err ? static_cast<const char*>(err->GetBufferPointer()) : "");
			CHECK(SUCCEEDED(hr));
			CHECK(code);
		}

	// Negative control: the same call on a broken copy has to fail, or the
	// check above could never have caught anything.
	std::string broken = src;
	broken.replace(broken.find("uPBR.y"), 6, "uPBRX.y");
	ComPtr<ID3DBlob> code, err;
	const HRESULT hr = D3DCompile(broken.c_str(), broken.size(), "worldpreview", nullptr, nullptr,
	                              "PSPreviewMesh", "ps_5_0", 0, 0, &code, &err);
	CHECK(FAILED(hr));
}
#endif
