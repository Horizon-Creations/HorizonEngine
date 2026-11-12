#include "doctest.h"
#include "MeshletCull.h"
#include <ContentManager/ContentManager.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

// Meshlet culling prototype (Thema 331, step 6): correctness of the tests against brute
// force on synthetic meshes, and (opt-in, needs the foliage test project) the probe that
// puts numbers on "what would meshlet culling save in the Foliage-Testszene".

namespace fs = std::filesystem;
using namespace MeshletCull;

namespace {

// A UV sphere, counter-clockwise from outside, radius 1 at the origin: a closed single-sided
// surface, the case the normal cone is made for.
StaticMeshAsset sphereMesh(int rings, int segments)
{
	StaticMeshAsset m;
	m.type = HE::AssetType::StaticMesh;
	for (int r = 0; r <= rings; ++r)
	{
		const float v = static_cast<float>(r) / rings;
		const float phi = v * 3.14159265f;
		for (int s = 0; s <= segments; ++s)
		{
			const float theta = static_cast<float>(s) / segments * 6.2831853f;
			const glm::vec3 p(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
			m.vertices.insert(m.vertices.end(), { p.x, p.y, p.z });
			m.normals.insert(m.normals.end(), { p.x, p.y, p.z });
			m.uvs.insert(m.uvs.end(), { static_cast<float>(s) / segments, v });
		}
	}
	const int stride = segments + 1;
	for (int r = 0; r < rings; ++r)
	{
		for (int s = 0; s < segments; ++s)
		{
			const uint32_t a = static_cast<uint32_t>(r * stride + s);
			const uint32_t b = a + 1;
			const uint32_t c = a + static_cast<uint32_t>(stride);
			const uint32_t d = c + 1;
			// Outward normal of (a, c, b) with these positions points away from the centre.
			m.indices.insert(m.indices.end(), { a, b, c, b, d, c });
		}
	}
	return m;
}

glm::vec3 posOf(const StaticMeshAsset& m, uint32_t i)
{
	return { m.vertices[i * 3], m.vertices[i * 3 + 1], m.vertices[i * 3 + 2] };
}

} // namespace

TEST_CASE("Meshlet culling: every triangle lands in exactly one meshlet, inside its bounding sphere")
{
	const StaticMeshAsset mesh = sphereMesh(24, 32);
	const Set set = build(mesh, 64, 124, 0.0f);
	REQUIRE(!set.meshlets.empty());

	uint32_t total = 0;
	for (const Meshlet& m : set.meshlets)
	{
		CHECK(m.triangleCount >= 1u);
		CHECK(m.triangleCount <= 124u);
		CHECK(m.vertexCount <= 64u);
		total += m.triangleCount;
		for (uint32_t t = 0; t < m.triangleCount; ++t)
			for (int k = 0; k < 3; ++k)
			{
				const glm::vec3 p = posOf(mesh, set.indices[(m.firstTriangle + t) * 3 + k]);
				CHECK(glm::length(p - m.center) <= m.radius * 1.0001f + 1e-5f);
			}
	}
	CHECK(total == set.sourceTriangles);
	CHECK(set.indices.size() == mesh.indices.size());

	// Same multiset of triangles (vertex triples, rotation-insensitive).
	auto key = [](uint32_t a, uint32_t b, uint32_t c) {
		std::array<uint32_t, 3> k{ a, b, c };
		std::rotate(k.begin(), std::min_element(k.begin(), k.end()), k.end());
		return k;
	};
	std::map<std::array<uint32_t, 3>, int> seen;
	for (size_t i = 0; i < mesh.indices.size(); i += 3)
		++seen[key(mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2])];
	for (size_t i = 0; i < set.indices.size(); i += 3)
		--seen[key(set.indices[i], set.indices[i + 1], set.indices[i + 2])];
	for (const auto& kv : seen)
		CHECK(kv.second == 0);
}

TEST_CASE("Meshlet culling: a frustum-culled meshlet has no vertex inside the frustum")
{
	const StaticMeshAsset mesh = sphereMesh(20, 28);
	const Set set = build(mesh, 64, 124, 0.0f);
	const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.6f, 0.1f, 100.0f);
	std::mt19937 rng(7);
	std::uniform_real_distribution<float> u(-1.0f, 1.0f);

	int culled = 0, kept = 0;
	for (int trial = 0; trial < 60; ++trial)
	{
		const glm::vec3 eye(u(rng) * 5.0f, u(rng) * 5.0f, u(rng) * 5.0f);
		glm::vec3 dir(u(rng), u(rng) * 0.3f, u(rng));
		if (glm::length(dir) < 0.1f) dir = glm::vec3(0, 0, 1);
		const glm::mat4 view = glm::lookAt(eye, eye + glm::normalize(dir), glm::vec3(0, 1, 0));
		const Frustum f = Frustum::fromViewProj(proj * view);
		const float s = 0.5f + (u(rng) + 1.0f);
		const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(u(rng), u(rng), u(rng)) * 10.0f)
		                      * glm::scale(glm::mat4(1.0f), glm::vec3(s));
		for (const Meshlet& m : set.meshlets)
		{
			if (meshletVisible(f, m, model, s)) { ++kept; continue; }
			++culled;
			for (uint32_t t = 0; t < m.triangleCount * 3; ++t)
			{
				const glm::vec3 w = glm::vec3(model * glm::vec4(posOf(mesh, set.indices[m.firstTriangle * 3 + t]), 1.0f));
				bool outside = false;
				for (const glm::vec4& p : f.plane)
					if (glm::dot(glm::vec3(p), w) + p.w < -1e-4f) { outside = true; break; }
				// A vertex of a culled meshlet may sit inside the sphere's far side of one
				// plane only if the sphere test were wrong: it must be outside SOME plane.
				CHECK(outside);
			}
		}
	}
	CHECK(culled > 0);
	CHECK(kept > 0);
}

TEST_CASE("Meshlet culling: a back-facing cone means every triangle faces away (single-sided surface)")
{
	const StaticMeshAsset mesh = sphereMesh(24, 32);
	const Set set = build(mesh, 64, 124, 0.5f);
	std::mt19937 rng(11);
	std::uniform_real_distribution<float> u(-1.0f, 1.0f);

	int back = 0;
	for (int trial = 0; trial < 40; ++trial)
	{
		const glm::vec3 eye = glm::normalize(glm::vec3(u(rng), u(rng), u(rng)) + glm::vec3(0.01f)) * (1.5f + std::abs(u(rng)) * 6.0f);
		for (const Meshlet& m : set.meshlets)
		{
			if (!meshletBackfacing(m, eye)) continue;
			++back;
			for (uint32_t t = 0; t < m.triangleCount; ++t)
			{
				const uint32_t* ix = &set.indices[(m.firstTriangle + t) * 3];
				const glm::vec3 a = posOf(mesh, ix[0]), b = posOf(mesh, ix[1]), c = posOf(mesh, ix[2]);
				const glm::vec3 n = glm::cross(b - a, c - a);
				CHECK(glm::dot(n, eye - a) <= 1e-4f);
			}
		}
	}
	CHECK(back > 0);
}

TEST_CASE("Meshlet culling: a mesh without triangles builds nothing")
{
	StaticMeshAsset empty;
	empty.type = HE::AssetType::StaticMesh;
	CHECK(build(empty).meshlets.empty());
}

// ─── probe on the real plants ────────────────────────────────────────────────────────────
// HE_MESHLET_PROBE=1 [HE_MESHLET_PROJECT=<foliage-test-project dir>]; silent skip otherwise.
// Rebuilds the base scene of docs/perf-audit/scenes/foliage-test-base.hescene (12 layers, a
// 12 m terrain each, density x 144 m2 plants, scale 0.8..1.3, drawDistance per layer) and asks,
// per camera: of the plants that survive draw distance + frustum (what the engine already
// culls per instance), how many meshlets / triangles would a meshlet test still remove?
namespace {

struct LayerSpec { const char* name; float cx, cz, density, minS, maxS, draw; };
const LayerSpec kLayers[] = {
	{ "fern_02",         -21, -14, 0.50f, 0.8f, 1.3f, 60 }, { "grass_medium_01",   -7, -14, 3.00f, 0.8f, 1.4f, 40 },
	{ "celandine_01",      7, -14, 2.00f, 0.8f, 1.3f, 40 }, { "dandelion_01",      21, -14, 1.50f, 0.8f, 1.3f, 40 },
	{ "weed_plant_02",   -21,   0, 1.50f, 0.8f, 1.3f, 40 }, { "nettle_plant",       -7,   0, 1.00f, 0.8f, 1.3f, 40 },
	{ "periwinkle_plant",  7,   0, 1.00f, 0.8f, 1.3f, 40 }, { "shrub_03",           21,   0, 1.00f, 0.8f, 1.3f, 40 },
	{ "shrub_04",        -21,  14, 0.80f, 0.8f, 1.3f, 50 }, { "anthurium_botany_01", -7, 14, 0.50f, 0.8f, 1.2f, 60 },
	{ "flower_gazania",    7,  14, 0.25f, 0.8f, 1.2f, 60 }, { "shrub_02",           21,  14, 0.15f, 0.8f, 1.2f, 80 },
};

struct CameraSpec { const char* name; glm::vec3 eye; glm::vec3 dir; };

struct Totals
{
	uint64_t plants = 0, inRange = 0, passInstance = 0, whole = 0;
	uint64_t meshletsTested = 0, meshletsPass = 0, meshletsPassCone = 0;
	uint64_t trisInstance = 0, trisMeshlet = 0, trisMeshletCone = 0;
	uint64_t runs = 0;              // draws a per-meshlet-range submission would need
	double   cpuMs = 0.0;
};

} // namespace

TEST_CASE("Meshlet culling probe: Foliage-Testszene (opt-in, HE_MESHLET_PROBE=1)")
{
	if (!std::getenv("HE_MESHLET_PROBE")) return;
	const char* proj = std::getenv("HE_MESHLET_PROJECT");
	const fs::path root = fs::path(proj ? proj : (std::string(std::getenv("HOME")) + "/HorizonEngineTestAssets/foliage-test-project"))
	                      / "Content";
	if (!fs::exists(root / "Models" / "Foliage")) { MESSAGE("no foliage test project, probe skipped"); return; }

	ContentManager cm(root.string());
	struct Plant { const StaticMeshAsset* mesh = nullptr; Set flat, coned; glm::vec3 boundCenter{ 0 }; float boundRadius = 0; };
	std::map<std::string, Plant> plants;
	for (const LayerSpec& L : kLayers)
	{
		const fs::path dir = root / "Models" / "Foliage" / L.name;
		Plant best;
		for (const auto& e : fs::directory_iterator(dir))
		{
			if (e.path().extension() != ".hasset") continue;
			const std::string rel = fs::relative(e.path(), root).generic_string();
			const HE::UUID id = cm.loadAsset(rel);
			const StaticMeshAsset* m = cm.getStaticMesh(id);
			if (m && m->indices.size() > (best.mesh ? best.mesh->indices.size() : 0)) best.mesh = m;
		}
		REQUIRE_MESSAGE(best.mesh != nullptr, L.name);
		best.flat  = build(*best.mesh, 64, 124, 0.0f);
		best.coned = build(*best.mesh, 64, 124, 0.5f);
		// Sphere of the whole mesh from the union of its meshlet spheres' AABB (what an
		// instance-level culler holds).
		glm::vec3 lo(1e30f), hi(-1e30f);
		for (const Meshlet& m : best.flat.meshlets)
		{
			lo = glm::min(lo, m.center - glm::vec3(m.radius));
			hi = glm::max(hi, m.center + glm::vec3(m.radius));
		}
		best.boundCenter = (lo + hi) * 0.5f;
		best.boundRadius = glm::length(hi - lo) * 0.5f;
		plants[L.name] = std::move(best);
		const Plant& p = plants[L.name];
		std::cerr << "[meshlet-probe] " << L.name << ": " << p.flat.sourceTriangles << " tris, "
		          << p.flat.meshlets.size() << " meshlets (64v/124t, cone 0), "
		          << p.coned.meshlets.size() << " (cone 0.5), mean "
		          << (p.flat.meshlets.empty() ? 0.0 : double(p.flat.sourceTriangles) / p.flat.meshlets.size()) << " tris/meshlet, "
		          << "size " << p.boundRadius * 2.0f << " m\n";
	}

	const float aspect = 2840.0f / 1528.0f;
	const glm::mat4 projM = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 500.0f);
	const CameraSpec cams[] = {
		{ "edge, looking over the field (baseline camera)", { 0, 2.5f, -24 }, glm::normalize(glm::vec3(0, -0.15f, 1)) },
		{ "inside the field, eye height 1.7 m",           { 0, 1.7f, -10 }, glm::vec3(0, 0, 1) },
		{ "inside the field, looking sideways",           { 0, 1.7f, -6 },  glm::vec3(1, 0, 0) },
	};

	for (const CameraSpec& cam : cams)
	{
		const glm::mat4 view = glm::lookAt(cam.eye, cam.eye + cam.dir, glm::vec3(0, 1, 0));
		const Frustum fr = Frustum::fromViewProj(projM * view);
		Totals all;
		std::cerr << "[meshlet-probe] camera: " << cam.name << "\n";
		for (const LayerSpec& L : kLayers)
		{
			const Plant& p = plants[L.name];
			Totals t;
			std::mt19937 rng(42);
			std::uniform_real_distribution<float> uPos(-6.0f, 6.0f), uYaw(0.0f, 6.2831853f), uS(L.minS, L.maxS);
			const int n = static_cast<int>(std::lround(L.density * 144.0f));
			const auto t0 = std::chrono::steady_clock::now();
			for (int i = 0; i < n; ++i)
			{
				const glm::vec3 pos(L.cx + uPos(rng), 0.0f, L.cz + uPos(rng));
				const float yaw = uYaw(rng), s = uS(rng);
				++t.plants;
				if (glm::length(glm::vec2(pos.x - cam.eye.x, pos.z - cam.eye.z)) > L.draw) continue;
				++t.inRange;
				const glm::mat4 model = glm::translate(glm::mat4(1.0f), pos)
				                      * glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0, 1, 0))
				                      * glm::scale(glm::mat4(1.0f), glm::vec3(s));
				if (!sphereVisible(fr, glm::vec3(model * glm::vec4(p.boundCenter, 1.0f)), p.boundRadius * s)) continue;
				++t.passInstance;
				t.trisInstance += p.flat.sourceTriangles;

				// Meshlet pass: sphere test per meshlet, runs counted in the meshlet-ordered index buffer.
				const glm::vec3 eyeObj = glm::vec3(glm::inverse(model) * glm::vec4(cam.eye, 1.0f));
				bool prev = false; uint64_t runs = 0; bool allIn = true;
				for (const Meshlet& m : p.flat.meshlets)
				{
					++t.meshletsTested;
					const bool vis = meshletVisible(fr, m, model, s);
					if (vis) { ++t.meshletsPass; t.trisMeshlet += m.triangleCount; if (!prev) ++runs; }
					else allIn = false;
					prev = vis;
				}
				for (const Meshlet& m : p.coned.meshlets)
					if (meshletVisible(fr, m, model, s) && !meshletBackfacing(m, eyeObj))
					{ ++t.meshletsPassCone; t.trisMeshletCone += m.triangleCount; }
				t.runs += runs;
				if (allIn) ++t.whole;
			}
			t.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			std::cerr << "[meshlet-probe]   " << L.name << ": plants " << t.plants << ", in range " << t.inRange
			          << ", pass instance cull " << t.passInstance << ", wholly inside " << t.whole
			          << ", tris " << t.trisInstance << " -> " << t.trisMeshlet << " (frustum meshlets) / "
			          << t.trisMeshletCone << " (+ single-sided cone, upper bound)\n";
			all.plants += t.plants; all.inRange += t.inRange; all.passInstance += t.passInstance; all.whole += t.whole;
			all.meshletsTested += t.meshletsTested; all.meshletsPass += t.meshletsPass; all.meshletsPassCone += t.meshletsPassCone;
			all.trisInstance += t.trisInstance; all.trisMeshlet += t.trisMeshlet; all.trisMeshletCone += t.trisMeshletCone;
			all.runs += t.runs; all.cpuMs += t.cpuMs;
		}
		const double saved = all.trisInstance ? 100.0 * double(all.trisInstance - all.trisMeshlet) / double(all.trisInstance) : 0.0;
		const double savedCone = all.trisInstance ? 100.0 * double(all.trisInstance - all.trisMeshletCone) / double(all.trisInstance) : 0.0;
		std::cerr << "[meshlet-probe]   TOTAL plants " << all.plants << " in range " << all.inRange << " pass instance cull " << all.passInstance
		          << " (partial " << all.passInstance - all.whole << "); meshlets tested " << all.meshletsTested << " pass " << all.meshletsPass
		          << "; tris " << all.trisInstance << " -> " << all.trisMeshlet << " (saves " << saved << " %), with cone "
		          << all.trisMeshletCone << " (saves " << savedCone << " %); draws for runs " << all.runs
		          << " vs " << all.passInstance << " instances; CPU " << all.cpuMs << " ms (one thread, incl. cone set)\n";
		CHECK(all.trisMeshlet <= all.trisInstance);
	}
}
