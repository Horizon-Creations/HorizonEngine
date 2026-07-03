#include "doctest.h"
#include <Math/AABB.h>
#include <HorizonRendering/FrustumCuller.h>
#include <HorizonRendering/RenderSorter.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <glm/gtc/matrix_transform.hpp>

TEST_CASE("RenderExtractor: real mesh bounds when a ContentManager is set, unit-cube otherwise")
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
	// Without one, it falls back to the unit-cube proxy (±0.5).
	{
		RenderExtractor ex;
		RenderWorld rw;
		ex.extract(world, rw, 1.0f);
		REQUIRE(rw.objects.size() == 1);
		CHECK(rw.objects[0].worldBounds.min.x == doctest::Approx(-0.5f));
		CHECK(rw.objects[0].worldBounds.max.x == doctest::Approx( 0.5f));
	}
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
