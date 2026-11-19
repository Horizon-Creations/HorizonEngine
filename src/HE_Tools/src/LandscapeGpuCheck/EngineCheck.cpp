#include "EngineCheck.h"
#include "GpuCheck.h"

#include <ContentManager/ContentManager.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeRuntime.h>
#include <Renderer/IRenderer.h>
#include <Renderer/RendererFactory.h>
#include <Window/Window.h>

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <thread>

namespace gpucheck
{
namespace
{
constexpr double kSlope = 0.1, kBase = 600.0, kPivot = 3072.0, kWorld = 32 * 512.0;
constexpr uint32_t kW = 480, kH = 270;
}

int runEngineCheck(HE::RendererBackend backend, HE::RenderPath path, const std::string& packPath, const std::string& pngPath, const std::string& arrayRoot)
{
	const char* bname = backend == HE::RendererBackend::Metal ? "Metal" : backend == HE::RendererBackend::D3D11 ? "D3D11"
	                  : backend == HE::RendererBackend::D3D12 ? "D3D12" : backend == HE::RendererBackend::Vulkan ? "Vulkan" : "OpenGL";
	const char* pname = path == HE::RenderPath::Deferred ? "deferred" : "forward";
	std::printf("[%s engine, %s]\n", bname, pname);
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
			int expectedGround = 0, groundGreen = 0, expectedSky = 0, skyGreen = 0;
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
} // namespace gpucheck
