#include "doctest.h"
#include <Renderer/IRenderer.h>
#include <HorizonScene/Components/HierarchyComponent.h>

#include <Math/AABB.h>
#include <HorizonRendering/FrustumCuller.h>
#include <HorizonRendering/RenderSorter.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/CommandBuffer.h>
#include <HorizonRendering/SsaoKernel.h>
#include <HorizonRendering/SkyNoise3D.h>
#include <HorizonRendering/SkyEnvBake.h>
#include <HorizonRendering/WeatherParticleSeed.h>
#include <HorizonRendering/SkyFrameParams.h>
#include <HorizonRendering/LightPacking.h>
#include <HorizonScene/HorizonWorld.h>
#include "TestFsUtil.h"
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

TEST_CASE("RenderExtractor: real mesh bounds when a ContentManager is set, invalid (kept visible) otherwise")
{
	// A mesh whose real bounds are a 4-unit box (much bigger than the unit cube).
	ContentManager cm;
	StaticMeshAsset mesh; mesh.type = HE::AssetType::StaticMesh; mesh.name = "big";
	mesh.indices = {0,1,2};
	mesh.boundsMin[0] = mesh.boundsMin[1] = mesh.boundsMin[2] = -2.0f;
	mesh.boundsMax[0] = mesh.boundsMax[1] = mesh.boundsMax[2] =  2.0f;
	const HE::UUID meshId = cm.registerStaticMesh(mesh);

	HorizonWorld world;
	auto e = world.createEntity("obj");
	world.registry().emplace<TransformComponent>(e, TransformComponent{}); // identity
	MeshComponent mc; mc.meshAssetId = meshId;
	world.registry().emplace<MeshComponent>(e, mc);

	// With the ContentManager, worldBounds should be the real 4-unit box.
	{
		RenderExtractor ex;
		ex.setContentManager(&cm);
		RenderWorld rw;
		ex.extract(world, rw, 1.0f);
		REQUIRE(rw.objects.size() == 1);
		CHECK(rw.objects[0].worldBounds.min.x == doctest::Approx(-2.0f));
		CHECK(rw.objects[0].worldBounds.max.x == doctest::Approx( 2.0f));
	}
	// Without a ContentManager the real bounds are unknown, so the object is left with
	// INVALID bounds — the conservative frustum culler then keeps it visible rather than
	// culling a large mesh against a tiny unit-cube proxy (the cause of in-view meshes
	// vanishing while streaming / on LOD swaps).
	{
		RenderExtractor ex;
		RenderWorld rw;
		ex.extract(world, rw, 1.0f);
		REQUIRE(rw.objects.size() == 1);
		CHECK_FALSE(rw.objects[0].worldBounds.isValid());
	}
}

TEST_CASE("FrustumCuller keeps objects with unknown (invalid) bounds visible")
{
	// A mesh whose real bounds aren't resolved yet must never be culled — otherwise a
	// large in-view mesh vanishes while streaming / mid LOD swap. The extractor leaves
	// such objects' worldBounds invalid; the culler must treat them as visible.
	RenderWorld rw;
	rw.camera.view       = glm::lookAt(glm::vec3(0,0,5), glm::vec3(0,0,0), glm::vec3(0,1,0));
	rw.camera.projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);

	RenderObject visibleBox;   // a valid box far off to the side → should be culled
	visibleBox.worldBounds.expand({ 900.0f, 900.0f, 0.0f });
	visibleBox.worldBounds.expand({ 901.0f, 901.0f, 1.0f });
	RenderObject unknown;       // invalid bounds (default) → must be kept visible
	rw.objects = { visibleBox, unknown };

	FrustumCuller culler;
	std::vector<uint8_t> vis;
	culler.cull(rw, vis);
	REQUIRE(vis.size() == 2);
	CHECK(vis[0] == 0u);   // valid box outside the frustum → culled
	CHECK(vis[1] == 1u);   // unknown/invalid bounds → kept visible
}

TEST_CASE("AABB build and ray intersection")
{
	const float verts[] = { -1,-1,-1,  1,1,1,  0,0,0 };
	HE::AABB box = HE::AABB::fromPositions(verts, 3);
	REQUIRE(box.isValid());
	CHECK(box.min == glm::vec3(-1));
	CHECK(box.max == glm::vec3( 1));

	float t = 0;
	// Straight-on hit
	CHECK(box.intersectRay({ 0, 0, -5 }, { 0, 0, 1 }, t));
	CHECK(t == doctest::Approx(4.0f));
	// Pointing away
	CHECK_FALSE(box.intersectRay({ 0, 0, -5 }, { 0, 0, -1 }, t));
	// Parallel miss
	CHECK_FALSE(box.intersectRay({ 5, 0, -5 }, { 0, 0, 1 }, t));
	// Origin inside
	CHECK(box.intersectRay({ 0, 0, 0 }, { 1, 0, 0 }, t));
	CHECK(t == doctest::Approx(0.0f));
}

TEST_CASE("AABB transform")
{
	HE::AABB box;
	box.expand({ -1, -1, -1 });
	box.expand({  1,  1,  1 });

	const glm::mat4 m = glm::translate(glm::mat4(1.0f), { 10, 0, 0 })
	                  * glm::scale(glm::mat4(1.0f), { 2, 2, 2 });
	HE::AABB moved = box.transformed(m);
	CHECK(moved.min.x == doctest::Approx(8.0f));
	CHECK(moved.max.x == doctest::Approx(12.0f));
	CHECK(moved.min.y == doctest::Approx(-2.0f));
}

TEST_CASE("Frustum culls boxes outside the view")
{
	// Camera at origin looking down -Z
	const glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 0),
	                                   glm::vec3(0, 0, -1),
	                                   glm::vec3(0, 1, 0));
	const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
	const Frustum frustum = Frustum::fromViewProj(proj * view);

	auto boxAt = [](glm::vec3 center)
	{
		HE::AABB b;
		b.expand(center - glm::vec3(0.5f));
		b.expand(center + glm::vec3(0.5f));
		return b;
	};

	CHECK(frustum.intersects(boxAt({ 0, 0, -10 })));      // straight ahead
	CHECK_FALSE(frustum.intersects(boxAt({ 0, 0, 10 })));  // behind camera
	CHECK_FALSE(frustum.intersects(boxAt({ 50, 0, -10 }))); // far off to the right
	CHECK_FALSE(frustum.intersects(boxAt({ 0, 0, -200 }))); // beyond far plane
	CHECK(frustum.intersects(boxAt({ 0, 0, -99.9f })));    // straddling far plane
}

TEST_CASE("FrustumCuller marks objects via world bounds")
{
	RenderWorld world;
	world.camera.view = glm::lookAt(glm::vec3(0, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
	world.camera.projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);

	auto makeObj = [](glm::vec3 pos)
	{
		RenderObject o;
		o.transform = glm::translate(glm::mat4(1.0f), pos);
		HE::AABB b;
		b.expand(pos - glm::vec3(0.5f));
		b.expand(pos + glm::vec3(0.5f));
		o.worldBounds = b;
		return o;
	};
	world.objects.push_back(makeObj({ 0, 0, -5 }));  // visible
	world.objects.push_back(makeObj({ 0, 0, 50 }));  // behind camera
	world.objects.push_back(RenderObject{});         // invalid bounds → never culled

	FrustumCuller culler;
	std::vector<uint8_t> visible;
	culler.cull(world, visible);
	REQUIRE(visible.size() == 3);
	CHECK(visible[0]);
	CHECK_FALSE(visible[1]);
	CHECK(visible[2]);
}

TEST_CASE("FrustumCuller parallel cull matches sequential for large object counts")
{
	// Build a camera frustum looking down -Z
	RenderWorld world;
	world.camera.view = glm::lookAt(glm::vec3(0, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
	world.camera.projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);

	// 256 objects: alternating visible (in front) and invisible (behind camera)
	for (int i = 0; i < 256; ++i)
	{
		RenderObject o;
		const float z = (i % 2 == 0) ? -float(i + 1) : float(i + 1); // negative = visible
		glm::vec3 pos(0, 0, z);
		HE::AABB b;
		b.expand(pos - glm::vec3(0.4f));
		b.expand(pos + glm::vec3(0.4f));
		o.worldBounds = b;
		world.objects.push_back(o);
	}

	FrustumCuller culler;
	std::vector<uint8_t> visible;
	culler.cull(world, visible);

	REQUIRE(visible.size() == 256);
	for (int i = 0; i < 256; ++i)
	{
		const float z = (i % 2 == 0) ? -float(i + 1) : float(i + 1);
		const bool expectVisible = (z < 0.0f && z > -100.0f);
		if (expectVisible)
			CHECK(visible[i] != 0);
		else
			CHECK(visible[i] == 0);
	}
}

TEST_CASE("RenderSorter groups by mesh and sorts front-to-back")
{
	RenderWorld world;
	world.camera.position = { 0, 0, 0 };

	HE::UUID meshA; meshA.hi = 1; meshA.lo = 1;
	HE::UUID meshB; meshB.hi = 2; meshB.lo = 2;

	auto makeObj = [](HE::UUID mesh, float z)
	{
		RenderObject o;
		o.meshAssetId = mesh;
		o.transform   = glm::translate(glm::mat4(1.0f), { 0, 0, z });
		return o;
	};
	world.objects.push_back(makeObj(meshB, -10)); // 0
	world.objects.push_back(makeObj(meshA, -20)); // 1
	world.objects.push_back(makeObj(meshA,  -5)); // 2
	world.objects.push_back(makeObj(meshB,  -1)); // 3

	std::vector<uint8_t> visible(4, 1u);
	visible[0] = 0u; // culled — must not appear

	RenderSorter sorter;
	std::vector<uint32_t> order;
	sorter.sort(world, visible, order);

	REQUIRE(order.size() == 3);
	// meshA group first (lower id), front-to-back inside the group
	CHECK(order[0] == 2);
	CHECK(order[1] == 1);
	CHECK(order[2] == 3);
}

TEST_CASE("Repro: a placed mesh dead-center in view must not be culled")
{
	ContentManager cm;
	StaticMeshAsset mesh; mesh.type = HE::AssetType::StaticMesh; mesh.name = "box";
	mesh.indices = {0,1,2};
	mesh.boundsMin[0]=mesh.boundsMin[1]=mesh.boundsMin[2]=-1.0f;
	mesh.boundsMax[0]=mesh.boundsMax[1]=mesh.boundsMax[2]= 1.0f;
	const HE::UUID meshId = cm.registerStaticMesh(mesh);

	HorizonWorld world;
	auto e = world.createEntity("obj");
	TransformComponent t; t.position = glm::vec3(10.0f, 0.0f, 0.0f);
	world.registry().emplace<TransformComponent>(e, t);
	MeshComponent mc; mc.meshAssetId = meshId;
	world.registry().emplace<MeshComponent>(e, mc);

	EditorCameraOverride cam;
	cam.active = true;
	cam.position = glm::vec3(10.0f, 0.0f, 20.0f);
	cam.view = glm::lookAt(cam.position, glm::vec3(10.0f,0.0f,0.0f), glm::vec3(0,1,0));

	RenderExtractor ex; ex.setContentManager(&cm);
	RenderWorld rw;
	ex.extract(world, rw, 16.0f/9.0f, &cam);
	REQUIRE(rw.objects.size() == 1);
	INFO("worldBounds min=(", rw.objects[0].worldBounds.min.x, ",", rw.objects[0].worldBounds.min.y, ",", rw.objects[0].worldBounds.min.z, ") max=(", rw.objects[0].worldBounds.max.x, ",", rw.objects[0].worldBounds.max.y, ",", rw.objects[0].worldBounds.max.z, ")");
	INFO("transform col3=(", rw.objects[0].transform[3][0], ",", rw.objects[0].transform[3][1], ",", rw.objects[0].transform[3][2], ")");

	FrustumCuller culler;
	std::vector<uint8_t> vis;
	culler.cull(rw, vis);
	REQUIRE(vis.size() == 1);
	CHECK(vis[0] == 1u);   // dead-center placed mesh must be visible
}

// ─── Shared cross-backend renderer helpers (audit 1a) ─────────────────────────
// These pure functions used to exist as one private copy per backend (OpenGL,
// Metal, Vulkan, D3D11, D3D12). The copies were kept in sync by hand, guarded
// only by comments. These tests are what replaces those comments: they pin the
// exact numbers the five backends have to agree on.

TEST_CASE("SsaoKernel: deterministic and inside the +Z unit hemisphere")
{
	const std::vector<glm::vec3> a = HE::BuildSSAOKernel(HE::kSsaoKernelSize);
	const std::vector<glm::vec3> b = HE::BuildSSAOKernel(HE::kSsaoKernelSize);
	REQUIRE(a.size() == static_cast<size_t>(HE::kSsaoKernelSize));

	// Same seed → same samples on every call and in every backend. If this ever
	// drifts, GL/Metal/Vulkan/D3D stop producing the same ambient occlusion.
	for (size_t i = 0; i < a.size(); ++i)
	{
		CHECK(a[i].x == b[i].x);
		CHECK(a[i].y == b[i].y);
		CHECK(a[i].z == b[i].z);
	}

	for (size_t i = 0; i < a.size(); ++i)
	{
		INFO("sample ", i);
		CHECK(a[i].z >= 0.0f);                    // hemisphere oriented to +Z (tangent space)
		CHECK(glm::length(a[i]) <= 1.0f);         // inside the unit sphere
		// Packed toward the origin: |sample| <= 0.1 + 0.9*t^2 with t = i/n.
		const float t   = static_cast<float>(i) / static_cast<float>(a.size());
		const float cap = 0.1f + 0.9f * t * t;
		CHECK(glm::length(a[i]) <= cap + 1e-5f);
	}
	// The very first sample is squeezed into the innermost 10% of the hemisphere.
	CHECK(glm::length(a[0]) <= 0.1f);

	// Exact value pin: the kernel is uploaded verbatim to five different GPUs.
	CHECK(a[0].x == doctest::Approx(-0.0183749106f));
	CHECK(a[0].y == doctest::Approx( 0.0315782912f));
	CHECK(a[0].z == doctest::Approx( 0.0095498776f));
}

TEST_CASE("SsaoKernel: rotation noise lies in the tangent plane; RGBA variant matches")
{
	const std::vector<glm::vec3> n    = HE::BuildSSAONoise(HE::kSsaoNoiseCount);
	const std::vector<glm::vec4> nRgba = HE::BuildSSAONoiseRGBA(HE::kSsaoNoiseCount);
	REQUIRE(n.size() == 16);
	REQUIRE(nRgba.size() == 16);

	for (size_t i = 0; i < n.size(); ++i)
	{
		INFO("noise ", i);
		CHECK(n[i].z == 0.0f);            // rotation happens IN the tangent plane
		CHECK(n[i].x >= -1.0f); CHECK(n[i].x < 1.0f);
		CHECK(n[i].y >= -1.0f); CHECK(n[i].y < 1.0f);
		// Vulkan uploads the padded form; it must be the same numbers.
		CHECK(nRgba[i].x == n[i].x);
		CHECK(nRgba[i].y == n[i].y);
		CHECK(nRgba[i].z == n[i].z);
		CHECK(nRgba[i].w == 0.0f);
	}
	CHECK(n[0].x == doctest::Approx( 0.7702374458f));
	CHECK(n[0].y == doctest::Approx( 0.1023778915f));
}

// FNV-1a over the raw bytes — a change of a single texel changes the digest.
static uint64_t heFnv1a(const void* data, size_t bytes)
{
	const unsigned char* p = static_cast<const unsigned char*>(data);
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
	return h;
}

TEST_CASE("SkyNoise3D: generated volume is byte-pinned")
{
	// HISTORY: the "pixelige Wolken" bug was fixed by replacing a 2D FBM with this
	// 3D noise volume, and the fix was validated by proving every backend baked
	// BYTE-IDENTICAL data. These digests are that proof, frozen. A failure here
	// means the sky changed on all five backends at once — re-pin only if that is
	// what you intended.
	const std::vector<uint16_t> v8 = HE::BuildSkyNoise3D(8);
	REQUIRE(v8.size() == 8u * 8u * 8u * 2u);          // interleaved RG16
	CHECK(heFnv1a(v8.data(), v8.size() * sizeof(uint16_t)) == 0x4018d777b550a138ull);

	const std::vector<uint16_t> v16 = HE::BuildSkyNoise3D(16);
	REQUIRE(v16.size() == 16u * 16u * 16u * 2u);
	CHECK(heFnv1a(v16.data(), v16.size() * sizeof(uint16_t)) == 0x00c5555fdb46da21ull);

	// R channel of voxel (0,0,0) is starHash(0,0,0) = 0 — the value-noise lattice
	// must line up with the shader's starHash or the stars/nebula shift.
	CHECK(v16[0] == 0u);

	// Deterministic across calls (no static state, no RNG carry-over).
	CHECK(HE::BuildSkyNoise3D(8) == v8);
}

TEST_CASE("SkyEnvBake: the IBL ambient cube face is pinned and backend-independent")
{
	// This bake IS the ambient light every lit surface receives, and OpenGL and
	// Metal must produce the same bytes from it (it used to be 88 duplicated lines
	// in each). A failure here means the ambient lighting changed on both backends
	// at once — re-pin only if that is what you intended.
	const glm::vec3 sun = glm::normalize(glm::vec3(0.3f, 0.6f, 0.2f));

	// The face-direction convention is the cubemap contract (slice order
	// +X,-X,+Y,-Y,+Z,-Z, shared by GL and Metal — no per-backend axis flip).
	CHECK(HE::SkyEnvFaceDirection(0, 0.0f, 0.0f) == glm::vec3( 1.0f, 0.0f, 0.0f));
	CHECK(HE::SkyEnvFaceDirection(1, 0.0f, 0.0f) == glm::vec3(-1.0f, 0.0f, 0.0f));
	CHECK(HE::SkyEnvFaceDirection(2, 0.0f, 0.0f) == glm::vec3( 0.0f, 1.0f, 0.0f));
	CHECK(HE::SkyEnvFaceDirection(3, 0.0f, 0.0f) == glm::vec3( 0.0f,-1.0f, 0.0f));
	CHECK(HE::SkyEnvFaceDirection(4, 0.0f, 0.0f) == glm::vec3( 0.0f, 0.0f, 1.0f));
	CHECK(HE::SkyEnvFaceDirection(5, 0.0f, 0.0f) == glm::vec3( 0.0f, 0.0f,-1.0f));

	// Digest over all six faces at 4². Quantised to 12 bits first, deliberately:
	// the bake is float and a raw-bits pin would be hostage to the compiler's
	// contraction choices, whereas the visible result is an 8-bit-ish colour.
	// Verified stable at -O0/-O2/-g and bit-identical to the pre-unification code.
	std::vector<uint16_t> q;
	for (int f = 0; f < 6; ++f)
	{
		const std::vector<float> face = HE::BuildSkyEnvFace(4, f, sun);
		REQUIRE(face.size() == 4u * 4u * 4u);          // RGBA32F, faceN² texels
		for (size_t i = 0; i < face.size(); ++i)
		{
			if (i % 4 == 3) CHECK(face[i] == 1.0f);    // alpha is always opaque
			const float c = glm::clamp(face[i], 0.0f, 1.0f);
			q.push_back(static_cast<uint16_t>(c * 4095.0f + 0.5f));
		}
	}
	// Re-pinned with the twilight-sky fix (atmoRaySphere's miss sentinel + the
	// twilight wedge + the horizon-derived ground haze). This sun is HIGH, so the
	// twilight terms are all zero here and the change is confined to the eight
	// just-below-horizon texels the widened ground band now reaches: 0.0007 on a
	// 0.24 value, a fifth of one 8-bit step. Daylight ambient is unchanged; what
	// the fix moves is dusk and dawn, which this sun does not sample.
	CHECK(heFnv1a(q.data(), q.size() * sizeof(uint16_t)) == 0x6991c028c3256941ull);

	// Row granularity must reproduce the whole-face path EXACTLY: OpenGL fans the
	// six faces out row-per-task over the thread pool while Metal loops the rows
	// serially, and the two have to bake identical cubes.
	for (int f = 0; f < 6; ++f)
	{
		const std::vector<float> whole = HE::BuildSkyEnvFace(8, f, sun);
		std::vector<float>       rows(8u * 8u * 4u);
		for (int t = 0; t < 8; ++t)
			HE::BuildSkyEnvFaceRow(8, f, t, sun, &rows[static_cast<size_t>(t) * 8 * 4]);
		CHECK(whole == rows);
	}

	// Sanity on the model itself: straight down is the flat ground colour, and the
	// bake keeps a sun disk (that is what carries the sun's energy into the
	// ambient — the shader's own skyColor drops it).
	const glm::vec3 down = HE::SkyColorCPU(glm::vec3(0.0f, -1.0f, 0.0f), sun);
	CHECK(down.r == doctest::Approx(0.24f));
	CHECK(down.g == doctest::Approx(0.23f));
	CHECK(down.b == doctest::Approx(0.21f));
	// The sun disk is pinned by VALUE, not by a ">" threshold. The 4x4 face digest
	// above cannot see it at all — the disk is pow(dot,1800)*14, far too tight to
	// land on any texel of a 4x4 face — so halving the amplitude or the exponent
	// slipped through a bare `atSun.r > 10.0f`. These two samples close that:
	//   - straight at the sun pins the AMPLITUDE (the 14.0 term),
	//   - 1.6 degrees off pins the EXPONENT: pow(cos(1.6deg), 1800) is near half,
	//     so the disk's falloff width is what this number measures.
	// Tolerance is 1%, not a tight epsilon: these are ~30-iteration float sums, so
	// FP contraction moves them ~0.2% between -O0 and -O2 and between compilers.
	// 1% still catches what matters — halving the amplitude shifts atSun by ~12%,
	// and 1800->1200 shifts the off-axis sample by ~18%.
	const glm::vec3 atSun = HE::SkyColorCPU(sun, sun);
	CHECK(atSun.r == doctest::Approx(17.118f).epsilon(0.01));
	CHECK(atSun.g == doctest::Approx(16.583f).epsilon(0.01));
	CHECK(atSun.b == doctest::Approx(15.523f).epsilon(0.01));

	const glm::vec3 sunAxis = glm::normalize(glm::cross(sun, glm::vec3(0.0f, 1.0f, 0.0f)));
	const float     theta   = 1.6f * 3.14159265f / 180.0f;
	const glm::vec3 offSun  = glm::normalize(sun * std::cos(theta) + sunAxis * std::sin(theta));
	const glm::vec3 atOff   = HE::SkyColorCPU(offSun, sun);
	CHECK(atOff.r == doctest::Approx(9.899f).epsilon(0.01));
	// Deterministic across calls (pure functions, no cached state).
	CHECK(HE::BuildSkyEnvFace(4, 0, sun) == HE::BuildSkyEnvFace(4, 0, sun));
}

// The sky used to fall off a cliff at sunset: atmoRaySphere returned a POSITIVE
// near distance for "the sun ray misses the planet", which the sun-visibility test
// read as "shadowed" — so every sample whose sun ray cleared the planet entirely
// was skipped, and the scattering integral hit exactly zero at sunY = 0. On screen
// that was a black sky under clouds the sun was still lighting, with a hard bright
// band along the horizon. This pins the shape of the ramp, not its exact values.
TEST_CASE("SkyEnvBake: the sky darkens smoothly through sunset, never off a cliff")
{
	// Sun sinking along +X, sampled from well above the horizon into deep night.
	const float elev[] = { 0.20f, 0.10f, 0.05f, 0.02f, 0.0f, -0.02f, -0.05f,
	                       -0.09f, -0.14f, -0.20f, -0.28f };
	auto sunAt = [](float y) {
		return glm::normalize(glm::vec3(std::sqrt(std::max(1e-6f, 1.0f - y * y)), y, 0.0f));
	};
	auto lum = [](const glm::vec3& c) { return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b; };

	const glm::vec3 zenith(0.0f, 1.0f, 0.0f);
	const glm::vec3 antiSun = glm::normalize(glm::vec3(-1.0f, 0.03f, 0.0f)); // horizon, away from the sun

	float prevZ = lum(HE::SkyColorCPU(zenith,  sunAt(elev[0])));
	float prevA = lum(HE::SkyColorCPU(antiSun, sunAt(elev[0])));
	for (size_t i = 1; i < std::size(elev); ++i)
	{
		const glm::vec3 s = sunAt(elev[i]);
		const float z = lum(HE::SkyColorCPU(zenith,  s));
		const float a = lum(HE::SkyColorCPU(antiSun, s));

		// Never black. The old code gave 0.008 at the zenith while the sun was
		// still ABOVE the horizon, and 0.019 on the anti-sun horizon.
		CHECK(z > 0.015f);
		CHECK(a > 0.015f);
		// No step steeper than 3x — the cliff was 9x at the zenith and 35x on the
		// anti-sun horizon in ONE step.
		CHECK(prevZ < z * 3.0f);
		CHECK(prevA < a * 3.0f);
		// The zenith only ever dims. The anti-solar horizon is allowed a small
		// rise: that band really does brighten for a few minutes after sunset
		// (the Belt of Venus), and the twilight wedge reproduces it. Bounded, so
		// an accidental flat lift of the whole sky still trips this.
		CHECK(z <= prevZ + 1e-4f);
		CHECK(a <= prevA * 1.25f + 1e-4f);
		prevZ = z; prevA = a;
	}

	// Twilight is warm toward the sun and cool away from it — the wedge is a
	// direction-dependent term, not a flat lift of the whole sky.
	const glm::vec3 dusk    = sunAt(-0.08f);
	const glm::vec3 toSun   = HE::SkyColorCPU(glm::normalize(glm::vec3( 1.0f, 0.05f, 0.0f)), dusk);
	const glm::vec3 awaySun = HE::SkyColorCPU(glm::normalize(glm::vec3(-1.0f, 0.05f, 0.0f)), dusk);
	CHECK(toSun.r > toSun.b);        // warm on the sun side
	CHECK(awaySun.b > awaySun.r);    // cool opposite it
	CHECK(toSun.r > awaySun.r * 3.0f);

	// ...and it is gone by the middle of the night, rather than leaving a
	// permanent glow pinned at the sunY clamp.
	const glm::vec3 midnight = HE::SkyColorCPU(glm::normalize(glm::vec3(1.0f, 0.05f, 0.0f)),
	                                           glm::vec3(0.0f, -1.0f, 0.0f));
	CHECK(midnight.r < midnight.b);
}

TEST_CASE("SeedWeatherParticles: both backend lane layouts carry identical particles")
{
	// The two backends' simulation shaders read the 8-float record differently, so
	// the seeder takes the lane order as a parameter. What must NOT differ is the
	// particles themselves: same positions, same seeds, same velocities — only
	// their slots move. Before this was shared, GL and Metal each had their own
	// copy of the RNG and the lane maths, so a change to one silently desynced the
	// look of a downpour between backends.
	IRenderer::GpuParticleParams p{};
	p.cameraPos   = { 10.0f, 5.0f, -3.0f };
	p.boxTop      = 20.0f;
	p.boxHalf     = 15.0f;
	p.groundLevel = 0.0f;
	p.fallSpeed   = 9.0f;
	p.windVec     = { 2.0f, 0.0f, -1.0f };
	p.isSnow      = false;

	constexpr int kN = 4;
	std::vector<float> gl(kN * HE::kWeatherParticleFloats, 0.0f);
	HE::SeedWeatherParticles(p, kN, HE::WeatherParticleLayout::PosVelLifeSeed, gl.data());
	std::vector<float> mt(kN * HE::kWeatherParticleFloats, 0.0f);
	HE::SeedWeatherParticles(p, kN, HE::WeatherParticleLayout::PosLifeVelSeed, mt.data());

	for (int i = 0; i < kN; ++i)
	{
		const float* g = &gl[static_cast<size_t>(i) * HE::kWeatherParticleFloats];
		const float* m = &mt[static_cast<size_t>(i) * HE::kWeatherParticleFloats];
		// Lanes 0..2 (position) and lane 7 (seed) are layout-invariant.
		for (int k = 0; k < 3; ++k) CHECK(g[k] == m[k]);
		CHECK(g[7] == m[7]);
		// The permutation itself: GL [vx,vy,vz,life], Metal [life,vx,vy,vz].
		CHECK(g[3] == m[4]);  // vx
		CHECK(g[4] == m[5]);  // vy
		CHECK(g[5] == m[6]);  // vz
		CHECK(g[6] == m[3]);  // life
		// Life expires exactly as the drop reaches the ground plane.
		CHECK(g[6] == doctest::Approx((g[1] - p.groundLevel) / p.fallSpeed));
		// The pool is spread evenly so `step(seed, coverage)` keeps a coverage
		// fraction alive — an uneven spread would make coverage non-linear.
		CHECK(g[7] == doctest::Approx((i + 0.5f) / static_cast<float>(kN)));
	}

	// Pin the RNG itself: these are the first record's values. The generator is a
	// fixed golden-ratio start value plus an LCG precisely so a downpour looks the
	// same on every backend and every run — reseeding it is a visual change.
	CHECK(gl[0] == doctest::Approx(22.322083f).epsilon(0.0001));
	CHECK(gl[1] == doctest::Approx(6.524979f).epsilon(0.0001));
	CHECK(gl[2] == doctest::Approx(-10.547133f).epsilon(0.0001));

	// Snow is blown around far less than rain by the same wind (0.3 vs 1.2), and
	// that is the only thing isSnow changes here — positions must be untouched.
	std::vector<float> snow(kN * HE::kWeatherParticleFloats, 0.0f);
	p.isSnow = true;
	HE::SeedWeatherParticles(p, kN, HE::WeatherParticleLayout::PosVelLifeSeed, snow.data());
	CHECK(snow[0] == doctest::Approx(gl[0]));
	CHECK(snow[1] == doctest::Approx(gl[1]));
	CHECK(snow[3] == doctest::Approx(p.windVec.x * 0.3f));
	CHECK(gl[3]   == doctest::Approx(p.windVec.x * 1.2f));
	CHECK(snow[4] == doctest::Approx(-p.fallSpeed)); // gravity is unchanged
}

TEST_CASE("CloudWindVector: 0 degrees drifts toward -Z (north), clockwise from there")
{
	IRenderer::EnvironmentSettings env;
	env.windSpeed = 1.0f;

	// windDirection is the compass direction the clouds drift TOWARD.
	env.windDirection = 0.0f;                       // north = -Z
	glm::vec3 w = HE::CloudWindVector(env);
	CHECK(w.x == doctest::Approx(0.0f).epsilon(1e-6));
	CHECK(w.y == 0.0f);
	CHECK(w.z == doctest::Approx(-0.025f));

	env.windDirection = 90.0f;                      // east = +X
	w = HE::CloudWindVector(env);
	CHECK(w.x == doctest::Approx(0.025f));
	CHECK(w.z == doctest::Approx(0.0f).epsilon(1e-6));

	env.windDirection = 180.0f;                     // south = +Z
	w = HE::CloudWindVector(env);
	CHECK(w.z == doctest::Approx(0.025f));

	env.windDirection = 270.0f;                     // west = -X
	w = HE::CloudWindVector(env);
	CHECK(w.x == doctest::Approx(-0.025f));

	// Speed scales linearly; the horizontal plane is never left.
	env.windDirection = 30.0f;
	env.windSpeed     = 4.0f;
	w = HE::CloudWindVector(env);
	CHECK(w.y == 0.0f);
	CHECK(glm::length(w) == doctest::Approx(0.1f));

	env.windSpeed = 0.0f;
	CHECK(glm::length(HE::CloudWindVector(env)) == doctest::Approx(0.0f));
}

TEST_CASE("RenderWorld::dominantDirectionalLight picks the brightest directional light")
{
	RenderWorld rw;

	// No lights at all → false, direction falls back to the sky-dome sun, colour
	// is HARD ZERO (never the environment sun, see the header comment).
	{
		glm::vec3 toward(0.0f), color(1.0f);
		CHECK(rw.dominantDirectionalLight(toward, color) == false);
		CHECK(color == glm::vec3(0.0f));
		CHECK(glm::length(toward) == doctest::Approx(1.0f));
	}

	// Point/spot lights are NOT candidates however bright.
	{
		LightData p; p.type = 1; p.intensity = 100.0f; p.direction = glm::vec3(0,-1,0);
		LightData s; s.type = 2; s.intensity = 100.0f; s.direction = glm::vec3(0,-1,0);
		rw.lights = { p, s };
		glm::vec3 toward(0.0f), color(1.0f);
		CHECK(rw.dominantDirectionalLight(toward, color) == false);
		CHECK(color == glm::vec3(0.0f));
	}

	// Brightest directional wins; `toward` is the NEGATED travel direction.
	{
		LightData dim;    dim.type = 0;    dim.intensity = 0.5f; dim.direction = glm::vec3(1,0,0);
		dim.color = glm::vec3(1,0,0);
		LightData bright; bright.type = 0; bright.intensity = 3.0f; bright.direction = glm::vec3(0,-1,0);
		bright.color = glm::vec3(0,1,0);
		LightData point;  point.type = 1;  point.intensity = 99.0f; point.direction = glm::vec3(0,0,-1);
		rw.lights = { dim, point, bright };

		glm::vec3 toward(0.0f), color(0.0f);
		CHECK(rw.dominantDirectionalLight(toward, color) == true);
		CHECK(toward.x == doctest::Approx(0.0f));
		CHECK(toward.y == doctest::Approx(1.0f));   // light travels -Y → toward is +Y
		CHECK(toward.z == doctest::Approx(0.0f));
		CHECK(color.g == doctest::Approx(3.0f));    // color * intensity
		CHECK(color.r == doctest::Approx(0.0f));
	}

	// Zero-intensity directionals are skipped entirely.
	{
		LightData off; off.type = 0; off.intensity = 0.0f; off.direction = glm::vec3(0,-1,0);
		rw.lights = { off };
		glm::vec3 toward(0.0f), color(1.0f);
		CHECK(rw.dominantDirectionalLight(toward, color) == false);
		CHECK(color == glm::vec3(0.0f));
	}

	// TIE-BREAK: the comparison is a strict `>`, so on equal intensity the FIRST
	// light in extractor order wins. Backends relied on this implicitly.
	{
		LightData first;  first.type = 0;  first.intensity = 2.0f;
		first.direction = glm::vec3(0,-1,0); first.color = glm::vec3(1,0,0);
		LightData second; second.type = 0; second.intensity = 2.0f;
		second.direction = glm::vec3(0,-1,0); second.color = glm::vec3(0,0,1);
		rw.lights = { first, second };
		glm::vec3 toward(0.0f), color(0.0f);
		CHECK(rw.dominantDirectionalLight(toward, color) == true);
		CHECK(color.r == doctest::Approx(2.0f));   // first won
		CHECK(color.b == doctest::Approx(0.0f));
	}

	// A degenerate (zero-length) direction is treated as "nothing shines" — the
	// normalize would otherwise produce NaNs on five GPUs.
	{
		LightData bad; bad.type = 0; bad.intensity = 5.0f; bad.direction = glm::vec3(0.0f);
		rw.lights = { bad };
		glm::vec3 toward(0.0f), color(1.0f);
		CHECK(rw.dominantDirectionalLight(toward, color) == false);
		CHECK(color == glm::vec3(0.0f));
	}
}

TEST_CASE("BuildPackedLightArray: order, filtering and encoding are the shader contract")
{
	RenderWorld rw;
	LightData dir;   dir.type = 0;   dir.intensity = 5.0f;
	LightData pt;    pt.type  = 1;   pt.intensity  = 2.0f;
	pt.position = glm::vec3(1,2,3); pt.range = 7.0f; pt.color = glm::vec3(1,0.5f,0.25f);
	pt.direction = glm::vec3(0,-1,0); pt.spotAngleCos = 0.0f;
	LightData off;   off.type = 1;   off.intensity = 0.0f;   // switched off → skipped
	LightData spot;  spot.type = 2;  spot.intensity = 3.0f;
	spot.position = glm::vec3(-1,0,0); spot.range = 0.0f;     // range 0 → clamped
	spot.spotAngleCos = 0.7f; spot.direction = glm::vec3(0,0,-1);

	rw.lights = { dir, pt, off, spot };
	const HE::PackedLightArray a = HE::BuildPackedLightArray(rw);

	// Directional and zero-intensity lights are dropped; the rest keep extractor order.
	REQUIRE(a.count == 2);
	CHECK(a.posRange[0] == glm::vec4(1,2,3,7));
	CHECK(a.colorType[0].x == doctest::Approx(2.0f));       // color * intensity
	CHECK(a.colorType[0].y == doctest::Approx(1.0f));
	CHECK(a.colorType[0].w == doctest::Approx(1.0f));       // LightType point
	CHECK(a.dirCos[0].w    == doctest::Approx(0.0f));

	CHECK(a.posRange[1].x == doctest::Approx(-1.0f));
	CHECK(a.posRange[1].w == doctest::Approx(1e-4f));       // range floored, never 0
	CHECK(a.colorType[1].w == doctest::Approx(2.0f));       // LightType spot
	CHECK(a.dirCos[1].w    == doctest::Approx(0.7f));

	// Hard clamp at 8 accepted lights — the uniform arrays are exactly that long.
	rw.lights.clear();
	for (int i = 0; i < 20; ++i)
	{
		LightData l; l.type = 1; l.intensity = 1.0f;
		l.position = glm::vec3(static_cast<float>(i), 0.0f, 0.0f);
		rw.lights.push_back(l);
	}
	const HE::PackedLightArray full = HE::BuildPackedLightArray(rw);
	CHECK(full.count == HE::kMaxLightWindow);
	CHECK(full.posRange[0].x == doctest::Approx(0.0f));     // first eight, in order
	CHECK(full.posRange[7].x == doctest::Approx(7.0f));
}

TEST_CASE("BuildMaskedLocalLights: counts within the 8-light window, fills the first 4")
{
	RenderWorld rw;
	auto local = [](float x) { LightData l; l.type = 1; l.intensity = 1.0f;
	                           l.position = glm::vec3(x,0,0); l.range = 5.0f; return l; };

	// A directional light INSIDE the window consumes a window slot but no mask
	// channel — the fragment shader's channel index counts type != 0 only.
	LightData dir; dir.type = 0; dir.intensity = 1.0f;
	rw.lights = { dir, local(1), local(2), local(3), local(4), local(5) };
	const HE::PackedLocalShadowLights m = HE::BuildMaskedLocalLights(rw);
	CHECK(m.count == HE::kMaxMaskedLocalLights);            // clamped to 4
	CHECK(m.posRange[0].x == doctest::Approx(1.0f));
	CHECK(m.posRange[3].x == doctest::Approx(4.0f));
	CHECK(m.posRange[3].w == doctest::Approx(5.0f));

	// Lights BEYOND the 8-light window are invisible here even if the first eight
	// slots hold no local light at all.
	rw.lights.clear();
	for (int i = 0; i < 8; ++i) { LightData d; d.type = 0; d.intensity = 1.0f; rw.lights.push_back(d); }
	rw.lights.push_back(local(99.0f));
	const HE::PackedLocalShadowLights none = HE::BuildMaskedLocalLights(rw);
	CHECK(none.count == 0);

	// Unlike BuildPackedLightArray this scan does NOT filter on intensity: the
	// shader's channel counter doesn't either, so dropping dark lights here would
	// shift every later light's mask channel.
	rw.lights.clear();
	LightData dark = local(1.0f); dark.intensity = 0.0f;
	rw.lights = { dark, local(2.0f) };
	const HE::PackedLocalShadowLights dk = HE::BuildMaskedLocalLights(rw);
	CHECK(dk.count == 2);
	CHECK(dk.posRange[0].x == doctest::Approx(1.0f));
}

TEST_CASE("RenderSorter: transparency partition uses the tinted opacity")
{
	std::vector<DrawCall> calls(4);
	calls[0].opacity = 1.0f;                                            // opaque
	calls[1].opacity = 0.5f;                                            // translucent material
	calls[2].opacity = 1.0f; calls[2].instanceTint.a = 0.25f;           // tint-driven fade
	calls[3].opacity = 0.9995f;                                         // within the epsilon → opaque

	CHECK(RenderSorter::isTransparent(calls[0]) == false);
	CHECK(RenderSorter::isTransparent(calls[1]) == true);
	CHECK(RenderSorter::isTransparent(calls[2]) == true);
	CHECK(RenderSorter::isTransparent(calls[3]) == false);
	CHECK(RenderSorter::effectiveOpacity(calls[2]) == doctest::Approx(0.25f));

	std::vector<const DrawCall*> opaque, transparent;
	RenderSorter::partitionByOpacity(calls, opaque, transparent);
	REQUIRE(opaque.size() == 2);
	REQUIRE(transparent.size() == 2);
	// Record order is preserved inside each bucket (the opaque pass batches runs).
	CHECK(opaque[0] == &calls[0]);
	CHECK(opaque[1] == &calls[3]);
	CHECK(transparent[0] == &calls[1]);
	CHECK(transparent[1] == &calls[2]);
}

TEST_CASE("RenderSorter: blended pass is ordered farthest-first")
{
	const glm::vec3 camPos(0.0f, 0.0f, 0.0f);
	std::vector<DrawCall> calls(3);
	calls[0].transform[3] = glm::vec4(5.0f, 0.0f, 0.0f, 1.0f);
	calls[1].transform[3] = glm::vec4(50.0f, 0.0f, 0.0f, 1.0f);
	calls[2].transform[3] = glm::vec4(20.0f, 0.0f, 0.0f, 1.0f);

	std::vector<const DrawCall*> t = { &calls[0], &calls[1], &calls[2] };
	RenderSorter::sortBackToFront(t, camPos);
	CHECK(t[0] == &calls[1]);   // 50
	CHECK(t[1] == &calls[2]);   // 20
	CHECK(t[2] == &calls[0]);   //  5

	// Squared distance is only a sort key — same order as the true distance.
	CHECK(RenderSorter::backToFrontKey(calls[1].transform, camPos) == doctest::Approx(2500.0f));
}

TEST_CASE("SkyFrameParams: one EnvironmentSettings translation for every backend")
{
	// The constant buffer is memcpy'd straight to the GPU — the size is a contract
	// with the MSL/GLSL/HLSL struct.
	CHECK(sizeof(HE::SkyFrameParams) == 64 + 17 * 16);

	IRenderer::EnvironmentSettings env;
	env.windDirection    = 90.0f;
	env.windSpeed        = 2.0f;
	env.cloudCoverage    = 0.75f;
	env.timeOfDay        = 0.25f;
	env.moonPhase        = 0.3f;
	env.flash            = 0.5f;
	env.rainAmount       = 0.4f;
	env.cloudQuality     = 2;
	env.starTwinkle      = 0.8f;
	env.nebulaCoverage   = 0.6f;
	env.cloudMode        = 1;
	env.nebulaQuality    = 2;
	env.godRays          = 0.9f;
	env.shootingStars    = 0.1f;
	env.cloudStyle       = 0;
	env.cloudInterShadows = false;
	env.cloudEvolution   = 1.5f;

	HE::SkyFrameInputs in;
	in.sunDir         = glm::vec3(0.0f, 1.0f, 0.0f);
	in.cameraPos      = glm::vec3(1.0f, 2.0f, 3.0f);
	in.time           = 12.5f;
	in.hasMoonTexture = true;
	in.lowResClouds   = true;

	const HE::SkyFrameParams p = HE::BuildSkyFrameParams(env, in);

	// wind.xyz is exactly CloudWindVector; wind.w is the lightning flash.
	const glm::vec3 w = HE::CloudWindVector(env);
	CHECK(p.wind.x == doctest::Approx(w.x));
	CHECK(p.wind.z == doctest::Approx(w.z));
	CHECK(p.wind.w == doctest::Approx(0.5f));

	CHECK(p.sunDir.w   == doctest::Approx(1.0f));    // has-moon flag
	CHECK(p.sunColor.w == doctest::Approx(0.3f));    // lunar phase
	CHECK(p.params.x   == doctest::Approx(0.25f));   // timeOfDay
	CHECK(p.params.y   == doctest::Approx(0.75f));   // coverage
	CHECK(p.params.z   == doctest::Approx(12.5f));   // wall clock
	CHECK(p.cameraPos.x == doctest::Approx(1.0f));
	CHECK(p.cameraPos.w == doctest::Approx(1.0f));   // cloudMode
	CHECK(p.nebulaColor2.w == doctest::Approx(2.0f)); // nebula quality
	CHECK(p.nebulaColor3.w == doctest::Approx(0.9f)); // god rays
	CHECK(p.auroraColorTop.w == doctest::Approx(0.1f)); // meteors
	// neb2 = (nebulaCoverage, cloudStyle, cloudInterShadows, cloudEvolution)
	CHECK(p.neb2.x == doctest::Approx(0.6f));
	CHECK(p.neb2.y == doctest::Approx(0.0f));
	CHECK(p.neb2.z == doctest::Approx(0.0f));
	CHECK(p.neb2.w == doctest::Approx(1.5f));

	// star2 = (twinkle, cloudQuality, composite-low-res-clouds, rainAmount)
	CHECK(p.star2.x == doctest::Approx(0.8f));
	CHECK(p.star2.y == doctest::Approx(2.0f));
	CHECK(p.star2.z == doctest::Approx(1.0f));
	CHECK(p.star2.w == doctest::Approx(0.4f));

	// The cloud PRE-PASS asks for lowResClouds = false-vs-true independently; only
	// star2.z changes, everything else stays byte-identical.
	HE::SkyFrameInputs in2 = in; in2.lowResClouds = false;
	const HE::SkyFrameParams q = HE::BuildSkyFrameParams(env, in2);
	CHECK(q.star2.z == doctest::Approx(0.0f));
	CHECK(q.star2.w == doctest::Approx(p.star2.w));
	CHECK(q.wind    == p.wind);
}

// ─── Cascaded-shadow-map fit ─────────────────────────────────────────────────
// RenderExtractor::fitDirectionalShadow is built from three free functions in
// HorizonRendering/RenderExtractor.h. They are free precisely so the maths can
// be pinned here: reaching the same numbers through extract() would mean
// building a whole ECS world and then reverse-engineering a cascade matrix.

TEST_CASE("fitCascadeSphere: sphere through the slice's near and far corner rings")
{
	// A slice of a narrow frustum (tan(halfFov) = 0.2 on both axes), [1, 2].
	// This is the ordinary case: the centre lands INSIDE the slice and the sphere
	// touches both corner rings, which is what "fit" means here.
	const HE::CascadeSphere s = HE::fitCascadeSphere(1.0f, 2.0f, 0.2f, 0.2f);
	CHECK(s.centerDistance == doctest::Approx(1.62f));

	// Corner-ring distances from that centre — equal, and both inside the radius.
	const float dNear = std::sqrt(0.08f + (1.0f - 1.62f) * (1.0f - 1.62f)); // |(0.2,0.2)|² = 0.08
	const float dFar  = std::sqrt(0.32f + (2.0f - 1.62f) * (2.0f - 1.62f)); // |(0.4,0.4)|² = 0.32
	CHECK(dNear == doctest::Approx(dFar));
	CHECK(s.radius == doctest::Approx(0.6875f));  // ceil(0.681469 * 16) / 16
	CHECK(s.radius >= dNear);

	// 90° fov, square aspect, slice [1, 3]: the unquantised centre would sit at
	// z = 6, BEHIND the far plane, so the fit clamps it to the far plane and the
	// sphere degenerates to the far ring's circumsphere, r = |(3, 3)| = sqrt(18).
	const HE::CascadeSphere wide = HE::fitCascadeSphere(1.0f, 3.0f, 1.0f, 1.0f);
	CHECK(wide.centerDistance == doctest::Approx(3.0f));
	CHECK(wide.radius == doctest::Approx(4.25f));  // ceil(sqrt(18) * 16) / 16
}

TEST_CASE("fitCascadeSphere: the radius is quantised, so small fov drift cannot swim the shadows")
{
	// The radius IS the cascade's texel size. If it wobbled with a hair of aspect
	// drift (a resized viewport, a fov slider), the texel grid would wobble with
	// it and the shadow edges would crawl. Quantising to 1/16 absorbs that.
	const float base = HE::fitCascadeSphere(0.1f, 40.0f, 1.3333f, 1.0f).radius;
	CHECK(base * 16.0f == doctest::Approx(std::round(base * 16.0f)));
	CHECK(HE::fitCascadeSphere(0.1f, 40.0f, 1.3333f + 1e-5f, 1.0f).radius == base);
	CHECK(HE::fitCascadeSphere(0.1f, 40.0f, 1.3333f + 3e-4f, 1.0f).radius == base);

	// A degenerate (near-zero-extent) slice still yields a usable radius — a
	// zero-width ortho box would make the cascade projection singular.
	CHECK(HE::fitCascadeSphere(5.0f, 5.001f, 0.0f, 0.0f).radius >= 0.01f);
}

TEST_CASE("computeCascadeSplits: blends the logarithmic and the uniform split series")
{
	float u[4] = {}, l[4] = {}, s[4] = {};

	// lambda = 0 → the uniform series verbatim.
	HE::computeCascadeSplits(0.1f, 250.0f, 3, 0.0f, u);
	CHECK(u[0] == doctest::Approx(0.1f));
	CHECK(u[1] == doctest::Approx(83.4f));
	CHECK(u[2] == doctest::Approx(166.7f));
	CHECK(u[3] == doctest::Approx(250.0f));

	// lambda = 1 → the logarithmic series, near * (far/near)^(i/n).
	HE::computeCascadeSplits(0.1f, 250.0f, 3, 1.0f, l);
	CHECK(l[0] == doctest::Approx(0.1f));
	CHECK(l[1] == doctest::Approx(0.1f * std::pow(250.0f / 0.1f, 1.0f / 3.0f)));
	CHECK(l[2] == doctest::Approx(0.1f * std::pow(250.0f / 0.1f, 2.0f / 3.0f)));
	CHECK(l[3] == doctest::Approx(250.0f));

	// The extractor's lambda = 0.5 is exactly the mean of the two, strictly
	// increasing, and pinned at both ends (the last split IS the shadow distance).
	HE::computeCascadeSplits(0.1f, 250.0f, 3, 0.5f, s);
	for (int i = 0; i <= 3; ++i) CHECK(s[i] == doctest::Approx(0.5f * (u[i] + l[i])));
	CHECK(s[0] < s[1]);
	CHECK(s[1] < s[2]);
	CHECK(s[2] < s[3]);
	CHECK(s[0] == doctest::Approx(0.1f));
	CHECK(s[3] == doctest::Approx(250.0f));
}

TEST_CASE("cascadeTexelSnapOffset: anchors the shadow texel grid to the world")
{
	constexpr float kRes    = 2048.0f;
	const float     halfRes = kRes * 0.5f;
	const float     crad    = 12.5f;
	// A cascade centre deliberately off the texel grid.
	const glm::vec3 center(3.7f, 1.3f, -8.9f);
	const glm::vec3 dir  = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f)); // light travel dir
	const glm::mat4 view = glm::lookAt(center - dir * 40.0f, center, glm::vec3(0, 1, 0));
	glm::mat4       proj = glm::ortho(-crad, crad, -crad, crad, 0.05f, 80.0f);

	const glm::vec4 before = (proj * view) * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
	CHECK(std::abs(before.x * halfRes - std::round(before.x * halfRes)) > 1e-3f); // really unaligned
	CHECK(std::abs(before.y * halfRes - std::round(before.y * halfRes)) > 1e-3f);

	// The shift is sub-texel — it may never move the cascade by a whole texel.
	const glm::vec2 off = HE::cascadeTexelSnapOffset(proj * view, kRes);
	CHECK(std::abs(off.x) <= 1.0f / halfRes);
	CHECK(std::abs(off.y) <= 1.0f / halfRes);

	// Applied the way the extractor applies it, the (fixed) world origin lands on
	// a whole shadow texel — that is what locks the grid to the world instead of
	// to the camera-following cascade centre.
	proj[3][0] += off.x;
	proj[3][1] += off.y;
	const glm::vec4 after = (proj * view) * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
	CHECK(std::abs(after.x * halfRes - std::round(after.x * halfRes)) < 1e-3f);
	CHECK(std::abs(after.y * halfRes - std::round(after.y * halfRes)) < 1e-3f);

	// Idempotent: an already-snapped projection asks for no further shift.
	const glm::vec2 off2 = HE::cascadeTexelSnapOffset(proj * view, kRes);
	CHECK(std::abs(off2.x) < 1e-5f);
	CHECK(std::abs(off2.y) < 1e-5f);
}

// ─── GI shader-kernel drift guard ────────────────────────────────────────────
// The ray-traced-GI shading maths exists as a family of hand-kept ports: one
// per API dialect (GLSL / HLSL / MSL) and, for the ray kernels, one per
// traversal path (software BVH vs. hardware ray query). They are held in sync
// by discipline alone — nothing forces it, and a divergence shows up only as a
// rendering difference on one backend, which is exactly the kind of bug nobody
// notices for months. This compares the constants and the small shared function
// bodies across the copies that live in their own source FILES; the copies
// embedded as C++ string literals in the backend translation units are named in
// each file's sync-comment block instead (they are being migrated and would
// make this test fight the backend rework).
namespace shaderdrift
{
namespace fs = std::filesystem;

// Locate the checkout (the directory holding src/HE_Rendering/shaders). This
// test reads SOURCE files, so the build tree alone is not enough: seed the walk
// with the directory this TU was compiled from and with the process CWD (the
// build dir lives inside the checkout), then walk up from both.
inline fs::path findRepoRoot()
{
	std::error_code ec;
	std::vector<fs::path> seeds;
	fs::path self(__FILE__);
	if (!self.is_absolute()) self = fs::current_path(ec) / self;
	seeds.push_back(self.parent_path());
	seeds.push_back(fs::current_path(ec));
	for (fs::path seed : seeds)
		for (int up = 0; up < 8 && !seed.empty(); ++up, seed = seed.parent_path())
			if (fs::exists(seed / "src" / "HE_Rendering" / "shaders" / "gi_probe_hw.comp", ec))
				return seed;
	return {};
}

// Read a source file with its line endings NORMALISED to '\n'.
//
// .gitattributes sets `* text=auto`, so a Windows checkout materialises these
// files with CRLF while macOS/Linux get LF. Read verbatim (binary, so the reader
// is not at the mercy of the platform's text-mode translation), then strip the
// '\r' explicitly — the needles below are C++ string literals written with '\n',
// and a multi-line one can never match CRLF content. That is not hypothetical:
// the SSR/Fresnel needle added in 619e098 turned Windows CI red and left the
// other two platforms green, because every OTHER check here matches within a
// single line and so never noticed the difference.
inline std::string readFile(const fs::path& p)
{
	std::ifstream f(p, std::ios::binary);
	std::ostringstream ss;
	ss << f.rdbuf();
	std::string s = ss.str();
	s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
	return s;
}

// Comments differ freely between the copies (each explains its own dialect), and
// several of them quote the very constants below — strip them before matching.
inline std::string stripLineComments(const std::string& src)
{
	std::string out;
	out.reserve(src.size());
	for (size_t i = 0; i < src.size();)
	{
		if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/')
		{
			while (i < src.size() && src[i] != '\n') ++i;
			continue;
		}
		out += src[i++];
	}
	return out;
}

// Reduce a captured snippet to a dialect-neutral, formatting-neutral form:
// collapse whitespace, rewrite the HLSL type/intrinsic spellings to their GLSL
// equivalents, and canonicalise every numeric literal (so 8 == 8.0 and
// 1e-4 == 0.0001). What survives is the part that must actually agree.
inline std::string canonicalise(const std::string& s)
{
	std::string ws;
	bool inSpace = false;
	for (char c : s)
	{
		if (std::isspace(static_cast<unsigned char>(c))) { inSpace = true; continue; }
		if (inSpace && !ws.empty()) ws += ' ';
		inSpace = false;
		ws += c;
	}
	// Order matters: uint2 before int2, float4x4 before float4.
	static const char* kDialect[][2] = {
		{ "float4x4", "mat4" }, { "float3x3", "mat3" },
		{ "uint2", "uvec2" },   { "uint3", "uvec3" },
		{ "float2", "vec2" },   { "float3", "vec3" },  { "float4", "vec4" },
		{ "int2", "ivec2" },    { "int3", "ivec3" },   { "int4", "ivec4" },
		{ "frac(", "fract(" },  { "lerp(", "mix(" },
	};
	for (const auto& r : kDialect)
	{
		const size_t n = std::string(r[0]).size();
		for (size_t p = ws.find(r[0]); p != std::string::npos; p = ws.find(r[0], p + 1))
			ws.replace(p, n, r[1]);
	}
	static const std::regex kNum(R"([0-9]*\.?[0-9]+(?:[eE][+-]?[0-9]+)?)");
	std::string out;
	size_t last = 0;
	for (auto it = std::sregex_iterator(ws.begin(), ws.end(), kNum);
	     it != std::sregex_iterator(); ++it)
	{
		out.append(ws, last, static_cast<size_t>(it->position()) - last);
		char buf[64];
		std::snprintf(buf, sizeof buf, "%.9g", std::atof(it->str().c_str()));
		out += buf;
		last = static_cast<size_t>(it->position() + it->length());
	}
	out.append(ws, last, std::string::npos);
	return out;
}

// Every capture of every match, joined — so a constant that appears N times is
// compared N times, and a copy that lost one occurrence is caught too.
inline std::string extract(const std::string& text, const char* pattern)
{
	const std::regex re(pattern);
	std::string joined;
	for (auto it = std::sregex_iterator(text.begin(), text.end(), re);
	     it != std::sregex_iterator(); ++it)
		for (size_t g = 1; g < it->size(); ++g)
		{
			if (!joined.empty()) joined += " | ";
			joined += (*it)[g].str();
		}
	return canonicalise(joined);
}

struct SharedConstant
{
	const char* name;
	// One pattern shared by every copy, or one per copy where the dialects spell
	// the surrounding code differently (workgroup size, tile-size declaration).
	std::vector<const char*> patterns;
};

inline void checkGroup(const std::vector<const char*>& names,
                       const std::vector<std::string>& sources,
                       const std::vector<SharedConstant>& constants)
{
	for (const SharedConstant& c : constants)
	{
		std::vector<std::string> vals;
		for (size_t i = 0; i < sources.size(); ++i)
			vals.push_back(extract(sources[i], c.patterns.size() == 1 ? c.patterns[0]
			                                                         : c.patterns[i]));
		// Empty means the pattern stopped matching everywhere — the code moved, and
		// the guard would silently pass. That is a failure too.
		CHECK_MESSAGE(!vals[0].empty(), "no match for '", c.name, "' in ", names[0],
		              " - the drift guard's pattern is stale");
		for (size_t i = 1; i < vals.size(); ++i)
			CHECK_MESSAGE(vals[i] == vals[0], c.name, " drifted: ", names[0], " has '",
			              vals[0], "', ", names[i], " has '", vals[i], "'");
	}
}

} // namespace shaderdrift

TEST_CASE("shaderdrift::readFile normalises CRLF, so multi-line needles match everywhere")
{
	// readFile's normalisation is load-bearing but invisible on the platforms most
	// of us develop on: `* text=auto` only materialises CRLF on the Windows
	// runner, so dropping it again would turn exactly one job red, and only for
	// multi-line needles (single-line regexes swallow the \r via \\s*). 619e098
	// shipped that state for two weeks. Pin the behaviour where every platform
	// checks it, instead of relying on Windows CI to notice a second time.
	namespace fs = std::filesystem;
	const fs::path tmp = fs::temp_directory_path() / "he_test_crlf_readfile.txt";
	{
		std::ofstream f(tmp, std::ios::binary);
		f << "float NdV = clamp(dot(n, V), 0.0, 1.0);\r\nvec3 fresnelSpec = specColor;\r\n";
	}
	const std::string text = shaderdrift::readFile(tmp);
	he_test::removeQuiet(tmp);

	CHECK(text.find('\r') == std::string::npos);
	CHECK(text.find("float NdV = clamp(dot(n, V), 0.0, 1.0);\n"
	                "vec3 fresnelSpec = specColor;") != std::string::npos);
}

TEST_CASE("GI kernels: the constants the hand-kept copies must share")
{
	using namespace shaderdrift;
	const fs::path root = findRepoRoot();
	if (root.empty())
	{
		// Not run from a checkout: there is nothing to compare, and failing here
		// would report a build layout, not shader drift.
		MESSAGE("GI shader sources not found - drift comparison skipped");
		return;
	}
	const fs::path sh = root / "src" / "HE_Rendering" / "shaders";

	SUBCASE("shadow-ray kernels")
	{
		const std::vector<const char*> names = { "gi_shadow.comp", "gi_shadow_hw.comp",
		                                         "gi_shadow_hw.hlsl" };
		std::vector<std::string> src;
		for (const char* n : names) src.push_back(stripLineComments(readFile(sh / n)));
		checkGroup(names, src, {
			{ "surface origin bias",  { R"(\+ N \* ([0-9.]+))" } },
			{ "sun ray tMin/tMax",    { R"(giSceneAnyHit\(origin, dir, ([0-9.]+), ([0-9.]+)\))" } },
			{ "local-light skip",     { R"(distL <= ([0-9.]+)\))" } },
			{ "local ray tMin/slack", { R"(dirL, ([0-9.]+), max\(distL - ([0-9.]+), ([0-9.]+)\)\))" } },
			{ "cone jitter hash",     { R"(sin\(dot\(p, \w*2\(([0-9.]+), ([0-9.]+)\)\)\) \* ([0-9.]+)\))" } },
			{ "workgroup size",       { R"(local_size_x = (\d+), local_size_y = (\d+))",
			                            R"(local_size_x = (\d+), local_size_y = (\d+))",
			                            R"(numthreads\((\d+), (\d+), 1\))" } },
		});
	}

	SUBCASE("DDGI probe-update kernels")
	{
		const std::vector<const char*> names = { "gi_probe.comp", "gi_probe_hw.comp",
		                                         "gi_probe_hw.hlsl" };
		std::vector<std::string> src;
		for (const char* n : names) src.push_back(stripLineComments(readFile(sh / n)));
		checkGroup(names, src, {
			{ "octahedral tile size", { R"(const int kOctSize = (\d+);)" } },
			{ "texel -> direction",   { R"(\w*2\(texel\) \+ ([0-9.]+)\) / float\(kOctSize\) \* ([0-9.]+) - ([0-9.]+))" } },
			{ "sun bounce ray",       { R"(hitNormal \* ([0-9.]+), uSunDirRadius\.xyz, ([0-9.]+), ([0-9.]+)\))" } },
			{ "local bounce ray",     { R"(hitNormal \* ([0-9.]+), L, ([0-9.]+), max\(d - ([0-9.]+), ([0-9.]+)\)\))" } },
			{ "grid spacing floors",  { R"(max\(uGridOrigin\.w, ([0-9.e+-]+)\))" } },
			{ "hysteresis clamp",     { R"(clamp\(uRayParams\.y, ([0-9.]+), ([0-9.]+)\))" } },
			{ "hysteresis converge",  { R"((?:mix|lerp)\(baseH, ([0-9.]+),)" } },
			{ "irradiance delta gain",{ R"(oldIrr\.rgb\) \* ([0-9.]+))" } },
			{ "octDecode body",       { R"(octDecode\((?:vec2|float2) e\)\s*\{([^}]*\}[^}]*)\})" } },
			{ "workgroup size",       { R"(local_size_x = (\d+), local_size_y = (\d+))",
			                            R"(local_size_x = (\d+), local_size_y = (\d+))",
			                            R"(numthreads\((\d+), (\d+), 1\))" } },
		});
	}

	SUBCASE("DDGI probe sampling on the shading side")
	{
		// kLightingPreamble (graph materials, every backend) vs. the Vulkan
		// built-in PBR shader. They must agree or a graph material and a built-in
		// material standing side by side disagree about indirect light.
		const std::string lib = readFile(root / "src" / "HE_Rendering" / "src" / "material" /
		                                 "MaterialShaderLibrary.cpp");
		// Anchored on the declaration itself, so the sync-comment block above it
		// (which names this test) cannot be mistaken for the literal's start.
		const size_t decl  = lib.find("kLightingPreamble = R\"(");
		const size_t open  = (decl == std::string::npos) ? std::string::npos
		                                                 : lib.find("R\"(", decl);
		const size_t close = (open == std::string::npos) ? std::string::npos
		                                                 : lib.find(")\";", open + 3);
		REQUIRE(close != std::string::npos); // the preamble literal moved or was renamed

		// The OpenGL backend keeps two MORE copies, as embedded string literals
		// rather than files: its built-in scene shader, and the GI-reflection
		// kernel, which shades a traced hit from the same probe field (a
		// reflected surface disagreeing with the same surface seen directly is
		// exactly the drift this catches). docs/gi-reflections-plan.md §7 asked
		// for them to be pinned here the moment a second copy appeared.
		const std::string gl = readFile(root / "src" / "HE_Rendering" / "src" /
		                                "Backends" / "OpenGL" / "OpenGLRenderer.cpp");
		auto glslLiteral = [&](const char* literal) -> std::string
		{
			const std::string decl = std::string("static const char* ") + literal + " = R\"GLSL(";
			const size_t body = gl.find(decl);
			REQUIRE(body != std::string::npos); // literal renamed/moved
			const size_t from = body + decl.size();
			const size_t to   = gl.find(")GLSL\";", from);
			REQUIRE(to != std::string::npos);
			return stripLineComments(gl.substr(from, to - from));
		};

		const std::vector<const char*> names = { "kLightingPreamble", "scene.frag",
		                                         "kUnlitFS (GL)", "kGiReflCS (GL)" };
		const std::vector<std::string> src = {
			stripLineComments(lib.substr(open + 3, close - open - 3)),
			stripLineComments(readFile(sh / "scene.frag")),
			glslLiteral("kUnlitFS"),
			glslLiteral("kGiReflCS"),
		};
		checkGroup(names, src, {
			{ "octEncode body",         { R"(OctEncode\(vec3 n\)\s*\{([^}]*)\})" } },
			{ "octahedral tile size",   { R"(const float kOct = ([0-9.]+);)",
			                              R"(const int GI_PROBE_OCT = (\d+);)",
			                              R"(const int GI_PROBE_OCT = (\d+);)",
			                              R"(const int kOctSize = (\d+);)" } },
			{ "probe distance floor",   { R"(max\(length\(toProbe\), ([0-9.e+-]+)\))" } },
			{ "trilinear weight cutoff",{ R"(weight <= ([0-9.e+-]+)\) continue)" } },
			{ "backface weight floor",  { R"(weight \*= max\(([0-9.]+), dot\(N, dirToProbe\))" } },
			{ "chebyshev sharpen",      { R"(chebyshev = (chebyshev \* chebyshev \* chebyshev);)" } },
			{ "chebyshev floor",        { R"(weight \*= max\(chebyshev, ([0-9.]+)\))" } },
			{ "irradiance normaliser",  { R"(sumColor / max\(sumWeight, ([0-9.e+-]+)\))" } },
		});
	}

	SUBCASE("SSR composite mirrors heLitP's Fresnel ambSpec term")
	{
		// The deferred reflection pass (kSSRCompositeFS) re-adds exactly the
		// specular-IBL term heLitP skips via heLight.ssr.w — if the two drift,
		// SSR-off deferred no longer matches forward. The roughness-aware
		// Schlick line must appear byte-identically in BOTH string literals.
		const std::string lib = readFile(root / "src" / "HE_Rendering" / "src" /
		                                 "material" / "MaterialShaderLibrary.cpp");
		const std::string needle =
			"        float NdV = clamp(dot(n, V), 0.0, 1.0);\n"
			"        vec3 fresnelSpec = specColor\n"
			"            + (max(vec3(1.0 - rough), specColor) - specColor) * pow(1.0 - NdV, 5.0);";
		const size_t first = lib.find(needle);
		REQUIRE(first != std::string::npos); // term moved/reworded — update BOTH copies + this needle
		CHECK(lib.find(needle, first + 1) != std::string::npos);
	}
}
