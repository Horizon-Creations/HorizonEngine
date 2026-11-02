// landscape_gpu_check — the world-scale landscape (Thema 326, step 2, part B) drawn by the real backend code, offscreen, and compared with
// geometry the checker knows.
//
//   landscape_gpu_check [--out <dir>] [--only gl|metal|d3d11|d3d12|vulkan] [--arrays <EngineContent root>] [--measure]
//
// Two layers per backend. (1) OFFSCREEN: the backend's landscape class (OpenGLLandscape, MetalLandscape) draws into a target of the tool's own,
// and colour and depth are compared with the plane, pixel by pixel. (2) ENGINE: the real renderer (RendererFactory, hidden window, the backend's
// scene pass, forward and deferred) draws the same world through IRenderer::SetLandscapeProvider and RenderSceneImage, and the picture is
// checked for ground where the plane is and sky where it is not (tone-mapped, so not for exact colours).
//
// Scene 1 ("ramp"): 32 x 32 tiles of the real geometry whose ground is the plane y = 600 + 0.1 * (x - 3072), all paint on layer 0. A plane is
// reproduced exactly at every level of detail, so the ground height at any pixel is known, and the quadtree's levels, the morph and the
// tile/coarse lookups cannot hide behind "close enough". Per pixel the checker intersects the camera ray with the plane and compares
//   * hole or extra: is something drawn where the plane is (within 5 km), nothing where it is not;
//   * depth: the distance to the drawn surface against the distance to the plane;
//   * colour: layer colour 0 times ambient + sun * n.l with the plane's normal.
// The camera then jumps 10 km, the store streams (the atlas holds ~190 of the 1024 tiles, so slots are released and reused) and the same
// checks run on the same images (the second frame's uploads and table updates are executed on top of the first). Scene 2 ("hills") is the engine's procedural fBm, drawn for the eye and checked only for "ground there".
// Writes landscape_<backend>_<scene>.png. Not a test: it needs a window system and a GPU, and it measures no time.
#include "EngineCheck.h"
#include "GpuCheck.h"

#include <ContentManager/ContentManager.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeRuntime.h>
#include <HorizonScene/LandscapeWater.h>
#include <HorizonScene/LandscapeWorldProxies.h>
#include <HorizonScene/LandscapeSource.h>
#include <HorizonScene/LandscapeTile.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <thread>

using namespace HE;

namespace
{
constexpr float  kMin = 0.0f, kScale = 3000.0f;
constexpr double kSlope = 0.1, kBase = 600.0, kPivot = 3072.0;
constexpr int    kTiles = 32;                     // the ramp world: 16.4 km, more than the atlas holds, so moving evicts and reuses slots
constexpr int    kHillsTiles = 12;
constexpr double kWorld = kTiles * 512.0;
constexpr int    kW = 480, kH = 270;

// height(x) = kBase + kSlope * (x - kPivot); all paint on layer 0
class RampSource final : public LandscapeTileSource
{
public:
	RampSource()
	{
		ProceduralLandscapeParams p;
		p.tilesX = p.tilesZ = kTiles;
		m_base = ProceduralLandscapeSource(p).generate({ 0, 0 });
		const int ch = m_base.geometry.channels;
		for (auto& level : m_base.weights)
			for (size_t t = 0; t + static_cast<size_t>(ch) <= level.size(); t += static_cast<size_t>(ch))
				for (int c = 0; c < ch; ++c) level[t + static_cast<size_t>(c)] = c == 0 ? 255 : 0;
	}
	const LandscapeGeometry& geometry() const override { return m_base.geometry; }
	int  tilesX() const override { return kTiles; }
	int  tilesZ() const override { return kTiles; }
	bool heightRange(float& minM, float& scaleM) const override { minM = kMin; scaleM = kScale; return true; }
	bool load(LandscapeTileKey key, int maxLevels, LandscapeTile& out) const override
	{
		if (key.x < 0 || key.z < 0 || key.x >= kTiles || key.z >= kTiles) return false;
		out = m_base;
		const int cells = out.geometry.hCells;
		const double spacing = out.geometry.tileM / cells;
		for (size_t level = 0; level < out.heights.size(); ++level)
		{
			const int dim = out.geometry.heightDim(static_cast<int>(level));
			for (int j = 0; j < dim; ++j)
				for (int i = 0; i < dim; ++i)
				{
					const double x = (static_cast<double>(key.x) * cells + static_cast<double>(i) * (1 << level)) * spacing;
					const double v = std::clamp((kBase + kSlope * (x - kPivot) - kMin) / kScale * 65535.0, 0.0, 65535.0);
					out.heights[level][static_cast<size_t>(j) * static_cast<size_t>(dim) + static_cast<size_t>(i)] = static_cast<uint16_t>(std::lround(v));
				}
		}
		if (maxLevels >= 0)
		{
			out.heights.resize(std::min<size_t>(out.heights.size(), static_cast<size_t>(maxLevels)));
			out.weights.resize(std::min<size_t>(out.weights.size(), static_cast<size_t>(maxLevels)));
		}
		return true;
	}

private:
	LandscapeTile m_base;
};

bool settle(LandscapeRuntime& rt, const glm::dvec3& cam, const glm::dvec3& origin, int ms = 120000)
{
	const auto t0 = std::chrono::steady_clock::now();
	for (;;)
	{
		rt.updateAt(cam, glm::vec3(0.0f), 5.0, origin);
		if (rt.store()->settled() && rt.atlas()->stats().used == rt.store()->residentKeys().size()) return true;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > ms) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

struct Camera
{
	glm::dvec3 pos;       // absolute
	glm::dvec3 dir;
	glm::dvec3 origin;    // the floating origin the frame is relative to
};

gpucheck::RenderIn makeInput(const Camera& c, bool depthIsNdc, LandscapeRuntime& rt)
{
	gpucheck::RenderIn in;
	in.width = kW;
	in.height = kH;
	in.depthIsNdc = depthIsNdc;
	in.origin = c.origin;
	const glm::vec3 eye = glm::vec3(c.pos - c.origin);
	const glm::mat4 view = glm::lookAt(eye, eye + glm::vec3(glm::normalize(c.dir)), glm::vec3(0, 1, 0));
	const glm::mat4 proj = glm::perspective(glm::radians(70.0f), static_cast<float>(kW) / static_cast<float>(kH), 2.0f, 20000.0f);
	in.viewProj = proj * view;
	const glm::dvec3 origin = c.origin;
	in.provider = rt.renderProvider([origin]() { return origin; });
	return in;
}

// The ray through a pixel centre, in absolute space.
struct Ray { glm::dvec3 o, d; };
Ray pixelRay(const gpucheck::RenderIn& in, int px, int py)
{
	const glm::dmat4 inv = glm::inverse(glm::dmat4(in.viewProj));
	const double nx = (px + 0.5) / in.width * 2.0 - 1.0, ny = 1.0 - (py + 0.5) / in.height * 2.0;
	const double zNear = -1.0;   // the GL-convention matrix: ndc z of the near plane
	glm::dvec4 a = inv * glm::dvec4(nx, ny, zNear, 1.0), b = inv * glm::dvec4(nx, ny, 1.0, 1.0);
	const glm::dvec3 pa = glm::dvec3(a) / a.w + in.origin, pb = glm::dvec3(b) / b.w + in.origin;
	return { pa, glm::normalize(pb - pa) };
}

// What each backend drew, kept to be compared with the other (the morph, the lookups and the shading are the same maths in GLSL and MSL).
std::map<std::string, gpucheck::RenderOut>& drawn(const char* scene)
{
	static std::map<std::string, std::map<std::string, gpucheck::RenderOut>> all;
	return all[scene];
}

struct Tally
{
	int failures = 0;
	void check(bool ok, const char* what, const std::string& detail = {})
	{
		if (!ok) { ++failures; std::printf("    FAIL  %s %s\n", what, detail.c_str()); }
	}
};

void writePng(const std::string& path, const gpucheck::RenderOut& out)
{
	gpucheck::writePngFile(path, out.width, out.height, out.rgba.data());
}

// The checks of scene 1 on one image.
void checkRamp(const char* label, const gpucheck::RenderIn& in, const gpucheck::RenderOut& out, const Camera& cam, uint64_t atlasUploads, uint64_t atlasReleases, Tally& tally)
{
	const float sunLen = glm::length(in.sunDir);
	const glm::dvec3 n = glm::normalize(glm::dvec3(-kSlope, 1.0, 0.0));
	const double ndl = std::max(glm::dot(n, glm::dvec3(in.sunDir / sunLen)), 0.0);
	const glm::vec4 layer0 = LandscapeRuntimeConfig::defaultLayerColors()[0];
	const glm::vec3 expectColor = glm::vec3(layer0) * (in.ambient + in.sunColor * static_cast<float>(ndl));

	int expectedHit = 0, holes = 0, extras = 0, depthChecked = 0, depthBad = 0, colourChecked = 0, colourBad = 0;
	double worstDepth = 0.0;
	int worstColour = 0;
	for (int py = 0; py < in.height; ++py)
		for (int px = 0; px < in.width; ++px)
		{
			const Ray r = pixelRay(in, px, py);
			// plane: y - kSlope * x = kBase - kSlope * kPivot
			const double den = r.d.y - kSlope * r.d.x;
			double t = -1.0;
			if (std::abs(den) > 1e-9) t = (kBase - kSlope * kPivot - r.o.y + kSlope * r.o.x) / den;
			bool hit = t > 0.0;
			glm::dvec3 p(0.0);
			if (hit)
			{
				p = r.o + r.d * t;
				hit = p.x >= 0.0 && p.x <= kWorld && p.z >= 0.0 && p.z <= kWorld && t < 20000.0 - 1.0;
			}
			const float zw = out.depth[static_cast<size_t>(py) * static_cast<size_t>(in.width) + static_cast<size_t>(px)];
			const bool drawn = zw < 1.0f;
			if (hit && t < 5000.0)
			{
				++expectedHit;
				if (!drawn) { ++holes; continue; }
				// distance to the drawn surface
				const double ndcz = in.depthIsNdc ? zw : zw * 2.0 - 1.0;
				const double nx = (px + 0.5) / in.width * 2.0 - 1.0, ny = 1.0 - (py + 0.5) / in.height * 2.0;
				glm::dvec4 q = glm::inverse(glm::dmat4(in.viewProj)) * glm::dvec4(nx, ny, ndcz, 1.0);
				const glm::dvec3 P = glm::dvec3(q) / q.w + in.origin;
				const double tm = glm::length(P - r.o);
				const double te = glm::length(p - r.o);
				if (te < 4000.0)
				{
					++depthChecked;
					const double err = std::abs(tm - te), tol = 0.35 + 3e-4 * te;
					worstDepth = std::max(worstDepth, err - tol);
					if (err > tol) ++depthBad;
				}
				if (te < 5000.0)   // out to where the coarse world's weights are read, not only the tiles'
				{
					++colourChecked;
					const uint8_t* c = &out.rgba[(static_cast<size_t>(py) * static_cast<size_t>(in.width) + static_cast<size_t>(px)) * 4];
					int worst = 0;
					for (int k = 0; k < 3; ++k) worst = std::max(worst, std::abs(static_cast<int>(c[k]) - static_cast<int>(std::lround(std::clamp(expectColor[k], 0.0f, 1.0f) * 255.0f))));
					worstColour = std::max(worstColour, worst);
					if (worst > 4) ++colourBad;
				}
			}
			else if (!hit && drawn)
				++extras;
		}
	std::printf("  %-26s patches %llu in %llu draws, uploads %llu (atlas %llu), expected-hit px %d: holes %d, extras %d | depth %d px, bad %d | colour %d px, bad %d (worst %d/255)\n",
	            label, static_cast<unsigned long long>(out.patches), static_cast<unsigned long long>(out.drawCalls), static_cast<unsigned long long>(out.uploads),
	            static_cast<unsigned long long>(atlasUploads), expectedHit, holes, extras, depthChecked, depthBad, colourChecked, colourBad, worstColour);
	(void)cam;
	std::printf("  %-26s atlas releases so far: %llu\n", "", static_cast<unsigned long long>(atlasReleases));
	tally.check(expectedHit > in.width * in.height / 10, "the camera sees the ground", std::to_string(expectedHit) + " px");
	tally.check(holes <= expectedHit / 500, "no holes in the ground", std::to_string(holes) + " px");
	tally.check(extras <= in.width * in.height / 500, "nothing drawn where there is no ground", std::to_string(extras) + " px");
	tally.check(depthBad <= depthChecked / 500 && depthChecked > 100, "the drawn surface is the plane", std::to_string(depthBad) + " of " + std::to_string(depthChecked) + " px, worst excess " + std::to_string(worstDepth) + " m");
	tally.check(colourBad <= colourChecked / 200 && colourChecked > 100, "the paint and the light are right", std::to_string(colourBad) + " of " + std::to_string(colourChecked) + " px");
	tally.check(out.uploads == atlasUploads, "every upload the runtime counted was executed");
	tally.check(out.rejected == 0, "no upload or table update was refused");
	tally.check(out.error.empty(), "no GL error", out.error);
}

int runBackend(gpucheck::Backend& be, const std::string& packPath, const std::filesystem::path& outDir)
{
	Tally tally;
	std::printf("[%s]\n", be.name());

	uint64_t lastUploads = 0;
	// ── scene 1: the ramp ──
	{
		LandscapeRuntime rt;
		LandscapeRuntimeConfig cfg;
		cfg.store.byteBudget = 80ull * 1024 * 1024;     // about 190 slots, not the 836-slot default
		std::string err;
		if (!rt.beginFromPack(packPath, cfg, &err)) { std::printf("  runtime: %s\n", err.c_str()); return 1; }
		const bool z1 = be.depthIsNdc();
		Camera a{ { 2600.0, 720.0, 3072.0 }, { 1.0, -0.12, 0.2 }, { 2048.0, 100.0, 1024.0 } };
		if (!settle(rt, a.pos, a.origin)) { std::printf("  the store did not settle\n"); return 1; }
		gpucheck::RenderIn in = makeInput(a, z1, rt);
		gpucheck::RenderOut out;
		if (!be.render(in, out)) { std::printf("  render failed: %s\n", out.error.c_str()); return 1; }
		const uint64_t baseline = out.uploads;   // 0 in the first scene; the backend counts across scenes
		checkRamp("ramp, first position", in, out, a, rt.atlas()->stats().uploads, rt.atlas()->stats().releases, tally);
		writePng((outDir / (std::string("landscape_") + be.name() + "_ramp.png")).string(), out);
		drawn("ramp")[be.name()] = out;

		// a tile and a half on: the store streams, the second frame's uploads go on top of the first's
		Camera b{ { 12000.0, 1640.0, 9000.0 }, { -0.8, -0.1, 0.5 }, { 11264.0, 1000.0, 8192.0 } };
		if (!settle(rt, b.pos, b.origin)) { std::printf("  the store did not settle (2)\n"); return 1; }
		in = makeInput(b, z1, rt);
		if (!be.render(in, out)) { std::printf("  render failed: %s\n", out.error.c_str()); return 1; }
		lastUploads = out.uploads;
		checkRamp("ramp, camera jumped 10 km", in, out, b, rt.atlas()->stats().uploads, rt.atlas()->stats().releases, tally);
		tally.check(rt.atlas()->stats().releases > 0, "the jump released slots (the streaming path ran)");
		(void)baseline;
	}

	// ── scene 2: hills, for the eye ──
	{
		const uint64_t uploadsBefore = lastUploads;
		ProceduralLandscapeParams p;
		p.tilesX = p.tilesZ = kHillsTiles;
		auto src = std::make_shared<ProceduralLandscapeSource>(p);
		LandscapeRuntime rt;
		LandscapeRuntimeConfig cfg;
		cfg.store.byteBudget = 80ull * 1024 * 1024;
		std::string err;
		if (!rt.begin(src, cfg, &err)) { std::printf("  runtime: %s\n", err.c_str()); return 1; }
		Camera c{ { 1200.0, 2600.0, 1200.0 }, { 0.7, -0.22, 0.7 }, { 0.0, 0.0, 0.0 } };
		if (!settle(rt, c.pos, c.origin)) { std::printf("  the store did not settle (hills)\n"); return 1; }
		gpucheck::RenderIn in = makeInput(c, be.depthIsNdc(), rt);
		in.width = 960;
		in.height = 540;
		gpucheck::RenderOut out;
		if (!be.render(in, out)) { std::printf("  render failed: %s\n", out.error.c_str()); return 1; }
		int ground = 0;
		for (float d : out.depth) ground += d < 1.0f ? 1 : 0;
		std::printf("  %-26s patches %llu in %llu draws, uploads %llu, ground %.1f %% of the image\n", "hills", static_cast<unsigned long long>(out.patches),
		            static_cast<unsigned long long>(out.drawCalls), static_cast<unsigned long long>(out.uploads), 100.0 * ground / static_cast<double>(out.depth.size()));
		tally.check(ground > static_cast<int>(out.depth.size()) / 5, "the hills fill part of the image");
		tally.check(out.uploads - uploadsBefore == rt.atlas()->stats().uploads, "every upload the runtime counted was executed");
		writePng((outDir / (std::string("landscape_") + be.name() + "_hills.png")).string(), out);
		drawn("hills")[be.name()] = out;
	}
	// ── scene 3: the ground in world coordinates (the albedo array of a texture group) ──
	{
		// the ramp world: every texel is 100 % layer 0, so the ground shows slice 0 and nothing else
		LandscapeRuntime rt;
		LandscapeRuntimeConfig cfg;
		cfg.store.byteBudget = 80ull * 1024 * 1024;
		cfg.textureGroup = 0;            // the frame then names the group's albedo array; what the backend binds is the tool's own
		cfg.textureMeters = 8.0f;
		std::string err;
		if (!rt.beginFromPack(packPath, cfg, &err)) { std::printf("  runtime: %s\n", err.c_str()); return 1; }
		// slices 0-3: stripes (left half / right half of a tile) in colours nothing else in the picture has
		gpucheck::TexArray tex;
		tex.size = 64;
		tex.layers = 4;
		tex.rgba.resize(static_cast<size_t>(tex.size) * static_cast<size_t>(tex.size) * 4u * 4u);
		const uint8_t colours[4][2][3] = { { { 255, 0, 0 }, { 255, 255, 0 } }, { { 0, 255, 0 }, { 0, 255, 255 } }, { { 0, 0, 255 }, { 255, 0, 255 } }, { { 255, 255, 255 }, { 0, 0, 0 } } };
		for (int l = 0; l < 4; ++l)
			for (int y = 0; y < tex.size; ++y)
				for (int x = 0; x < tex.size; ++x)
				{
					uint8_t* px = &tex.rgba[((static_cast<size_t>(l) * static_cast<size_t>(tex.size) + static_cast<size_t>(y)) * static_cast<size_t>(tex.size) + static_cast<size_t>(x)) * 4u];
					const uint8_t* c = colours[l][x < tex.size / 2 ? 0 : 1];
					px[0] = c[0]; px[1] = c[1]; px[2] = c[2]; px[3] = 255;
				}
		Camera c{ { 2600.0, 720.0, 3072.0 }, { 1.0, -0.12, 0.2 }, { 2048.0, 100.0, 1024.0 } };
		if (!settle(rt, c.pos, c.origin)) { std::printf("  the store did not settle (world uv)\n"); return 1; }
		const auto shot = [&](const Camera& cam, const gpucheck::TexArray* array, gpucheck::RenderOut& out) {
			gpucheck::RenderIn in = makeInput(cam, be.depthIsNdc(), rt);
			in.albedo = array;
			return be.render(in, out);
		};
		gpucheck::RenderOut flat, textured, shifted;
		if (!shot(c, nullptr, flat) || !shot(c, &tex, textured)) { std::printf("  render failed (world uv)\n"); return 1; }
		size_t ground = 0, changed = 0, red = 0, yellow = 0, flatGreen = 0;
		for (size_t i = 0; i < flat.depth.size(); ++i)
		{
			if (flat.depth[i] >= 1.0f) continue;
			++ground;
			int worst = 0;
			for (int k = 0; k < 3; ++k) worst = std::max(worst, std::abs(static_cast<int>(flat.rgba[i * 4 + static_cast<size_t>(k)]) - static_cast<int>(textured.rgba[i * 4 + static_cast<size_t>(k)])));
			changed += worst > 16 ? 1 : 0;
			const int r = textured.rgba[i * 4], g = textured.rgba[i * 4 + 1], b = textured.rgba[i * 4 + 2];
			// slice 0 is red | yellow, lit: no blue at all; the flat layer 0 is green with blue in it
			if (b < 6 && r > 30) { (g < 6 ? red : yellow) += 1; }
			if (flat.rgba[i * 4 + 1] > flat.rgba[i * 4] && flat.rgba[i * 4 + 2] > 6) ++flatGreen;
		}
		std::printf("  %-26s %zu of %zu ground pixels differ from the flat colours by more than 16/255, red %zu, yellow %zu\n", "world uv, textured", changed, ground, red, yellow);
		tally.check(ground > flat.depth.size() / 4, "the ramp fills part of the image (world uv)");
		tally.check(flatGreen * 10 > ground * 9, "without the array the ground is the flat green");
		tally.check(changed * 10 > ground * 9, "the albedo array changes the ground");
		tally.check(red * 20 > ground && yellow * 20 > ground, "both halves of the slice's stripe show");
		// the same absolute view from another floating origin: the texture must stay where it is in the world
		Camera c2 = c;
		c2.origin = { 1000.37, 0.0, -512.11 };
		if (!settle(rt, c2.pos, c2.origin)) { std::printf("  the store did not settle (world uv, shifted origin)\n"); return 1; }
		if (!shot(c2, &tex, shifted)) { std::printf("  render failed (world uv, shifted origin)\n"); return 1; }
		size_t differing = 0;
		for (size_t i = 0; i + 3 < textured.rgba.size(); i += 4)
		{
			int worst = 0;
			for (int k = 0; k < 3; ++k) worst = std::max(worst, std::abs(static_cast<int>(textured.rgba[i + static_cast<size_t>(k)]) - static_cast<int>(shifted.rgba[i + static_cast<size_t>(k)])));
			differing += worst > 16 ? 1 : 0;
		}
		std::printf("  %-26s %zu pixels more than 16/255 apart from the same view with the origin moved\n", "world uv, origin moved", differing);
		tally.check(differing * 500 <= textured.rgba.size() / 4, "the texture stays put when the floating origin moves");
		// Normal and mask arrays of the group: a normal leaning towards the sun lights the ground more, away from it less; a mask with AO 0.5 darkens the ambient part.
		const auto solid = [](uint8_t r, uint8_t g, uint8_t b) {
			gpucheck::TexArray t;
			t.size = 4;
			t.layers = 4;
			t.rgba.resize(4u * 4u * 4u * 4u);
			for (size_t i = 0; i < t.rgba.size(); i += 4) { t.rgba[i] = r; t.rgba[i + 1] = g; t.rgba[i + 2] = b; t.rgba[i + 3] = 255; }
			return t;
		};
		const auto meanLum = [&](const gpucheck::RenderOut& o) {
			double sum = 0.0;
			size_t n = 0;
			for (size_t i = 0; i < flat.depth.size(); ++i)
			{
				if (flat.depth[i] >= 1.0f) continue;
				sum += 0.3 * o.rgba[i * 4] + 0.59 * o.rgba[i * 4 + 1] + 0.11 * o.rgba[i * 4 + 2];
				++n;
			}
			return n ? sum / static_cast<double>(n) : 0.0;
		};
		// the sun is at (+0.45, +0.8, +0.55): +x and +z lean towards it. Tangent space is OpenGL's: red = +u = +x, green = +v = +z.
		const gpucheck::TexArray nFlat = solid(128, 128, 255), nPlusZ = solid(128, 204, 230), nMinusZ = solid(128, 51, 230), nPlusX = solid(204, 128, 230), nMinusX = solid(51, 128, 230);
		const gpucheck::TexArray ao1 = solid(255, 128, 128), ao05 = solid(128, 128, 128);
		const auto lum = [&](const gpucheck::TexArray* normal, const gpucheck::TexArray* mask, double& out) {
			gpucheck::RenderIn in = makeInput(c, be.depthIsNdc(), rt);
			in.albedo = &tex;
			in.normal = normal;
			in.mask = mask;
			gpucheck::RenderOut o;
			if (!be.render(in, o)) return false;
			out = meanLum(o);
			return true;
		};
		double lFlat = 0, lPz = 0, lMz = 0, lPx = 0, lMx = 0, lNoAo = 0, lAo = 0, lNone = 0;
		if (!lum(nullptr, nullptr, lNone) || !lum(&nFlat, nullptr, lFlat) || !lum(&nPlusZ, nullptr, lPz) || !lum(&nMinusZ, nullptr, lMz) || !lum(&nPlusX, nullptr, lPx) ||
		    !lum(&nMinusX, nullptr, lMx) || !lum(nullptr, &ao1, lNoAo) || !lum(nullptr, &ao05, lAo))
		{ std::printf("  render failed (world uv maps)\n"); return 1; }
		std::printf("  %-26s mean luminance: no maps %.2f, flat normal %.2f, +z %.2f, -z %.2f, +x %.2f, -x %.2f; AO 1 %.2f, AO 0.5 %.2f\n", "world uv, normal + mask", lNone, lFlat, lPz, lMz, lPx, lMx, lNoAo, lAo);
		tally.check(std::abs(lFlat - lNone) < 1.0, "a flat normal array changes nothing");
		tally.check(lPz > lFlat + 3.0 && lMz < lFlat - 3.0, "a normal leaning to +z / -z lights the ground more / less (sun at +z)");
		tally.check(lPx > lFlat + 3.0 && lMx < lFlat - 3.0, "a normal leaning to +x / -x lights the ground more / less (sun at +x)");
		tally.check(std::abs(lNoAo - lNone) < 1.0, "an AO of 1 changes nothing");
		tally.check(lAo < lNone - 1.0, "an AO of 0.5 darkens the ambient light");
		writePng((outDir / (std::string("landscape_") + be.name() + "_worlduv.png")).string(), textured);
		drawn("worlduv")[be.name()] = textured;
	}
	std::printf("  %s\n", tally.failures == 0 ? "OK" : "FAILED");
	return tally.failures == 0 ? 0 : 1;
}

// ── --measure ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Series
{
	std::vector<double> v;
	void add(double x) { v.push_back(x); }
	double pct(double p) const
	{
		if (v.empty()) return 0.0;
		std::vector<double> s = v;
		std::sort(s.begin(), s.end());
		return s[std::min(s.size() - 1, static_cast<size_t>(p * static_cast<double>(s.size())))];
	}
	double mean() const { double a = 0; for (double x : v) a += x; return v.empty() ? 0.0 : a / static_cast<double>(v.size()); }
	void print(const char* what) const { std::printf("  %-34s p50 %7.3f ms   p95 %7.3f ms   max %7.3f ms   mean %7.3f ms\n", what, pct(0.5), pct(0.95), pct(1.0 - 1e-9), mean()); }
};

// One backend, a camera flying over a 12 km procedural world at 40 m/s, 1440p: the CPU side of the landscape (the runtime's update, the frame it builds,
// recording the draws) and the GPU's time for the landscape pass, per frame. Paced like a 60 fps game, so the store streams at the rate it would.
int runMeasure(gpucheck::Backend& be)
{
	std::printf("[%s, measured]\n", be.name());
	ProceduralLandscapeParams p;
	p.tilesX = p.tilesZ = 24;
	auto src = std::make_shared<ProceduralLandscapeSource>(p);
	LandscapeRuntime rt;
	LandscapeRuntimeConfig cfg;
	std::string err;
	if (!rt.begin(src, cfg, &err)) { std::printf("  runtime: %s\n", err.c_str()); return 1; }
	glm::dvec3 pos{ 2500.0, 1900.0, 2500.0 };
	const glm::dvec3 dir = glm::normalize(glm::dvec3(1.0, -0.08, 0.35));
	const glm::dvec3 step = glm::normalize(glm::dvec3(dir.x, 0.0, dir.z)) * 40.0 / 60.0;
	if (!settle(rt, pos, glm::dvec3(0.0))) { std::printf("  the store did not settle\n"); return 1; }
	Series update, provider, encode, gpu, patches, uploads;
	uint64_t lastUploads = 0;
	const int warm = 30, frames = 600;
	for (int f = 0; f < warm + frames; ++f)
	{
		const auto frameStart = std::chrono::steady_clock::now();
		const auto t0 = std::chrono::steady_clock::now();
		rt.updateAt(pos, glm::vec3(step * 60.0), 1.0, glm::dvec3(0.0));
		const double upMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		Camera c{ pos, dir, { 0.0, 0.0, 0.0 } };
		gpucheck::RenderIn in = makeInput(c, be.depthIsNdc(), rt);
		in.width = 2560;
		in.height = 1440;
		in.timing = true;
		gpucheck::RenderOut out;
		if (!be.render(in, out)) { std::printf("  render failed: %s\n", out.error.c_str()); return 1; }
		if (f >= warm)
		{
			update.add(upMs); provider.add(out.providerMs); encode.add(out.encodeMs); gpu.add(out.gpuMs);
			patches.add(static_cast<double>(out.patches)); uploads.add(static_cast<double>(out.uploads - lastUploads));
		}
		lastUploads = out.uploads;
		pos += step;
		const auto spent = std::chrono::steady_clock::now() - frameStart;
		if (spent < std::chrono::microseconds(16667)) std::this_thread::sleep_for(std::chrono::microseconds(16667) - std::chrono::duration_cast<std::chrono::microseconds>(spent));
	}
	std::printf("  %d frames at 2560 x 1440, %.0f m flown, %zu tiles resident, %.0f patches a frame (max %.0f), %.2f tile uploads a frame\n", frames, 40.0 * frames / 60.0,
	            rt.store()->residentKeys().size(), patches.mean(), patches.pct(1.0 - 1e-9), uploads.mean());
	update.print("runtime update (CPU)");
	provider.print("build the frame (CPU)");
	encode.print("record the draws (CPU)");
	Series cpu;
	for (size_t i = 0; i < update.v.size(); ++i) cpu.add(update.v[i] + provider.v[i] + encode.v[i]);
	cpu.print("landscape CPU, the three together");
	gpu.print(std::string(be.name()) == "Metal" ? "landscape GPU pass (Metal GPU time)" : "landscape pass wall time (GL, glFinish)");
	return 0;
}

// The CPU side of foliage and water: the runtime streaming them and the proxies that make them entities, on the main thread, per frame.
int runMeasureScene()
{
	std::printf("[foliage and water, CPU]\n");
	ProceduralLandscapeParams p;
	p.tilesX = p.tilesZ = 8;
	p.seaLevelM = 1500.0f;
	auto src = std::make_shared<ProceduralLandscapeSource>(p);
	LandscapeRuntime rt;
	LandscapeRuntimeConfig cfg;
	std::string err;
	if (!rt.begin(src, cfg, &err)) { std::printf("  runtime: %s\n", err.c_str()); return 1; }
	FoliageLayer grass;
	grass.meshAssetId = HE::UUID{ 1, 0 };
	grass.density = 0.05f;
	grass.drawDistance = 120.0f;
	FoliageLayer trees = grass;
	trees.density = 0.002f;
	trees.drawDistance = 1000.0f;
	trees.seed = 9;
	rt.setFoliageLayers({ grass, trees });
	HorizonWorld world;
	ContentManager content{ "." };
	LandscapeWorldProxies proxies;
	glm::dvec3 pos{ 1800.0, 1700.0, 1800.0 };
	const glm::dvec3 step = glm::normalize(glm::dvec3(1.0, 0.0, 0.35)) * 20.0 / 60.0;
	const auto t0 = std::chrono::steady_clock::now();
	for (;;)
	{
		rt.updateAt(pos, glm::vec3(0.0f), 5.0, glm::dvec3(0.0));
		std::vector<LandscapeFoliageDraw> d;
		rt.foliageDraws(d);
		if (!d.empty() && rt.waterRegion() && !rt.water().stats().building) break;
		if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(120)) { std::printf("  no foliage or water in 120 s\n"); return 1; }
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Series update, sync;
	for (int f = 0; f < 600; ++f)
	{
		const auto frameStart = std::chrono::steady_clock::now();
		const auto a = std::chrono::steady_clock::now();
		rt.updateAt(pos, glm::vec3(step * 60.0), 1.0, glm::dvec3(0.0));
		const auto b = std::chrono::steady_clock::now();
		proxies.sync(world, rt, content, nullptr);
		const auto c = std::chrono::steady_clock::now();
		if (f >= 30) { update.add(std::chrono::duration<double, std::milli>(b - a).count()); sync.add(std::chrono::duration<double, std::milli>(c - b).count()); }
		pos += step;
		const auto spent = std::chrono::steady_clock::now() - frameStart;
		if (spent < std::chrono::microseconds(16667)) std::this_thread::sleep_for(std::chrono::microseconds(16667) - std::chrono::duration_cast<std::chrono::microseconds>(spent));
	}
	std::printf("  600 frames, 20 m/s, %zu foliage entities, %zu water entities, %zu entities made over the run, %llu plants held\n", proxies.stats().foliageEntities, proxies.stats().waterEntities,
	            proxies.stats().created, static_cast<unsigned long long>(rt.foliage().stats().instances));
	update.print("runtime update with foliage + water");
	sync.print("LandscapeWorldProxies::sync");
	return 0;
}
} // namespace

namespace gpucheck
{
void writePngFile(const std::string& path, int width, int height, const uint8_t* rgba) { stbi_write_png(path.c_str(), width, height, 4, rgba, width * 4); }
} // namespace gpucheck

int main(int argc, char** argv)
{
	std::filesystem::path outDir = ".";
	std::string only;
	bool measure = false;    // --measure: only time the landscape (CPU and GPU) instead of checking pictures
	std::string arrayRoot;   // --arrays <EngineContent root>: also draw the ground from the Forest albedo array found there
	for (int i = 1; i < argc; ++i)
	{
		if (!std::strcmp(argv[i], "--out") && i + 1 < argc) outDir = argv[++i];
		else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
		else if (!std::strcmp(argv[i], "--arrays") && i + 1 < argc) arrayRoot = argv[++i];
		else if (!std::strcmp(argv[i], "--measure")) measure = true;
	}
	std::filesystem::create_directories(outDir);

	// the ramp world, once, as a pack
	const std::string pack = (std::filesystem::temp_directory_path() / ("he_landscape_gpu_check_" + std::to_string(std::random_device{}()) + ".bin")).string();
	{
		RampSource src;
		LandscapePackWriter w;
		if (!w.begin(pack, src.geometry(), kTiles, kTiles, 4, kMin, kScale)) { std::printf("pack: begin failed\n"); return 2; }
		for (int z = 0; z < kTiles; ++z)
			for (int x = 0; x < kTiles; ++x)
			{
				LandscapeTile t;
				if (!src.load({ x, z }, -1, t) || !w.add({ x, z }, encodeLandscapeTile(t))) { std::printf("pack: tile (%d, %d) failed\n", x, z); return 2; }
			}
		if (!w.finish()) { std::printf("pack: finish failed\n"); return 2; }
	}

	if (measure)
	{
		int mrc = 0;
		const auto go = [&](std::unique_ptr<gpucheck::Backend> be) {
			std::string err;
			if (!be->init(err)) { std::printf("[%s] cannot start here: %s\n", be->name(), err.c_str()); mrc = std::max(mrc, 3); return; }
			mrc = std::max(mrc, runMeasure(*be));
		};
		if (only.empty() || only == "gl") go(gpucheck::makeGlBackend());
#ifdef HE_CHECK_METAL
		if (only.empty() || only == "metal") go(gpucheck::makeMetalBackend());
#endif
#ifdef HE_CHECK_VULKAN
		if (only.empty() || only == "vulkan") go(gpucheck::makeVulkanBackend());
#endif
		if (only.empty() || only == "scene") mrc = std::max(mrc, runMeasureScene());
		std::filesystem::remove(pack);
		return mrc;
	}
	int rc = 0, ran = 0;
	const auto start = [&](std::unique_ptr<gpucheck::Backend> be) {
		std::string err;
		if (!be->init(err)) { std::printf("[%s] cannot start here: %s\n", be->name(), err.c_str()); rc = std::max(rc, 3); return; }
		++ran;
		rc = std::max(rc, runBackend(*be, pack, outDir));
	};
	// Each backend in turn, and its offscreen check gone before the real renderer starts: the engine's window owns SDL (it quits it).
	const auto engine = [&](HE::RendererBackend backend, const char* name) {
		for (auto path : { HE::RenderPath::Forward, HE::RenderPath::Deferred })
		{
			++ran;
			rc = std::max(rc, gpucheck::runEngineCheck(backend, path, pack,
			                                           (outDir / (std::string("landscape_") + name + "_engine_" + (path == HE::RenderPath::Deferred ? "deferred" : "forward") + ".png")).string()));
		}
		if (!arrayRoot.empty())
		{
			++ran;
			rc = std::max(rc, gpucheck::runEngineCheck(backend, HE::RenderPath::Forward, pack, (outDir / (std::string("landscape_") + name + "_engine_worlduv.png")).string(), arrayRoot));
		}
	};
	if (only.empty() || only == "gl")
	{
		start(gpucheck::makeGlBackend());
		engine(HE::RendererBackend::OpenGL, "OpenGL");
	}
#ifdef HE_CHECK_METAL
	if (only.empty() || only == "metal")
	{
		start(gpucheck::makeMetalBackend());
		engine(HE::RendererBackend::Metal, "Metal");
	}
#endif
#ifdef HE_CHECK_D3D11
	if (only.empty() || only == "d3d11")
	{
		start(gpucheck::makeD3D11Backend());
		engine(HE::RendererBackend::D3D11, "D3D11");
	}
#endif
#ifdef HE_CHECK_D3D12
	if (only.empty() || only == "d3d12")
	{
		start(gpucheck::makeD3D12Backend());
		engine(HE::RendererBackend::D3D12, "D3D12");
	}
#endif
#ifdef HE_CHECK_VULKAN
	if (only.empty() || only == "vulkan")
	{
		start(gpucheck::makeVulkanBackend());
		engine(HE::RendererBackend::Vulkan, "Vulkan");
	}
#endif
	// One backend against another on the same frames: the same shader maths in two languages must give the same picture. A plane morphs to itself, so the
	// hills are the only place a wrong morph or a wrong lookup in ONE of the ports shows; a fraction above 0.1 % of the pixels (more than 2/255
	// apart) is a difference between the ports, not rounding. (Crack-free seams and popping are not judged here: Plan section 4, on NN-WS03.)
	// OpenGL is the reference of everything it is run beside; D3D12 is also held against D3D11, the two share one HLSL source and the pair should be near-identical.
	const std::pair<const char*, const char*> pairs[] = { { "OpenGL", "Metal" }, { "OpenGL", "D3D11" }, { "OpenGL", "D3D12" }, { "OpenGL", "Vulkan" }, { "D3D11", "D3D12" } };
	for (const auto& [refName, otherName] : pairs)
		for (const char* scene : { "ramp", "hills", "worlduv" })
		{
			auto& m = drawn(scene);
			if (m.count(refName) == 0 || m.count(otherName) == 0) continue;
			const gpucheck::RenderOut& a = m[refName];
			const gpucheck::RenderOut& b = m[otherName];
			if (a.rgba.size() != b.rgba.size() || a.rgba.empty()) { std::printf("[%s vs %s] %s: sizes differ\n", refName, otherName, scene); rc = std::max(rc, 1); continue; }
			size_t differing = 0, far = 0;
			for (size_t i = 0; i + 3 < a.rgba.size(); i += 4)
			{
				int worst = 0;
				for (int k = 0; k < 3; ++k) worst = std::max(worst, std::abs(static_cast<int>(a.rgba[i + static_cast<size_t>(k)]) - static_cast<int>(b.rgba[i + static_cast<size_t>(k)])));
				differing += worst > 2 ? 1 : 0;
				far += worst > 16 ? 1 : 0;
			}
			const size_t pixels = a.rgba.size() / 4;
			const bool ok = differing * 1000 <= pixels;
			std::printf("[%s vs %s] %-6s %zu of %zu pixels more than 2/255 apart (%zu more than 16/255)  %s\n", refName, otherName, scene, differing, pixels, far, ok ? "OK" : "FAILED");
			if (!ok) rc = std::max(rc, 1);
		}
	std::filesystem::remove(pack);
	if (ran == 0) { std::printf("no backend ran\n"); return 3; }
	return rc;
}
