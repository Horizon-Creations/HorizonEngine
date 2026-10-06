#include "doctest.h"
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/Components/TrailComponent.h>
#include <HorizonScene/Components/NavAgentComponent.h>
#include <HorizonScene/FloatingOrigin.h>
#include <ContentManager/DefaultAssets.h>
#include <Net/BitStream.h>
#include <Renderer/IRenderer.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Thema 153, Schritt 4: larger worlds and more entities. The limits these pin
// are measured in docs/world-streaming-baseline-2026-10-06.md (§3.5, §3.6, §5):
// the transform walk paid sin/cos for every entity on every call, the Metal
// backend walked the registry once per pass, and Jolt stopped at 1024 bodies.

namespace
{
bool sameMatrix(const glm::mat4& a, const glm::mat4& b)
{
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			if (a[c][r] != b[c][r]) return false;
	return true;
}

bool nearMatrix(const glm::mat4& a, const glm::mat4& b, float eps)
{
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			if (std::fabs(a[c][r] - b[c][r]) > eps * (1.0f + std::fabs(b[c][r]))) return false;
	return true;
}

TransformComponent& tf(HorizonWorld& w, Entity e)
{
	return w.registry().get_or_emplace<TransformComponent>(e);
}

// The reference world's shape (scripts/perf/gen_reference_world.py): groups
// under the root, meshes under the groups.
std::vector<Entity> buildGroupedWorld(HorizonWorld& world, int groups, int perGroup,
                                      std::vector<Entity>* groupsOut = nullptr)
{
	std::vector<Entity> leaves;
	leaves.reserve(static_cast<size_t>(groups) * perGroup);
	for (int g = 0; g < groups; ++g)
	{
		const Entity grp = world.createEntity("Group");
		tf(world, grp).position = glm::vec3(static_cast<float>(g) * 8.0f, 0.0f, 0.0f);
		tf(world, grp).rotation = glm::vec3(0.0f, static_cast<float>(g % 360), 0.0f);
		if (groupsOut) groupsOut->push_back(grp);
		for (int i = 0; i < perGroup; ++i)
		{
			const Entity e = world.createEntity("Mesh");
			world.reparentEntity(e, grp);
			TransformComponent& t = tf(world, e);
			t.position = glm::vec3(static_cast<float>(i % 10), 0.5f, static_cast<float>(i / 10));
			t.rotation = glm::vec3(static_cast<float>(i), 15.0f, 0.0f);
			t.scale    = glm::vec3(1.0f + static_cast<float>(i % 3));
			leaves.push_back(e);
		}
	}
	return leaves;
}
} // namespace

// ─── Transform propagation: the local-matrix cache ──────────────────────────

TEST_CASE("propagateTransforms: a write that never sets dirty still reaches worldMatrix")
{
	HorizonWorld world;
	const Entity parent = world.createEntity("Parent");
	const Entity child  = world.createEntity("Child");
	world.reparentEntity(child, parent);
	tf(world, parent).position = glm::vec3(10.0f, 0.0f, 0.0f);
	tf(world, child).position  = glm::vec3(0.0f, 2.0f, 0.0f);

	HE::propagateTransforms(world);
	CHECK(tf(world, child).localCacheValid);
	const glm::mat4 before = tf(world, child).worldMatrix;

	// An Inspector drag or the gizmo: the value changes, the flag does not.
	tf(world, child).rotation = glm::vec3(0.0f, 90.0f, 0.0f);
	tf(world, child).dirty    = false;
	HE::propagateTransforms(world);

	const glm::mat4 expect = HE::localMatrix(tf(world, parent)) * HE::localMatrix(tf(world, child));
	CHECK_FALSE(sameMatrix(tf(world, child).worldMatrix, before));
	CHECK(sameMatrix(tf(world, child).worldMatrix, expect));
}

TEST_CASE("propagateTransforms: a moved parent carries its unchanged children, a reparent too")
{
	HorizonWorld world;
	const Entity a     = world.createEntity("A");
	const Entity b     = world.createEntity("B");
	const Entity child = world.createEntity("Child");
	world.reparentEntity(child, a);
	tf(world, a).position     = glm::vec3(1.0f, 0.0f, 0.0f);
	tf(world, b).position     = glm::vec3(0.0f, 0.0f, 50.0f);
	tf(world, b).rotation     = glm::vec3(0.0f, 45.0f, 0.0f);
	tf(world, child).position = glm::vec3(0.0f, 3.0f, 0.0f);
	HE::propagateTransforms(world);

	// The child's own values do not change; only what is above it does.
	tf(world, a).position = glm::vec3(-7.0f, 1.0f, 2.0f);
	HE::propagateTransforms(world);
	CHECK(sameMatrix(tf(world, child).worldMatrix,
	                 HE::localMatrix(tf(world, a)) * HE::localMatrix(tf(world, child))));

	REQUIRE(world.reparentEntity(child, b));
	HE::propagateTransforms(world);
	// reparentEntity keeps the WORLD pose by rewriting the local one; either
	// way the matrix must be B's world times the child's current local.
	CHECK(sameMatrix(tf(world, child).worldMatrix,
	                 HE::localMatrix(tf(world, b)) * HE::localMatrix(tf(world, child))));
	CHECK(nearMatrix(tf(world, child).worldMatrix, HE::worldMatrixOf(world, child), 1e-6f));
}

TEST_CASE("propagateTransforms: a cached matrix is bit-identical to computing it")
{
	HorizonWorld world;
	const Entity e = world.createEntity("E");
	TransformComponent& t = tf(world, e);
	t.position = glm::vec3(1234.5f, -6.25f, 0.125f);
	t.rotation = glm::vec3(33.0f, 271.0f, -12.5f);
	t.scale    = glm::vec3(0.5f, 2.0f, 3.0f);

	HE::propagateTransforms(world);
	const glm::mat4 first = tf(world, e).worldMatrix;
	HE::propagateTransforms(world);   // answered from the cache this time
	CHECK(sameMatrix(tf(world, e).worldMatrix, first));
	CHECK(sameMatrix(first, HE::localMatrix(tf(world, e))));
	CHECK(sameMatrix(HE::cachedLocalMatrix(tf(world, e)), HE::localMatrix(tf(world, e))));
	// The query side reads the same cache and must agree with the walk.
	CHECK(sameMatrix(HE::worldMatrixOf(world, e), first));
}

// ─── Entity count ───────────────────────────────────────────────────────────

TEST_CASE("100k entities: every world matrix matches its parent chain, before and after a move")
{
	HorizonWorld world;
	std::vector<Entity> groups;
	const std::vector<Entity> leaves = buildGroupedWorld(world, 1000, 100, &groups);
	REQUIRE(leaves.size() == 100000u);
	CHECK(world.registry().view<TransformComponent>().size() >= 101000u);

	HE::propagateTransforms(world);
	size_t cached = 0;
	for (auto [e, t] : world.registry().view<TransformComponent>().each())
		if (t.localCacheValid) ++cached;
	CHECK(cached == world.registry().view<TransformComponent>().size());

	for (size_t i = 0; i < leaves.size(); i += 997)
		CHECK(nearMatrix(tf(world, leaves[i]).worldMatrix, HE::worldMatrixOf(world, leaves[i]), 1e-6f));

	// One group moves: its hundred children follow, a neighbour's do not.
	const glm::mat4 neighbourBefore = tf(world, leaves[100]).worldMatrix;   // group 1
	tf(world, groups[0]).position += glm::vec3(0.0f, 100.0f, 0.0f);
	HE::propagateTransforms(world);
	for (int i = 0; i < 100; ++i)
		CHECK(tf(world, leaves[i]).worldMatrix[3].y > 99.0f);
	CHECK(sameMatrix(tf(world, leaves[100]).worldMatrix, neighbourBefore));
}

TEST_CASE("100k entities: the extractor sees every mesh")
{
	HorizonWorld world;
	const std::vector<Entity> leaves = buildGroupedWorld(world, 1000, 100);
	for (const Entity e : leaves)
	{
		MeshComponent mc;
		mc.meshAssetId = HE::kDefaultCubeMeshId;
		world.addComponent(e, mc);
	}
	RenderExtractor ex;
	RenderWorld rw;
	ex.extract(world, rw, 16.0f / 9.0f);
	CHECK(rw.objects.size() == leaves.size());
}

// ─── Render extraction: once per frame ──────────────────────────────────────

namespace
{
void buildSmallScene(HorizonWorld& world)
{
	for (int i = 0; i < 40; ++i)
	{
		const Entity e = world.createEntity("Mesh");
		tf(world, e).position = glm::vec3(static_cast<float>(i % 8) * 3.0f, 0.5f,
		                                  static_cast<float>(i / 8) * -3.0f);
		MeshComponent mc;
		mc.meshAssetId = HE::kDefaultCubeMeshId;
		world.addComponent(e, mc);
	}
	const Entity lamp = world.createEntity("Lamp");
	tf(world, lamp).position = glm::vec3(2.0f, 3.0f, -4.0f);
	LightComponent lc;
	lc.type        = HE::LightType::Point;
	lc.intensity   = 5.0f;
	lc.range       = 10.0f;
	lc.castsShadow = true;
	world.addComponent(lamp, lc);
}

EditorCameraOverride makeCam(const glm::vec3& eye)
{
	EditorCameraOverride cam;
	cam.active   = true;
	cam.position = eye;
	cam.view     = glm::lookAt(eye, glm::vec3(10.0f, 0.0f, -6.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	return cam;
}

void configure(RenderExtractor& ex)
{
	ex.setDayNight(true, 0.4f, glm::vec3(1.0f, 0.97f, 0.9f), 2.2f,
	               glm::vec3(0.55f, 0.65f, 0.95f), 0.66f, 0.2f);
	ex.setShadowSettings(120.0f, 3, 0.5f, 2048);
}

void checkSameWorld(const RenderWorld& a, const RenderWorld& b)
{
	REQUIRE(a.objects.size() == b.objects.size());
	for (size_t i = 0; i < a.objects.size(); ++i)
	{
		CHECK(a.objects[i].entityId == b.objects[i].entityId);
		CHECK(a.objects[i].meshAssetId == b.objects[i].meshAssetId);
		CHECK(sameMatrix(a.objects[i].transform, b.objects[i].transform));
	}
	REQUIRE(a.lights.size() == b.lights.size());
	for (size_t i = 0; i < a.lights.size(); ++i)
	{
		CHECK(a.lights[i].type == b.lights[i].type);
		CHECK(a.lights[i].intensity == b.lights[i].intensity);
		CHECK(a.lights[i].shadowLayer == b.lights[i].shadowLayer);
	}
	CHECK(sameMatrix(a.camera.view, b.camera.view));
	CHECK(sameMatrix(a.camera.projection, b.camera.projection));
	CHECK(a.shadow.enabled == b.shadow.enabled);
	CHECK(a.shadow.cascadeCount == b.shadow.cascadeCount);
	for (int c = 0; c < a.shadow.cascadeCount; ++c)
	{
		CHECK(sameMatrix(a.shadow.cascadeViewProj[c], b.shadow.cascadeViewProj[c]));
		CHECK(a.shadow.cascadeSplit[c] == b.shadow.cascadeSplit[c]);
	}
	CHECK(a.shadow.localLayerCount == b.shadow.localLayerCount);
	for (int l = 0; l < a.shadow.localLayerCount; ++l)
		CHECK(sameMatrix(a.shadow.localViewProj[l], b.shadow.localViewProj[l]));
	CHECK(a.sunDirection == b.sunDirection);
	CHECK(a.ambient == b.ambient);
}
} // namespace

TEST_CASE("RenderExtractor: inside a frame the second extract reuses the first walk")
{
	HorizonWorld world;
	buildSmallScene(world);
	const EditorCameraOverride cam = makeCam(glm::vec3(-5.0f, 6.0f, 8.0f));

	RenderExtractor ex;
	configure(ex);
	RenderWorld first, second;
	{
		RenderExtractor::FrameScope frame(ex);
		ex.extract(world, first, 1.5f, &cam);
		ex.extract(world, second, 1.5f, &cam);
	}
	CHECK(ex.fullExtractCount() == 1u);
	CHECK(ex.reusedExtractCount() == 1u);
	REQUIRE(first.objects.size() >= 40u);   // positive control: there was something to reuse
	CHECK(first.shadow.enabled);
	CHECK(first.shadow.localLayerCount > 0);   // the point light's cube faces
	checkSameWorld(second, first);
}

TEST_CASE("RenderExtractor: a reused extract at another aspect equals a full walk at it")
{
	HorizonWorld world;
	buildSmallScene(world);
	const EditorCameraOverride cam = makeCam(glm::vec3(-5.0f, 6.0f, 8.0f));

	RenderExtractor ex;
	configure(ex);
	RenderWorld scene, ssao;
	{
		RenderExtractor::FrameScope frame(ex);
		ex.extract(world, scene, 1920.0f / 1080.0f, &cam);
		// The SSAO pass: half resolution, rounded — a slightly different aspect.
		ex.extract(world, ssao, 960.0f / 541.0f, &cam);
	}
	CHECK(ex.reusedExtractCount() == 1u);

	RenderExtractor fresh;
	configure(fresh);
	RenderWorld reference;
	fresh.extract(world, reference, 960.0f / 541.0f, &cam);
	CHECK_FALSE(sameMatrix(scene.camera.projection, reference.camera.projection));
	checkSameWorld(ssao, reference);
}

TEST_CASE("RenderExtractor: nothing is reused outside a frame or with other inputs")
{
	HorizonWorld world;
	buildSmallScene(world);
	const EditorCameraOverride cam   = makeCam(glm::vec3(-5.0f, 6.0f, 8.0f));
	const EditorCameraOverride moved = makeCam(glm::vec3(-4.0f, 6.0f, 8.0f));

	RenderExtractor ex;
	configure(ex);
	RenderWorld rw;

	// No beginFrame: every call walks, exactly as before.
	ex.extract(world, rw, 1.5f, &cam);
	ex.extract(world, rw, 1.5f, &cam);
	CHECK(ex.fullExtractCount() == 2u);
	CHECK(ex.reusedExtractCount() == 0u);

	{
		RenderExtractor::FrameScope frame(ex);
		ex.extract(world, rw, 1.5f, &cam);
		ex.extract(world, rw, 1.5f, &moved);   // another camera
		ex.extract(world, rw, 1.5f);           // no editor camera at all
		ex.setDayNight(true, 0.7f, glm::vec3(1.0f), 2.0f, glm::vec3(0.5f), 0.5f, 0.2f);
		ex.extract(world, rw, 1.5f);           // another time of day
	}
	CHECK(ex.fullExtractCount() == 6u);
	CHECK(ex.reusedExtractCount() == 0u);

	// The next frame starts empty: an entity moved between frames is seen.
	const Entity e = world.createEntity("Late");
	tf(world, e).position = glm::vec3(0.0f, 50.0f, 0.0f);
	MeshComponent mc;
	mc.meshAssetId = HE::kDefaultCubeMeshId;
	world.addComponent(e, mc);
	const size_t before = rw.objects.size();
	{
		RenderExtractor::FrameScope frame(ex);
		ex.extract(world, rw, 1.5f);
	}
	CHECK(rw.objects.size() == before + 1);
}

// ─── Physics: past the old 1024-body wall ───────────────────────────────────

TEST_CASE("PhysicsWorld: a world with 3000 bodies builds all of them")
{
	HorizonWorld world;
	std::vector<Entity> statics;
	for (int i = 0; i < 3000; ++i)
	{
		const Entity e = world.createEntity("Crate");
		TransformComponent t;
		t.position = glm::vec3(static_cast<float>(i % 60) * 3.0f, 0.0f,
		                       static_cast<float>(i / 60) * 3.0f);
		world.addComponent(e, t);
		RigidBodyComponent rb;
		rb.type = RigidBodyType::Static;
		world.addComponent(e, rb);
		statics.push_back(e);
	}
	// Created last, so it is body 3001: the one the old limit refused first.
	const Entity faller = world.createEntity("Faller");
	TransformComponent ft;
	ft.position = glm::vec3(-50.0f, 20.0f, -50.0f);
	world.addComponent(faller, ft);
	RigidBodyComponent frb;
	frb.type = RigidBodyType::Dynamic;
	frb.mass = 1.0f;
	world.addComponent(faller, frb);

	PhysicsWorld phys;
	phys.initialize(world);
	int built = 0;
	for (const Entity e : statics)
		if (phys.hasPhysics(static_cast<uint32_t>(e))) ++built;
	CHECK(built == 3000);
	REQUIRE(phys.hasPhysics(static_cast<uint32_t>(faller)));

	for (int i = 0; i < 30; ++i) phys.step(world, 1.0f / 60.0f);
	CHECK(world.registry().get<TransformComponent>(faller).position.y < 19.0f);
}

// ─── World limit: large coordinates ─────────────────────────────────────────

TEST_CASE("Large coordinates: an entity 100 km out keeps its exact position through the walk")
{
	HorizonWorld world;
	const Entity e = world.createEntity("Far");
	tf(world, e).position = glm::vec3(100000.0f, 12.5f, -100000.0f);
	tf(world, e).rotation = glm::vec3(0.0f, 30.0f, 0.0f);
	HE::propagateTransforms(world);
	HE::propagateTransforms(world);   // cached
	const glm::vec3 p = glm::vec3(tf(world, e).worldMatrix[3]);
	CHECK(p == tf(world, e).position);
	CHECK(HE::worldPositionOf(world, e) == tf(world, e).position);
}

TEST_CASE("Large coordinates: replication carries a 30 km position once worldExtent covers it")
{
	// The extent is a project setting (Multiplayer > World extent, up to
	// 1000 km); 4096 m is only its default and positions beyond it clamp.
	const float pos  = 30000.123f;
	const int   bits = 24;
	auto roundTrip = [&](float extent) {
		HE::Net::BitWriter w;
		w.writeFloatQuantized(pos, -extent, extent, bits);
		const std::vector<std::uint8_t> bytes = w.data();
		HE::Net::BitReader r(bytes);
		float out = 0.0f;
		REQUIRE(r.readFloatQuantized(out, -extent, extent, bits));
		return out;
	};
	const float extent = 32768.0f;
	const float step   = 2.0f * extent / static_cast<float>((1u << bits) - 1u);
	CHECK(std::fabs(roundTrip(extent) - pos) <= step);
	CHECK(step < 0.005f);                              // under 5 mm at 32 km
	CHECK(roundTrip(4096.0f) == doctest::Approx(4096.0f));   // the default clamps
}

// ─── Floating origin (Thema 153, Schritt 5) ─────────────────────────────────

TEST_CASE("Floating origin: the shift is whole radii, and nothing inside the radius or when off")
{
	CHECK(HE::floatingOriginShift({ 7999.0f, -100.0f, 0.0f }, 8000.0f) == glm::vec3(0.0f));
	CHECK(HE::floatingOriginShift({ 1.0e6f, 0.0f, 0.0f }, 0.0f) == glm::vec3(0.0f));   // off
	CHECK(HE::floatingOriginShift({ 12000.0f, 5.0f, -25000.0f }, 8000.0f) ==
	      glm::vec3(16000.0f, 0.0f, -24000.0f));
	// The camera lands within half a radius of the new origin.
	const glm::vec3 cam(100001.5f, 40.0f, -99000.0f);
	const glm::vec3 s = HE::floatingOriginShift(cam, 8000.0f);
	CHECK(std::fabs(cam.x - s.x) <= 4000.0f);
	CHECK(std::fabs(cam.z - s.z) <= 4000.0f);
}

TEST_CASE("Floating origin: a shift keeps every absolute position, bodies included")
{
	HorizonWorld world;
	auto& reg = world.registry();
	const glm::vec3 far(100000.0f, 0.0f, -60000.0f);

	// A dynamic crate 100 km out over a static floor, a child under the crate,
	// and the runtime state that holds world positions of its own.
	const Entity floor = world.createEntity("Floor");
	tf(world, floor).position = far;
	tf(world, floor).scale    = glm::vec3(20.0f, 1.0f, 20.0f);
	RigidBodyComponent frb; frb.type = RigidBodyType::Static;
	world.addComponent(floor, frb);
	const Entity crate = world.createEntity("Crate");
	tf(world, crate).position = far + glm::vec3(1.5f, 5.0f, -2.0f);
	RigidBodyComponent crb; crb.type = RigidBodyType::Dynamic; crb.mass = 1.0f;
	world.addComponent(crate, crb);
	const Entity child = world.createEntity("Lamp");
	world.reparentEntity(child, crate);
	tf(world, child).position = glm::vec3(0.0f, 1.0f, 0.0f);
	ParticleSystemComponent ps;
	ps.particles.push_back(Particle{ far + glm::vec3(3.0f), glm::vec3(0.0f), 1.0f, 1.0f });
	world.addComponent(crate, ps);
	TrailComponent trail;
	trail.points.push_back({ far + glm::vec3(2.0f), 0.0f });
	world.addComponent(child, trail);
	NavAgentComponent agent;
	agent.targetPos = far + glm::vec3(10.0f, 0.0f, 0.0f);
	agent.path      = { far, far + glm::vec3(10.0f, 0.0f, 0.0f) };
	world.addComponent(floor, agent);

	PhysicsWorld phys;
	phys.initialize(world);
	REQUIRE(phys.hasPhysics(static_cast<uint32_t>(crate)));
	for (int i = 0; i < 10; ++i) phys.step(world, 1.0f / 60.0f);

	const auto absolute = [&](Entity e) { return glm::dvec3(HE::worldPositionOf(world, e)) + world.origin(); };
	const glm::dvec3 crateBefore = absolute(crate);
	const glm::dvec3 childBefore = absolute(child);
	const glm::vec3  matrixBefore(tf(world, crate).worldMatrix[3]);

	const glm::vec3 shift = HE::updateFloatingOrigin(world, &phys, HE::worldPositionOf(world, crate), 8000.0f);
	REQUIRE(shift == glm::vec3(104000.0f, 0.0f, -64000.0f));
	CHECK(world.origin() == glm::dvec3(104000.0, 0.0, -64000.0));
	const auto near = [](const glm::dvec3& a, const glm::dvec3& b) { return glm::length(a - b) < 0.01; };
	CHECK(near(absolute(crate), crateBefore));
	CHECK(near(absolute(child), childBefore));
	CHECK(glm::length(tf(world, crate).position) < 8000.0f);   // near the origin now
	CHECK(glm::vec3(tf(world, crate).worldMatrix[3]) == matrixBefore - shift);   // derived state too
	CHECK(reg.get<ParticleSystemComponent>(crate).particles[0].position == far + glm::vec3(3.0f) - shift);
	CHECK(reg.get<TrailComponent>(child).points[0].worldPos == far + glm::vec3(2.0f) - shift);
	CHECK(reg.get<NavAgentComponent>(floor).targetPos == far + glm::vec3(10.0f, 0.0f, 0.0f) - shift);
	CHECK(reg.get<NavAgentComponent>(floor).path[0] == far - shift);

	// The bodies moved with the world: the next steps continue the fall where
	// it was. Had Jolt kept the old coordinates, the step would write them back
	// into the transform, 120 km off.
	for (int i = 0; i < 10; ++i) phys.step(world, 1.0f / 60.0f);
	const glm::dvec3 crateAfter = absolute(crate);
	CHECK(std::fabs(crateAfter.x - crateBefore.x) < 0.01);
	CHECK(std::fabs(crateAfter.z - crateBefore.z) < 0.01);
	CHECK(crateAfter.y < crateBefore.y);   // still falling
	CHECK(crateAfter.y > double(far.y));   // and still above the floor it moved with

	// clear() puts the origin back.
	world.clear();
	CHECK(world.origin() == glm::dvec3(0.0));
}

// A measurement, not a check (does not run in CI): what one shift costs the
// frame it happens in, for the reference world's shape.
//   out/build/release/tests/he_tests --no-skip --test-case='Floating origin bench*'
TEST_CASE("Floating origin bench: one shift at 50k and 200k entities" * doctest::skip())
{
	for (const int groups : { 500, 2000 })
	{
		HorizonWorld world;
		buildGroupedWorld(world, groups, 100);
		HE::propagateTransforms(world);
		const auto t0 = std::chrono::steady_clock::now();
		HE::shiftWorldOrigin(world, nullptr, glm::vec3(8000.0f, 0.0f, 0.0f));
		const auto t1 = std::chrono::steady_clock::now();
		HE::propagateTransforms(world);   // the next extract's walk, which rebuilds the moved groups
		const auto t2 = std::chrono::steady_clock::now();
		const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
		MESSAGE((groups * 101) << " entities: shift " << ms(t1 - t0) << " ms, next walk " << ms(t2 - t1) << " ms");
	}
}

TEST_CASE("Floating origin: a millimetre step 100 km out survives only after the shift")
{
	// The witness for the precision table (docs/world-streaming-baseline-
	// 2026-10-06.md, 3.5): 100 km out a float step is 7.8 mm.
	HorizonWorld world;
	const Entity e = world.createEntity("Walker");
	tf(world, e).position = glm::vec3(100000.0f, 0.0f, 0.0f);
	const glm::vec3 step(0.001f, 0.0f, 0.0f);

	glm::vec3 p = tf(world, e).position + step;
	CHECK(p.x == 100000.0f);   // without a floating origin the step is lost

	HE::updateFloatingOrigin(world, nullptr, tf(world, e).position, 8000.0f);
	const float before = tf(world, e).position.x;
	p = tf(world, e).position + step;
	CHECK(p.x > before);       // with it, it arrives
	// Absolute within a quarter of a millimetre (a float step at 4 km).
	CHECK(std::fabs(double(p.x) + world.origin().x - 100000.001) < 0.00025);
}
