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
#include <HorizonRendering/SkyFrameParams.h>
#include <HorizonRendering/LightPacking.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <glm/gtc/matrix_transform.hpp>

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
	CHECK(p.neb2.x == doctest::Approx(0.6f));

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
