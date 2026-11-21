#include "EngineCheck.h"
#include "GpuCheck.h"

#include <ContentManager/ContentManager.h>
#include <Diagnostics/Log.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeRuntime.h>
#include <Renderer/IRenderer.h>
#include <Renderer/RendererFactory.h>
#include <Window/Window.h>

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <thread>

namespace gpucheck
{
namespace
{
constexpr double kSlope = 0.1, kBase = 600.0, kPivot = 3072.0, kWorld = 32 * 512.0;
constexpr uint32_t kW = 480, kH = 270;
}

int runEngineCheck(HE::RendererBackend backend, HE::RenderPath path, const std::string& packPath, const std::string& pngPath, const std::string& arrayRoot, bool farMarker)
{
	const char* bname = backend == HE::RendererBackend::Metal ? "Metal" : backend == HE::RendererBackend::D3D11 ? "D3D11"
	                  : backend == HE::RendererBackend::D3D12 ? "D3D12" : backend == HE::RendererBackend::Vulkan ? "Vulkan" : "OpenGL";
	const char* pname = path == HE::RenderPath::Deferred ? "deferred" : "forward";
	std::printf("[%s engine, %s%s]\n", bname, pname, farMarker ? ", far image" : "");
	if (!RendererFactory::Available(backend)) { std::printf("  not in this binary\n"); return 0; }

	int failures = 0;
	const auto fail = [&](const char* what, const std::string& detail = {}) { ++failures; std::printf("    FAIL  %s %s\n", what, detail.c_str()); };

	try
	{
		HE::WindowProps wp;
		wp.title = "landscape_gpu_check";
		wp.width = kW;
		wp.height = kH;
		wp.vsync = false;
		wp.api = backend;
		wp.startHidden = true;
		auto window = std::make_unique<HE::Window>(wp);
		std::unique_ptr<IRenderer> renderer = RendererFactory::Create(backend);
		ContentManager content;
		if (!arrayRoot.empty()) content.setEngineContentRoot(arrayRoot);
		renderer->SetContentManager(&content);
		renderer->Initialize(window.get());
		renderer->SetRenderPath(path);

		HorizonWorld world;
		const EngineCamera cam{ { 2600.0, 720.0, 3072.0 }, { 1.0, -0.12, 0.2 }, { 2048.0, 100.0, 1024.0 } };
		world.setOrigin(cam.origin);
		renderer->SetWorld(&world);

		HE::LandscapeRuntime rt;
		HE::LandscapeRuntimeConfig cfg;
		cfg.store.byteBudget = 80ull * 1024 * 1024;
		if (!arrayRoot.empty()) { cfg.textureGroup = 1; cfg.textureMeters = 8.0f; }   // Forest
		std::string err;
		if (!rt.beginFromPack(packPath, cfg, &err)) { std::printf("  runtime: %s\n", err.c_str()); return 1; }
		const auto t0 = std::chrono::steady_clock::now();
		for (;;)
		{
			rt.updateAt(cam.pos, glm::vec3(0.0f), 5.0, cam.origin);
			if (rt.store()->settled() && rt.atlas()->stats().used == rt.store()->residentKeys().size()) break;
			if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(120)) { std::printf("  the store did not settle\n"); return 1; }
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		renderer->SetLandscapeProvider(rt.renderProvider(world));

		const glm::vec3 eye = glm::vec3(cam.pos - cam.origin);
		EditorCameraOverride ov;
		ov.active = true;
		ov.view = glm::lookAt(eye, eye + glm::vec3(glm::normalize(cam.dir)), glm::vec3(0, 1, 0));
		ov.position = eye;
		ov.fovDegrees = 70.0f;
		ov.nearPlane = 2.0f;
		ov.farPlane = 20000.0f;
		std::vector<uint8_t> rgba;
		bool got = false;
		for (int frame = 0; frame < 3 && !got; ++frame)      // the first frame may build pipelines; the landscape's own state is what counts
		{
			rgba.clear();
			got = renderer->RenderSceneImage(ov, kW, kH, rgba);
		}
		if (!got || rgba.size() != static_cast<size_t>(kW) * kH * 4) { fail("RenderSceneImage gave an image"); }
		else
		{
			writePngFile(pngPath, static_cast<int>(kW), static_cast<int>(kH), rgba.data());
			// where the plane is, from the camera's own matrix (the extractor builds perspective(fov, aspect, near, far) in GL convention)
			const glm::mat4 proj = glm::perspective(glm::radians(70.0f), static_cast<float>(kW) / static_cast<float>(kH), 2.0f, 20000.0f);
			const glm::dmat4 inv = glm::inverse(glm::dmat4(proj * ov.view));
			int expectedGround = 0, groundGreen = 0, expectedSky = 0, skyGreen = 0, marker = 0;
			double contrast = 0.0;   // mean |luminance step| between horizontal neighbours on the nearest ground: a flat lit plane is smooth, a texture is not
			int contrastN = 0;
			for (uint32_t py = 0; py < kH; ++py)
				for (uint32_t px = 0; px < kW; ++px)
				{
					const double nx = (px + 0.5) / kW * 2.0 - 1.0, ny = 1.0 - (py + 0.5) / kH * 2.0;
					glm::dvec4 a = inv * glm::dvec4(nx, ny, -1.0, 1.0), b = inv * glm::dvec4(nx, ny, 1.0, 1.0);
					const glm::dvec3 o = glm::dvec3(a) / a.w + cam.origin, d = glm::normalize(glm::dvec3(b) / b.w + cam.origin - o);
					const double den = d.y - kSlope * d.x;
					double t = -1.0;
					if (std::abs(den) > 1e-9) t = (kBase - kSlope * kPivot - o.y + kSlope * o.x) / den;
					bool hit = t > 0.0;
					if (hit)
					{
						const glm::dvec3 p = o + d * t;
						hit = p.x >= 0.0 && p.x <= kWorld && p.z >= 0.0 && p.z <= kWorld;
					}
					const uint8_t* c = &rgba[(static_cast<size_t>(py) * kW + px) * 4];
					const bool green = c[1] > c[0] + 6 && c[1] > c[2] + 6;
					marker += c[0] > c[1] + 30 && c[2] > c[1] + 30 ? 1 : 0;   // magenta survives the tonemap as "green is the smallest by far"
					if (hit && t < 4000.0)
					{
						++expectedGround; groundGreen += green ? 1 : 0;
						if (px + 1 < kW && py > kH * 85 / 100)   // the nearest ground: farther out one pixel is many texture tiles and the mips average it out
						{
							const uint8_t* n = c + 4;
							const double l0 = 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2], l1 = 0.3 * n[0] + 0.59 * n[1] + 0.11 * n[2];
							contrast += std::abs(l1 - l0);
							++contrastN;
						}
					}
					else if (!hit) { ++expectedSky; skyGreen += green ? 1 : 0; }
				}
			std::printf("  expected ground px %d, green %d (%.1f %%); expected sky px %d, green %d\n", expectedGround, groundGreen,
			            100.0 * groundGreen / std::max(expectedGround, 1), expectedSky, skyGreen);
			if (!arrayRoot.empty())
			{
				std::printf("  world-uv ground from the Forest array: green %.1f %%, mean step between neighbours %.2f/255\n", 100.0 * groundGreen / std::max(expectedGround, 1), contrast / std::max(contrastN, 1));
				if (groundGreen > expectedGround / 5) fail("the ground is no longer the flat green", std::to_string(groundGreen) + " of " + std::to_string(expectedGround));
				if (contrast / std::max(contrastN, 1) < 0.6) fail("the ground has texture in it");
			}
			if (farMarker)
			{
				// The real renderer's frame, not the offscreen class: its own texture bindings must leave the far image's slot alone.
				const bool takes = backend == HE::RendererBackend::Metal || backend == HE::RendererBackend::OpenGL;
				std::printf("  far image: %d px in the marker's colour (%s)\n", marker, takes ? "this backend reads it" : "this backend does not take it yet");
				if (takes && marker < static_cast<int>(kW * kH / 200)) fail("ground beyond the atlas is drawn from the far image", std::to_string(marker) + " px");
				if (!takes && marker != 0) fail("a backend that does not take the far image shows none of it", std::to_string(marker) + " px");
			}
			if (expectedGround < static_cast<int>(kW * kH / 10)) fail("the camera sees the ground");
			if (arrayRoot.empty() && groundGreen < expectedGround * 97 / 100) fail("the landscape is drawn where the plane is", std::to_string(groundGreen) + " of " + std::to_string(expectedGround));
			if (skyGreen > expectedSky * 3 / 100) fail("nothing green above the horizon", std::to_string(skyGreen) + " of " + std::to_string(expectedSky));
		}
		renderer->SetLandscapeProvider({});
		renderer->Shutdown();
	}
	catch (const std::exception& e)
	{
		std::printf("  the renderer could not start here: %s\n", e.what());
		return 3;
	}
	std::printf("  %s\n", failures == 0 ? "OK" : "FAILED");
	return failures == 0 ? 0 : 1;
}

// GI reflection against the TILE landscape (Thema 326 step 16's hfClosestHit kernel), GL only.
// Every check above calls SetLandscapeProvider but never HorizonWorld::setLandscapeRuntime, so
// RenderExtractor::extractLandscapeGiHeightfield (which reads world.landscapeRuntime()) never had
// anything to feed it — step 16's kGiReflCS + kGiHeightfieldGLSL change had never run on a GPU
// (step 23's gate). Builds the same ramp pack as the checks above, a mirror cube standing on it,
// and a camera above and behind the mirror looking down at it: reflecting off a vertical mirror
// only flips the horizontal component of the view ray, so the outgoing ray keeps the same
// downward slope and lands on the ramp a short hop past the mirror — comfortably inside
// IRenderer::GIReflectionSettings's default 200 m trace distance. Checks the 'GI pipelines built'
// log line and, since a heightfield hit still feeds a flat placeholder albedo (not yet the ramp's
// own paint — design doc §5/§6, a follow-up gap, not this step's), that the mirror's pixel actually
// MOVES when the landscape's GI heightfield is removed (step 26's fix): toggling
// GIReflectionSettings::enabled instead (what this used to do) is not a valid control, because
// env.skyEnabled defaults true and RenderSkyReflCube bakes a real sky cube every frame regardless —
// kGiReflCS's miss branch (OpenGLRenderer.cpp) then samples that cube with full confidence, so a
// mirror that only ever sees the baked sky (a permanently broken hfClosestHit) still "moves" when
// reflections toggle off and the check passed for the wrong reason. Pulling world.setLandscapeRuntime
// back out (reflections stay ON in both renders) makes extractLandscapeGiHeightfield feed nothing
// (RenderExtractor.cpp), so a broken march gets the SAME sky-only pixel in both renders and fails.
int runReflectionCheck(const std::string& packPath, const std::string& pngPath)
{
	std::printf("[OpenGL engine, GI reflection against the tile landscape]\n");
	if (!RendererFactory::Available(HE::RendererBackend::OpenGL)) { std::printf("  not in this binary\n"); return 0; }

	// [0] the GI pipelines linked, [1] this OpenGL has no GI at all (below 4.3: no compute shaders, every Mac). The renderer says [1] while it
	// starts, before anything is judged: from then on nothing here is a failure of the landscape.
	bool seen[2] = { false, false };
	bool& pipelinesBuilt = seen[0];
	int failures = 0;
	const auto fail = [&](const char* what, const std::string& detail = {}) {
		if (seen[1]) return;
		++failures;
		std::printf("    FAIL  %s %s\n", what, detail.c_str());
	};

	const int sink = HE::Log::addSink(
		[](const HE::Log::Record& r, void* user) {
			if (r.category != HE::Log::Cat::RHI) return;
			if (std::strstr(r.message, "GI pipelines built")) static_cast<bool*>(user)[0] = true;
			if (std::strstr(r.message, "GI unavailable")) static_cast<bool*>(user)[1] = true;
		},
		seen);

	try
	{
		HE::WindowProps wp;
		wp.title = "landscape_gpu_check (GI reflection)";
		wp.width = kW;
		wp.height = kH;
		wp.vsync = false;
		wp.api = HE::RendererBackend::OpenGL;
		wp.startHidden = true;
		auto window = std::make_unique<HE::Window>(wp);
		std::unique_ptr<IRenderer> renderer = RendererFactory::Create(HE::RendererBackend::OpenGL);
		ContentManager content;
		renderer->SetContentManager(&content);
		renderer->Initialize(window.get());
		renderer->SetRenderPath(HE::RenderPath::Forward);

		HorizonWorld world;
		world.setOrigin(glm::dvec3(0.0));
		renderer->SetWorld(&world);

		// kUnlitFS's uLightCount==0 branch is a flat unlit fallback that returns before ever touching
		// uGIRefl (OpenGLRenderer.cpp:537-543) — this check needs a real light, not just a landscape.
		{
			auto& lreg = world.registry();
			const auto sunEnt = world.createEntity("Sun");
			TransformComponent sunTf;
			sunTf.rotation = glm::vec3(-45.0f, -45.0f, 0.0f);
			lreg.emplace<TransformComponent>(sunEnt, sunTf);
			LightComponent sun;
			sun.type = HE::LightType::Directional;
			lreg.emplace<LightComponent>(sunEnt, sun);
		}

		HE::LandscapeRuntime rt;
		HE::LandscapeRuntimeConfig cfg;
		cfg.store.byteBudget = 80ull * 1024 * 1024;
		std::string err;
		if (!rt.beginFromPack(packPath, cfg, &err)) { std::printf("  runtime: %s\n", err.c_str()); HE::Log::removeSink(sink); return 1; }
		const glm::dvec3 camPos{ 2800.0, 750.0, 1024.0 };
		const auto t0 = std::chrono::steady_clock::now();
		for (;;)
		{
			rt.updateAt(camPos, glm::vec3(0.0f), 5.0, glm::dvec3(0.0));
			if (rt.store()->settled() && rt.atlas()->stats().used == rt.store()->residentKeys().size() && rt.giHeightfield().active())
				break;
			if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(120))
			{
				std::printf("  the store did not settle\n");
				HE::Log::removeSink(sink);
				return 1;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		world.setLandscapeRuntime(&rt);
		renderer->SetLandscapeProvider(rt.renderProvider(world));

		IRenderer::GIReflectionSettings refl;
		refl.enabled = true;
		renderer->SetGIReflectionSettings(refl);

		// 4 x 40 x 60 m, standing on the ramp at the pivot (ground height there is exactly kBase).
		const glm::vec3 mirrorPos(static_cast<float>(kPivot), static_cast<float>(kBase) + 20.0f, 1024.0f);
		MaterialAsset mirror;
		mirror.type      = HE::AssetType::Material;
		mirror.name      = "GIReflTileMirror";
		mirror.baseColor[0] = mirror.baseColor[1] = mirror.baseColor[2] = 0.9f;
		mirror.metallic  = 1.0f;
		mirror.roughness = 0.05f;
		const HE::UUID mirrorId = content.registerMaterial(std::move(mirror));
		auto& reg = world.registry();
		const auto mirrorEnt = world.createEntity("GIReflTileMirror");
		TransformComponent tf;
		tf.position = mirrorPos;
		tf.scale    = glm::vec3(4.0f, 40.0f, 60.0f);
		reg.emplace<TransformComponent>(mirrorEnt, tf);
		reg.emplace<MeshComponent>(mirrorEnt, MeshComponent{ HE::kDefaultCubeMeshId });
		reg.emplace<MaterialComponent>(mirrorEnt, MaterialComponent{ mirrorId });

		const glm::vec3 eye = glm::vec3(camPos);
		EditorCameraOverride ov;
		ov.active     = true;
		ov.view       = glm::lookAt(eye, mirrorPos, glm::vec3(0, 1, 0));
		ov.position   = eye;
		ov.fovDegrees = 60.0f;
		ov.nearPlane  = 1.0f;
		ov.farPlane   = 5000.0f;

		// The reflection targets are temporal (GIReflTemporal/Blur/Mix): frame 0's history is still
		// black, so a single frame reads mostly unconverged. he_gi_hf_check.py's own GI witnesses
		// settle on 60 frames by convention; match it rather than guess a smaller number.
		const auto renderMirrorPixel = [&](std::array<uint8_t, 4>& out) {
			std::vector<uint8_t> rgba;
			bool got = false;
			for (int frame = 0; frame < 60; ++frame)
			{
				rgba.clear();
				got = renderer->RenderSceneImage(ov, kW, kH, rgba);
				if (!got) break;
			}
			if (!got || rgba.size() != static_cast<size_t>(kW) * kH * 4) return false;
			// lookAt points the frame's centre pixel exactly at the mirror.
			const uint8_t* m = &rgba[(static_cast<size_t>(kH / 2) * kW + kW / 2) * 4];
			for (int c = 0; c < 4; ++c) out[static_cast<size_t>(c)] = m[c];
			return true;
		};

		std::array<uint8_t, 4> onPx{}, skyOnlyPx{};
		if (!renderMirrorPixel(onPx)) fail("RenderSceneImage gave an image (landscape heightfield)");
		else
		{
			std::vector<uint8_t> rgba;
			rgba.resize(static_cast<size_t>(kW) * kH * 4);
			renderer->RenderSceneImage(ov, kW, kH, rgba);   // one more frame, same state, just to save the picture
			writePngFile(pngPath, static_cast<int>(kW), static_cast<int>(kH), rgba.data());

			// Negative control: the SAME scene, reflections still ON, but with the landscape's GI
			// heightfield pulled back out (world.setLandscapeRuntime(nullptr) — SetLandscapeProvider
			// stays, so the ramp still renders, only extractLandscapeGiHeightfield now has nothing to
			// feed, RenderExtractor.cpp:804). This is deliberately NOT the old "toggle
			// GIReflectionSettings::enabled" control: env.skyEnabled defaults true, so RenderSkyReflCube
			// bakes a real sky cube every frame regardless, and kGiReflCS's miss branch samples it with
			// full confidence — a mirror that only ever sees that baked sky (i.e. hfClosestHit always
			// misses) still "moved" when reflections toggled off, passing for the wrong reason. With the
			// heightfield removed instead, a working march has nothing left to hit either, so it falls
			// onto that exact same sky branch — a broken march gets the identical pixel in both renders.
			world.setLandscapeRuntime(nullptr);
			if (!renderMirrorPixel(skyOnlyPx)) fail("RenderSceneImage gave an image (no heightfield)");
			else
			{
				std::printf("  mirror pixel landscape heightfield rgb(%d,%d,%d), no heightfield (sky only) rgb(%d,%d,%d)\n",
				            onPx[0], onPx[1], onPx[2], skyOnlyPx[0], skyOnlyPx[1], skyOnlyPx[2]);
				int worst = 0;
				for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(static_cast<int>(onPx[static_cast<size_t>(c)]) - static_cast<int>(skyOnlyPx[static_cast<size_t>(c)])));
				if (worst <= 8)
					fail("the mirror shows the landscape's heightfield, not just the baked sky",
					     "worst channel delta " + std::to_string(worst) + "/255");
			}
		}
		renderer->SetLandscapeProvider({});
		renderer->Shutdown();
	}
	catch (const std::exception& e)
	{
		std::printf("  the renderer could not start here: %s\n", e.what());
		HE::Log::removeSink(sink);
		return 3;
	}
	HE::Log::removeSink(sink);
	// Not a failure of the landscape: the kernel under test cannot exist on this context, so there is nothing to judge.
	if (seen[1]) { std::printf("  NOT CHECKED: this OpenGL has no GI (below 4.3), the reflection kernel does not run here\n"); return 0; }
	if (!pipelinesBuilt) fail("GI pipelines built (log line never appeared)");
	std::printf("  %s\n", failures == 0 ? "OK" : "FAILED");
	return failures == 0 ? 0 : 1;
}
} // namespace gpucheck
