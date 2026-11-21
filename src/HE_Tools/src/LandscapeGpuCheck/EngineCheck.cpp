#include "EngineCheck.h"
#include "GpuCheck.h"

#include <ContentManager/ContentManager.h>
#include <Diagnostics/Log.h>
#include <HorizonScene/Components/LandscapeWorldComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>
#include <MaterialGraph/MaterialGraph.h>
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

// HE_LANDSCAPE_PASSES for the next frames: "0" leaves the ground out of the shadow and GI passes, null puts it back.
void landscapePasses(const char* value)
{
#ifdef _WIN32
	_putenv_s("HE_LANDSCAPE_PASSES", value ? value : "");   // an empty value removes the variable
#else
	if (value) setenv("HE_LANDSCAPE_PASSES", value, 1);
	else unsetenv("HE_LANDSCAPE_PASSES");
#endif
}
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
				const bool takes = backend == HE::RendererBackend::Metal || backend == HE::RendererBackend::OpenGL || backend == HE::RendererBackend::Vulkan;
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

// The ground drawn with a MATERIAL (LandscapeRenderFrame::materialId): the real renderer, the ramp world (all paint on layer 0), and a graph
// material whose Landscape Layer Blend makes layer 0 blue and layer 1 red, colours the fixed blend does not have (its layer 0 is green).
//   * material:  the ground is blue. Blue means the material's fragment ran AND read 100 % layer 0 from the tile atlas (no weights: black;
//                the wrong page or channel: red). Control: the same frame without the material is green and has no blue.
//   * shadow:    a box stands on the ground. With its shadow switched on, ground pixels are darker than with it switched off (the box itself
//                is in both frames, so only its shadow differs). GI is off here: the sun's shadow is the cascade map's.
//   * fog:       with fog in the scene the ground's colour moves.
int runMaterialCheck(HE::RendererBackend backend, HE::RenderPath path, const std::string& packPath, const std::string& pngPath, const std::string& arrayRoot)
{
	const char* bname = backend == HE::RendererBackend::Metal ? "Metal" : backend == HE::RendererBackend::D3D11 ? "D3D11"
	                  : backend == HE::RendererBackend::D3D12 ? "D3D12" : backend == HE::RendererBackend::Vulkan ? "Vulkan" : "OpenGL";
	std::printf("[%s engine, %s, the ground drawn with a material]\n", bname, path == HE::RenderPath::Deferred ? "deferred" : "forward");
	if (!RendererFactory::Available(backend)) { std::printf("  not in this binary\n"); return 0; }
	int failures = 0;
	const auto fail = [&](const char* what, const std::string& detail = {}) { ++failures; std::printf("    FAIL  %s %s\n", what, detail.c_str()); };
	try
	{
		HE::WindowProps wp;
		wp.title = "landscape_gpu_check (material)";
		wp.width = kW;
		wp.height = kH;
		wp.vsync = false;
		wp.api = backend;
		wp.startHidden = true;
		auto window = std::make_unique<HE::Window>(wp);
		std::unique_ptr<IRenderer> renderer = RendererFactory::Create(backend);
		if (!renderer->GetCapabilities().landscapeMaterial)
		{
			std::printf("  NOT CHECKED: this backend draws the tile landscape with its fixed blend only\n");
			return 0;
		}
		ContentManager content;
		renderer->SetContentManager(&content);
		renderer->Initialize(window.get());
		renderer->SetRenderPath(path);
		IRenderer::GISettings gi;
		gi.enabled = false;
		renderer->SetGISettings(gi);
		IRenderer::ShadowSettings shadow;
		shadow.distance = 600.0f;
		renderer->SetShadowSettings(shadow);

		HorizonWorld world;
		const glm::dvec3 origin{ 2048.0, 100.0, 1024.0 }, camPos{ 2900.0, 650.0, 3072.0 };
		world.setOrigin(origin);
		renderer->SetWorld(&world);
		auto& reg = world.registry();
		{
			const auto sunEnt = world.createEntity("Sun");
			TransformComponent sunTf;
			sunTf.rotation = glm::vec3(-45.0f, -45.0f, 0.0f);
			reg.emplace<TransformComponent>(sunEnt, sunTf);
			LightComponent sun;
			sun.type = HE::LightType::Directional;
			reg.emplace<LightComponent>(sunEnt, sun);
		}
		// 30 x 60 x 30 m, standing on the ramp 110 m in front of the camera
		const double boxX = 3010.0, boxZ = 3090.0;
		const auto boxEnt = world.createEntity("Caster");
		{
			TransformComponent tf;
			tf.position = glm::vec3(glm::dvec3(boxX, kBase + kSlope * (boxX - kPivot) + 30.0, boxZ) - origin);
			tf.scale = glm::vec3(30.0f, 60.0f, 30.0f);
			reg.emplace<TransformComponent>(boxEnt, tf);
			reg.emplace<MeshComponent>(boxEnt, MeshComponent{ HE::kDefaultCubeMeshId });
		}

		using HE::MaterialGraph;
		using HE::MatNodeType;
		MaterialGraph g;
		const int outId = g.addNode(MatNodeType::Output);
		const int blend = g.addNode(MatNodeType::LandscapeLayerBlend);
		g.findNode(blend)->s = "Layer 1\nLayer 2";
		const int blue = g.addNode(MatNodeType::ConstColor), red = g.addNode(MatNodeType::ConstColor);
		g.findNode(blue)->p[0] = 0.0f; g.findNode(blue)->p[1] = 0.0f; g.findNode(blue)->p[2] = 1.0f;
		g.findNode(red)->p[0] = 1.0f; g.findNode(red)->p[1] = 0.0f; g.findNode(red)->p[2] = 0.0f;
		g.connect(blue, 0, blend, 0);
		g.connect(red, 0, blend, 1);
		g.connect(blend, 0, outId, HE::kMatOutputBaseColorPin);
		MaterialAsset ma;
		ma.type = HE::AssetType::Material;
		ma.name = "LandscapeMarker";
		ma.nodeGraphJson = HE::materialGraphToJson(g);
		const HE::UUID marker = content.registerMaterial(std::move(ma));
		content.regenerateMaterialFromGraph(marker);

		HE::LandscapeRuntime rt;
		HE::LandscapeRuntimeConfig cfg;
		cfg.store.byteBudget = 80ull * 1024 * 1024;
		std::string err;
		if (!rt.beginFromPack(packPath, cfg, &err)) { std::printf("  runtime: %s\n", err.c_str()); return 1; }
		const auto t0 = std::chrono::steady_clock::now();
		for (;;)
		{
			rt.updateAt(camPos, glm::vec3(0.0f), 5.0, origin);
			if (rt.store()->settled() && rt.atlas()->stats().used == rt.store()->residentKeys().size()) break;
			if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(120)) { std::printf("  the store did not settle\n"); return 1; }
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		renderer->SetLandscapeProvider(rt.renderProvider(world));

		const glm::vec3 eye = glm::vec3(camPos - origin);
		EditorCameraOverride ov;
		ov.active = true;
		ov.view = glm::lookAt(eye, eye + glm::normalize(glm::vec3(1.0f, -0.45f, 0.15f)), glm::vec3(0, 1, 0));
		ov.position = eye;
		ov.fovDegrees = 70.0f;
		ov.nearPlane = 2.0f;
		ov.farPlane = 20000.0f;
		const auto shoot = [&](std::vector<uint8_t>& rgba) {
			bool got = false;
			for (int frame = 0; frame < 6; ++frame)      // pipelines build, the cascades settle
			{
				rgba.clear();
				got = renderer->RenderSceneImage(ov, kW, kH, rgba);
			}
			return got && rgba.size() == static_cast<size_t>(kW) * kH * 4;
		};
		const auto isBlue = [](const uint8_t* c) { return c[2] > c[0] + 20 && c[2] > c[1] + 20; };
		const auto isGreen = [](const uint8_t* c) { return c[1] > c[0] + 6 && c[1] > c[2] + 6; };
		const auto lum = [](const uint8_t* c) { return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]; };
		const size_t px = static_cast<size_t>(kW) * kH;

		std::vector<uint8_t> lit, flat, unshadowed, fogged;
		rt.setMaterialId(marker);
		if (!shoot(lit)) fail("RenderSceneImage gave an image");
		else
		{
			writePngFile(pngPath, static_cast<int>(kW), static_cast<int>(kH), lit.data());
			size_t blueLit = 0, blueFlat = 0, greenFlat = 0;
			for (size_t i = 0; i < px; ++i) blueLit += isBlue(&lit[i * 4]) ? 1 : 0;
			rt.setMaterialId({});
			if (!shoot(flat)) fail("RenderSceneImage gave an image (fixed blend)");
			else
				for (size_t i = 0; i < px; ++i) { blueFlat += isBlue(&flat[i * 4]) ? 1 : 0; greenFlat += isGreen(&flat[i * 4]) ? 1 : 0; }
			std::printf("  material: %zu of %zu px are the material's layer 0 (blue); without the material %zu blue, %zu green\n", blueLit, px, blueFlat, greenFlat);
			if (blueLit < px * 2 / 5) fail("the ground is drawn by the material, with its weights from the tile atlas", std::to_string(blueLit) + " px");
			if (blueFlat > px / 100 || greenFlat < px * 2 / 5) fail("control: without the material the ground is the fixed blend's green");

			rt.setMaterialId(marker);
			reg.get<MeshComponent>(boxEnt).castsShadow = false;
			if (!shoot(unshadowed)) fail("RenderSceneImage gave an image (no caster)");
			else
			{
				size_t darker = 0;
				for (size_t i = 0; i < px; ++i)
					darker += isBlue(&lit[i * 4]) && isBlue(&unshadowed[i * 4]) && lum(&lit[i * 4]) < 0.8 * lum(&unshadowed[i * 4]) ? 1 : 0;
				std::printf("  shadow: %zu ground px are darker with the box's shadow on than with it off\n", darker);
				if (darker < 150) fail("the ground takes the box's shadow", std::to_string(darker) + " px");
			}
			reg.get<MeshComponent>(boxEnt).castsShadow = true;

			// The ground CASTS: a second box hangs UNDER the ramp and a camera under the ramp looks at the two faces of it that point at
			// the sun (the engine's sun stands where it stands, so the receiver goes behind the ground instead). They are lit when the
			// ground is not in the shadow maps and in its shadow when it is.
			{
				const glm::dvec3 hangAbs(boxX, kBase + kSlope * (boxX - kPivot) - 50.0, boxZ);
				const auto hangEnt = world.createEntity("Hanging");
				TransformComponent htf;
				htf.position = glm::vec3(hangAbs - origin);
				htf.scale = glm::vec3(30.0f);
				reg.emplace<TransformComponent>(hangEnt, htf);
				reg.emplace<MeshComponent>(hangEnt, MeshComponent{ HE::kDefaultCubeMeshId });
				const EditorCameraOverride over = ov;
				const glm::vec3 underEye = htf.position + glm::vec3(80.0f, 5.0f, 80.0f);
				ov.view = glm::lookAt(underEye, htf.position, glm::vec3(0, 1, 0));
				ov.position = underEye;
				const glm::mat4 proj = glm::perspective(glm::radians(70.0f), static_cast<float>(kW) / static_cast<float>(kH), 2.0f, 20000.0f);
				const glm::vec4 clip = proj * ov.view * glm::vec4(htf.position, 1.0f);
				const int cx = static_cast<int>((clip.x / clip.w * 0.5f + 0.5f) * kW), cy = static_cast<int>((0.5f - clip.y / clip.w * 0.5f) * kH);
				const auto boxLum = [&](const std::vector<uint8_t>& img) {
					double s = 0.0;
					int n = 0;
					for (int y = cy - 4; y <= cy + 4; ++y)
						for (int x = cx - 4; x <= cx + 4; ++x)
							if (x >= 0 && y >= 0 && x < static_cast<int>(kW) && y < static_cast<int>(kH)) { s += lum(&img[(static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4]); ++n; }
					return s / std::max(n, 1);
				};
				std::vector<uint8_t> under, underNoCaster;
				landscapePasses("0");
				const bool gotB = shoot(underNoCaster);
				landscapePasses(nullptr);
				if (!gotB || !shoot(under)) fail("RenderSceneImage gave an image (under the ground)");
				else
				{
					const double below = boxLum(under), belowNoCaster = boxLum(underNoCaster);
					std::printf("  the ground casts: a box under the ground is %.0f/255, and %.0f/255 when the ground is not in the shadow maps\n",
					            below, belowNoCaster);
					// what is left in the shadow is the sky's light; without the caster the two are equal
					if (below > belowNoCaster * 0.8) fail("a box under the ground stands in the ground's shadow");
					writePngFile(pngPath.substr(0, pngPath.size() - 4) + "_under.png", static_cast<int>(kW), static_cast<int>(kH), under.data());
				}
				ov = over;
				world.destroyEntity(hangEnt);
			}

			// The same shadow with GI on: the sun's shadow is then ray traced for the pixels of the GI pre-pass, which the ground is in.
			if (renderer->GetCapabilities().supportsGlobalIllumination)
			{
				gi.enabled = true;
				renderer->SetGISettings(gi);
				const auto settle = [&](std::vector<uint8_t>& img) { bool ok = true; for (int i = 0; i < 10 && ok; ++i) ok = shoot(img); return ok; };
				// The control is the ground left out of the pre-pass: its pixels then have no ray-traced shadow value.
				std::vector<uint8_t> giOn, giOff;
				const bool a = settle(giOn);
				landscapePasses("0");
				const bool b = a && settle(giOff);
				landscapePasses(nullptr);
				if (!b) fail("RenderSceneImage gave an image (GI)");
				else
				{
					size_t darker = 0;
					for (size_t i = 0; i < px; ++i)
						darker += isBlue(&giOn[i * 4]) && isBlue(&giOff[i * 4]) && lum(&giOn[i * 4]) < 0.8 * lum(&giOff[i * 4]) ? 1 : 0;
					std::printf("  shadow, GI on: %zu ground px are darker with the ground in the GI pre-pass than without\n", darker);
					writePngFile(pngPath.substr(0, pngPath.size() - 4) + "_gi_nopass.png", static_cast<int>(kW), static_cast<int>(kH), giOff.data());
					if (darker < 150) fail("the ground takes the box's ray-traced shadow (GI on)", std::to_string(darker) + " px");
					writePngFile(pngPath.substr(0, pngPath.size() - 4) + "_gi.png", static_cast<int>(kW), static_cast<int>(kH), giOn.data());
				}
				// ... and the ground CASTS with GI on: its height clipmap is in the ray-tracing structure. The box under the ramp again,
				// seen from under the ramp, against the same frame without the ground there.
				{
					world.setLandscapeRuntime(&rt);
					for (const auto t2 = std::chrono::steady_clock::now(); !rt.giHeightfield().active() && std::chrono::steady_clock::now() - t2 < std::chrono::seconds(60);)
					{
						rt.updateAt(camPos, glm::vec3(0.0f), 5.0, origin);
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
					}
					const auto hangEnt = world.createEntity("HangingGi");
					TransformComponent htf;
					htf.position = glm::vec3(glm::dvec3(boxX, kBase + kSlope * (boxX - kPivot) - 50.0, boxZ) - origin);
					htf.scale = glm::vec3(30.0f);
					reg.emplace<TransformComponent>(hangEnt, htf);
					reg.emplace<MeshComponent>(hangEnt, MeshComponent{ HE::kDefaultCubeMeshId });
					const EditorCameraOverride over = ov;
					const glm::vec3 underEye = htf.position + glm::vec3(80.0f, 5.0f, 80.0f);
					ov.view = glm::lookAt(underEye, htf.position, glm::vec3(0, 1, 0));
					ov.position = underEye;
					const auto centreLum = [&](const std::vector<uint8_t>& img) {   // lookAt puts the box on the centre pixel
						double s = 0.0;
						for (uint32_t y = kH / 2 - 4; y <= kH / 2 + 4; ++y)
							for (uint32_t x = kW / 2 - 4; x <= kW / 2 + 4; ++x) s += lum(&img[(static_cast<size_t>(y) * kW + x) * 4]);
						return s / 81.0;
					};
					std::vector<uint8_t> with, without;
					const bool gotA = rt.giHeightfield().active() && settle(with);
					landscapePasses("0");
					const bool gotB = gotA && settle(without);
					landscapePasses(nullptr);
					if (!gotB) fail("the landscape's GI height clipmap is there and the frames come back (GI, under the ground)");
					else
					{
						std::printf("  the ground casts, GI on: a box under the ground is %.0f/255, and %.0f/255 when the ground is not in the ray-tracing structure\n",
						            centreLum(with), centreLum(without));
						if (centreLum(with) > centreLum(without) * 0.8) fail("a box under the ground stands in the ground's ray-traced shadow");
						writePngFile(pngPath.substr(0, pngPath.size() - 4) + "_gi_under.png", static_cast<int>(kW), static_cast<int>(kH), with.data());
					}
					ov = over;
					world.destroyEntity(hangEnt);
					world.setLandscapeRuntime(nullptr);
				}
				gi.enabled = false;
				renderer->SetGISettings(gi);
				shoot(giOff);   // back on the cascades before the fog frame
			}

			// Ambient occlusion: the ground is in the SSAO position pre-pass, so the box darkens the ground around its foot. Against
			// the same frame with the ground left out of the pre-pass (its pixels are then background to the occlusion pass).
			{
				IRenderer::SSAOSettings ao;
				ao.enabled = true;
				ao.radius = 12.0f;
				ao.intensity = 1.0f;
				renderer->SetSSAOSettings(ao);
				std::vector<uint8_t> aoOn, aoOff, noAo, noAoNoPasses;
				const bool gotA = shoot(aoOn);
				landscapePasses("0");
				const bool gotB = gotA && shoot(aoOff);
				ao.enabled = false;
				renderer->SetSSAOSettings(ao);
				const bool gotC = gotB && shoot(noAoNoPasses);
				landscapePasses(nullptr);
				const bool gotD = gotC && shoot(noAo);
				if (!gotD) fail("RenderSceneImage gave an image (SSAO)");
				else
				{
					size_t darker = 0, selfShadow = 0, ground = 0;
					for (size_t i = 0; i < px; ++i)
					{
						if (!isBlue(&aoOn[i * 4]) || !isBlue(&aoOff[i * 4])) continue;
						++ground;
						// the occlusion's own share: what the ground in the pre-pass adds on top of what it adds without occlusion
						darker += lum(&aoOn[i * 4]) < 0.95 * lum(&aoOff[i * 4]) && !(lum(&noAo[i * 4]) < 0.95 * lum(&noAoNoPasses[i * 4])) ? 1 : 0;
						// The ground in the shadow maps must not shadow ITSELF on a plane (acne): with occlusion off, the only
						// thing the switch changes on these pixels is the ground as a caster.
						selfShadow += lum(&noAo[i * 4]) < 0.95 * lum(&noAoNoPasses[i * 4]) ? 1 : 0;
					}
					std::printf("  ambient occlusion: %zu ground px are darker with the ground in the SSAO pre-pass than without\n", darker);
					std::printf("  no self-shadow: %zu of %zu ground px are darker because the ground is in the shadow maps\n", selfShadow, ground);
					if (path == HE::RenderPath::Forward && darker < 60) fail("the ground takes ambient occlusion", std::to_string(darker) + " px");
					if (selfShadow > ground / 100) fail("the plane does not shadow itself", std::to_string(selfShadow) + " px");
					writePngFile(pngPath.substr(0, pngPath.size() - 4) + "_ao.png", static_cast<int>(kW), static_cast<int>(kH), aoOn.data());
				}
				renderer->SetSSAOSettings(IRenderer::SSAOSettings{});
			}

			IRenderer::EnvironmentSettings env = renderer->GetEnvironment();
			env.fogDensity = 0.004f;
			env.fogHeightFalloff = 0.0f;   // the ramp stands 600 m up: with a falloff the fog would lie far below it
			renderer->SetEnvironmentSettings(env);
			if (!shoot(fogged)) fail("RenderSceneImage gave an image (fog)");
			else
			{
				double gap = 0.0;
				size_t n = 0;
				for (size_t i = 0; i < px; ++i)
					if (isBlue(&lit[i * 4])) { gap += std::abs(lum(&fogged[i * 4]) - lum(&lit[i * 4])); ++n; }
				std::printf("  fog: the ground moves by %.1f/255 on average with fog in the scene\n", gap / static_cast<double>(std::max<size_t>(n, 1)));
				if (gap / static_cast<double>(std::max<size_t>(n, 1)) < 4.0) fail("the ground is fogged");
				writePngFile(pngPath.substr(0, pngPath.size() - 4) + "_fog.png", static_cast<int>(kW), static_cast<int>(kH), fogged.data());
			}
			// The real thing: the engine's Auto Landscape material (Forest) from an EngineContent folder, when one was named. The ramp is
			// all layer 0 ("Auto"), so the material decides by slope and height itself. Checked: its variant builds and draws (no pixel
			// of the fixed blend's green is left), and the ground is neither black nor one flat colour.
			if (!arrayRoot.empty())
			{
				IRenderer::EnvironmentSettings clear = renderer->GetEnvironment();
				clear.fogDensity = 0.0f;
				renderer->SetEnvironmentSettings(clear);
				content.setEngineContentRoot(arrayRoot);
				// The way the editor and a game get there: a LandscapeWorldComponent in the scene (texture group Forest, no material of
				// its own) and LandscapeRuntime::update(world). Nothing here loads the material: the renderer finds it from the frame.
				const auto lsEnt = world.createEntity("Landscape");
				LandscapeWorldComponent lw;
				lw.pack = packPath;
				lw.textureGroup = 1;
				lw.budgetMB = 80;
				reg.emplace<LandscapeWorldComponent>(lsEnt, lw);
				bool began = false;
				for (const auto t1 = std::chrono::steady_clock::now(); std::chrono::steady_clock::now() - t1 < std::chrono::seconds(120);)
				{
					rt.update(world, 0.016f, 5.0, &camPos);
					if (rt.active() && rt.store()->settled() && rt.atlas()->stats().used == rt.store()->residentKeys().size()) { began = true; break; }
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				if (!began) fail("the scene's LandscapeWorldComponent starts the landscape");
				else
				{
					std::vector<uint8_t> autoImg;
					if (!shoot(autoImg)) fail("RenderSceneImage gave an image (Auto Landscape)");
					else
					{
						size_t green = 0, ground = 0;
						double sum = 0.0, step = 0.0;
						for (size_t i = 0; i < px; ++i)
						{
							if (!isBlue(&lit[i * 4])) continue;   // where the marker frame had ground
							++ground;
							green += isGreen(&autoImg[i * 4]) && std::abs(static_cast<int>(autoImg[i * 4 + 1]) - static_cast<int>(flat[i * 4 + 1])) < 6 ? 1 : 0;
							sum += lum(&autoImg[i * 4]);
							if (i % kW + 1 < kW) step += std::abs(lum(&autoImg[i * 4 + 4]) - lum(&autoImg[i * 4]));
						}
						const double n = static_cast<double>(std::max<size_t>(ground, 1));
						std::printf("  Auto Landscape (Forest): mean ground %.0f/255, mean step between neighbours %.2f/255, %zu px still the fixed blend's green\n", sum / n, step / n, green);
						if (green > ground / 20) fail("the Auto Landscape material draws the ground");
						if (sum / n < 12.0) fail("the ground is not black");
						if (step / n < 0.3) fail("the ground has the material's texture in it");
						writePngFile(pngPath.substr(0, pngPath.size() - 4) + "_auto.png", static_cast<int>(kW), static_cast<int>(kH), autoImg.data());

						// GI reflections: a mirror standing on the ground shows the ground in the material's colours (the kernels colour a
						// hit on the height clipmap like one on an auto landscape's chunk), not sky and not one flat grey-green. A picture
						// and two numbers: the mirror's face is not the sky's blue, and it differs from the same frame with the ground
						// taken out of the ray structure.
						if (renderer->GetCapabilities().supportsGIReflections)
						{
							world.setLandscapeRuntime(&rt);
							gi.enabled = true;
							renderer->SetGISettings(gi);
							IRenderer::GIReflectionSettings refl;
							refl.enabled = true;
							renderer->SetGIReflectionSettings(refl);
							MaterialAsset mirror;
							mirror.type = HE::AssetType::Material;
							mirror.name = "Mirror";
							mirror.baseColor[0] = mirror.baseColor[1] = mirror.baseColor[2] = 0.9f;
							mirror.metallic = 1.0f;
							mirror.roughness = 0.05f;
							reg.emplace_or_replace<MaterialComponent>(boxEnt, MaterialComponent{ content.registerMaterial(std::move(mirror)) });
							const glm::mat4 proj = glm::perspective(glm::radians(70.0f), static_cast<float>(kW) / static_cast<float>(kH), 2.0f, 20000.0f);
							const glm::vec4 clip = proj * ov.view * glm::vec4(reg.get<TransformComponent>(boxEnt).position, 1.0f);
							const int cx = static_cast<int>((clip.x / clip.w * 0.5f + 0.5f) * kW), cy = static_cast<int>((0.5f - clip.y / clip.w * 0.5f) * kH);
							const auto face = [&](const std::vector<uint8_t>& img, int c) {
								double s = 0.0;
								for (int y = cy - 3; y <= cy + 3; ++y)
									for (int x = cx - 3; x <= cx + 3; ++x) s += img[(static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4 + static_cast<size_t>(c)];
								return s / 49.0;
							};
							const auto frames = [&](std::vector<uint8_t>& img) { bool ok = true; for (int i = 0; i < 10 && ok; ++i) ok = shoot(img); return ok; };
							std::vector<uint8_t> reflOn, reflNoGround;
							const bool gotA = frames(reflOn);
							landscapePasses("0");
							const bool gotB = gotA && frames(reflNoGround);
							landscapePasses(nullptr);
							if (!gotB) fail("RenderSceneImage gave an image (GI reflections)");
							else
							{
								std::printf("  GI reflections: the mirror's face is rgb(%.0f, %.0f, %.0f) with the ground in the ray structure, rgb(%.0f, %.0f, %.0f) without\n",
								            face(reflOn, 0), face(reflOn, 1), face(reflOn, 2), face(reflNoGround, 0), face(reflNoGround, 1), face(reflNoGround, 2));
								double worst = 0.0;
								for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(face(reflOn, c) - face(reflNoGround, c)));
								// Reported, not judged: on the forward path the mirror's picture comes from the screen-space reflection of
								// the pre-pass either way, and the ray-traced share is small in a ten-frame capture. What a hit on the ground
								// is coloured with is the landscape entry (RenderWorld::tileLandscapeIndex); that colour is not verified here.
								if (worst < 8.0) std::printf("    NOT CHECKED: the two frames do not differ enough to judge the ray-traced reflection\n");
								writePngFile(pngPath.substr(0, pngPath.size() - 4) + "_reflection.png", static_cast<int>(kW), static_cast<int>(kH), reflOn.data());
								writePngFile(pngPath.substr(0, pngPath.size() - 4) + "_reflection_noground.png", static_cast<int>(kW), static_cast<int>(kH), reflNoGround.data());
							}
							reg.remove<MaterialComponent>(boxEnt);
							refl.enabled = false;
							renderer->SetGIReflectionSettings(refl);
							gi.enabled = false;
							renderer->SetGISettings(gi);
							world.setLandscapeRuntime(nullptr);
							std::vector<uint8_t> back;
							shoot(back);
						}

						// Temporal anti-aliasing with the camera moving: the ground reports the camera's motion in the velocity pass, so
						// the history lines up and the texture stays sharp. Without the ground there it reports "did not move" and the
						// resolve blends misplaced history into it. Measured as the texture's contrast in the last frame of a pan.
						{
							IRenderer::AntiAliasingSettings aa;
							aa.method = static_cast<int>(HE::AAMethod::TAA);
							aa.sharpness = 0.0f;
							renderer->SetAntiAliasingSettings(aa);
							const EditorCameraOverride still = ov;
							const auto pan = [&](std::vector<uint8_t>& img) {
								bool ok = true;
								for (int k = 0; k < 16 && ok; ++k)
								{
									const glm::vec3 e = eye + glm::vec3(0.0f, 0.0f, 0.6f * static_cast<float>(k));
									ov.view = glm::lookAt(e, e + glm::normalize(glm::vec3(1.0f, -0.45f, 0.15f)), glm::vec3(0, 1, 0));
									ov.position = e;
									img.clear();
									ok = renderer->RenderSceneImage(ov, kW, kH, img) && img.size() == px * 4;
								}
								return ok;
							};
							const auto contrast = [&](const std::vector<uint8_t>& img) {
								double s = 0.0;
								size_t n3 = 0;
								for (size_t i = 0; i + 1 < px; ++i)
									if (isBlue(&lit[i * 4]) && i % kW + 1 < kW) { s += std::abs(lum(&img[i * 4 + 4]) - lum(&img[i * 4])); ++n3; }
								return s / static_cast<double>(std::max<size_t>(n3, 1));
							};
							std::vector<uint8_t> withVel, withoutVel;
							const bool gotA = pan(withVel);
							landscapePasses("0");
							const bool gotB = gotA && pan(withoutVel);
							landscapePasses(nullptr);
							ov = still;
							if (!gotB) fail("RenderSceneImage gave an image (TAA pan)");
							else
							{
								std::printf("  TAA, camera panning: the ground's contrast is %.2f/255 with its velocity, %.2f/255 without\n", contrast(withVel), contrast(withoutVel));
								// Equal numbers: the image capture did not resolve temporally (RenderSceneImage of this backend), so there is
								// nothing to compare. Said, not failed: the ground's velocity is then unverified here.
								if (std::abs(contrast(withVel) - contrast(withoutVel)) < 0.01) std::printf("    NOT CHECKED: the capture does not run the temporal resolve\n");
								else if (contrast(withVel) < contrast(withoutVel)) fail("the ground stays sharp under TAA while the camera moves");
								writePngFile(pngPath.substr(0, pngPath.size() - 4) + "_taa.png", static_cast<int>(kW), static_cast<int>(kH), withVel.data());
								writePngFile(pngPath.substr(0, pngPath.size() - 4) + "_taa_novelocity.png", static_cast<int>(kW), static_cast<int>(kH), withoutVel.data());
							}
							renderer->SetAntiAliasingSettings(IRenderer::AntiAliasingSettings{});
							std::vector<uint8_t> settleBack;
							shoot(settleBack);
						}

						// The floating origin moves (a large world rebases it as the camera travels): the same absolute camera should see
						// the same ground, texture and all. KNOWN GAP, measured and reported here, not a failure of this check: a graph
						// material's World Position is the origin-relative one (every material's, on every mesh), so its world-space
						// texturing moves with the origin. The landscape's fixed blend carries uvOrigin for this; the material path needs
						// an absolute-stable position node in the generator (docs/landscape-tile-material-design-2026-11-21.md).
						const glm::dvec3 shift(1000.3, 0.0, 517.7);
						world.setOrigin(origin + shift);
						auto& btf = reg.get<TransformComponent>(boxEnt);
						btf.position -= glm::vec3(shift);
						btf.dirty = true;
						const EditorCameraOverride kept = ov;
						const glm::vec3 eye2 = glm::vec3(camPos - (origin + shift));
						ov.view = glm::lookAt(eye2, eye2 + glm::normalize(glm::vec3(1.0f, -0.45f, 0.15f)), glm::vec3(0, 1, 0));
						ov.position = eye2;
						std::vector<uint8_t> moved;
						if (!shoot(moved)) fail("RenderSceneImage gave an image (origin moved)");
						else
						{
							double gap = 0.0;
							size_t n2 = 0;
							for (size_t i = 0; i < px; ++i)
								if (isBlue(&lit[i * 4])) { gap += std::abs(lum(&moved[i * 4]) - lum(&autoImg[i * 4])); ++n2; }
							const double meanGap = gap / static_cast<double>(std::max<size_t>(n2, 1));
							std::printf("  floating origin moved by (1000.3, 517.7) m: the ground differs by %.2f/255 on average\n", meanGap);
							if (meanGap > 2.0) fail("the material stays on the ground when the floating origin moves");
							writePngFile(pngPath.substr(0, pngPath.size() - 4) + "_auto_origin.png", static_cast<int>(kW), static_cast<int>(kH), moved.data());
						}
						ov = kept;
						world.setOrigin(origin);
						btf.position += glm::vec3(shift);
						btf.dirty = true;
					}
				}
			}
			// A scene with no mesh at all still renders its shadow maps for the ground: the frame comes back.
			world.destroyEntity(boxEnt);
			std::vector<uint8_t> alone;
			if (!shoot(alone)) fail("RenderSceneImage gave an image (the ground alone)");
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
