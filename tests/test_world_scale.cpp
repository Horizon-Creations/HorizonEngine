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
#include <HorizonScene/CellStreamer.h>
#include <HorizonScene/CellSplit.h>
#include <HorizonScene/SceneJsonParse.h>
#include <HorizonScene/CellPhysics.h>
#include <HorizonScene/Components/JointComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>
#include <functional>
#include <HorizonScene/SceneSerializer.h>
#include "TestFsUtil.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <thread>
#include <nlohmann/json.hpp>
#include <ContentManager/DefaultAssets.h>
#include <Net/BitStream.h>
#include <Renderer/IRenderer.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <set>
#include <vector>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif
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
	// reparentEntity leaves the local values alone, so the child's WORLD pose
	// changes (nothing of its own moved: only structureEpoch tells propagateTransforms);
	// the matrix must be B's world times the child's current local.
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

	// Like the backends: every pass extracts into the same RenderWorld.
	RenderExtractor ex;
	configure(ex);
	RenderWorld rw;
	const RenderObject* walked = nullptr;
	{
		RenderExtractor::FrameScope frame(ex);
		ex.extract(world, rw, 1.5f, &cam);
		walked = rw.objects.data();
		ex.extract(world, rw, 1.5f, &cam);
	}
	CHECK(ex.fullExtractCount() == 1u);
	CHECK(ex.reusedExtractCount() == 1u);
	CHECK(rw.objects.data() == walked);   // found in place: nothing was copied or reallocated
	REQUIRE(rw.objects.size() >= 40u);    // positive control: there was something to reuse
	CHECK(rw.shadow.enabled);
	CHECK(rw.shadow.localLayerCount > 0); // the point light's cube faces

	RenderExtractor fresh;
	configure(fresh);
	RenderWorld reference;
	fresh.extract(world, reference, 1.5f, &cam);
	checkSameWorld(rw, reference);
}

TEST_CASE("RenderExtractor: a reused extract at another aspect equals a full walk at it")
{
	HorizonWorld world;
	buildSmallScene(world);
	const EditorCameraOverride cam = makeCam(glm::vec3(-5.0f, 6.0f, 8.0f));

	RenderExtractor ex;
	configure(ex);
	RenderWorld rw, scene, ssao;
	{
		RenderExtractor::FrameScope frame(ex);
		ex.extract(world, rw, 1920.0f / 1080.0f, &cam);
		scene = rw;
		// The backends refine the bounds in place right after the walk. The tail below must
		// not read them: the cascades are the ones the shadow pass fit from the walk's own.
		for (RenderObject& o : rw.objects)
		{
			o.worldBounds = HE::AABB{};
			o.worldBounds.expand(glm::vec3(-1000.0f));
			o.worldBounds.expand(glm::vec3(1000.0f));
		}
		// The SSAO pass: half resolution, rounded — a slightly different aspect.
		ex.extract(world, rw, 960.0f / 541.0f, &cam);
		ssao = rw;
		// ...and the next pass is back at the scene's aspect.
		ex.extract(world, rw, 1920.0f / 1080.0f, &cam);
	}
	CHECK(ex.fullExtractCount() == 1u);
	CHECK(ex.reusedExtractCount() == 2u);

	RenderExtractor fresh;
	configure(fresh);
	RenderWorld referenceSsao, referenceScene;
	fresh.extract(world, referenceSsao, 960.0f / 541.0f, &cam);
	fresh.extract(world, referenceScene, 1920.0f / 1080.0f, &cam);
	CHECK_FALSE(sameMatrix(scene.camera.projection, referenceSsao.camera.projection));
	checkSameWorld(ssao, referenceSsao);
	checkSameWorld(rw, referenceScene);   // the cascades of the shadow pass are back, bit for bit
	checkSameWorld(scene, referenceScene);
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

TEST_CASE("RenderExtractor: the frame's state is the caller's RenderWorld, passes edit it in place")
{
	// Thema 162, Schritt 2: the walk is not copied out of the extractor and back. What a pass
	// does to it between two extracts (the backends replace the bounds by the real mesh bounds
	// and resolve the material scalars, both idempotent) is still there for the next one.
	HorizonWorld world;
	buildSmallScene(world);
	const EditorCameraOverride cam = makeCam(glm::vec3(-5.0f, 6.0f, 8.0f));

	RenderExtractor ex;
	configure(ex);
	RenderWorld rw;
	RenderExtractor::FrameScope frame(ex);
	ex.extract(world, rw, 1.5f, &cam);
	REQUIRE(rw.objects.size() >= 40u);
	const RenderObject* walked = rw.objects.data();
	rw.objects[3].roughness = 0.125f;   // a pass resolving a material
	ex.extract(world, rw, 1.5f, &cam);
	ex.extract(world, rw, 1.5f, &cam);
	CHECK(ex.fullExtractCount() == 1u);
	CHECK(ex.reusedExtractCount() == 2u);
	CHECK(rw.objects.data() == walked);
	CHECK(rw.objects[3].roughness == 0.125f);
}

TEST_CASE("RenderExtractor: another RenderWorld inside a frame is another consumer and gets its own walk")
{
	HorizonWorld world;
	buildSmallScene(world);
	const EditorCameraOverride cam = makeCam(glm::vec3(-5.0f, 6.0f, 8.0f));

	RenderExtractor ex;
	configure(ex);
	RenderWorld a, b;
	{
		RenderExtractor::FrameScope frame(ex);
		ex.extract(world, a, 1.5f, &cam);
		ex.extract(world, b, 1.5f, &cam);   // not served from `a`: its owner may be gone by now
		ex.extract(world, b, 1.5f, &cam);   // the walk is in `b` from here on
	}
	CHECK(ex.fullExtractCount() == 2u);
	CHECK(ex.reusedExtractCount() == 1u);
	checkSameWorld(b, a);
}

TEST_CASE("RenderExtractor: a RenderWorld that was cleared or resized since the walk no longer holds it")
{
	HorizonWorld world;
	buildSmallScene(world);
	const EditorCameraOverride cam = makeCam(glm::vec3(-5.0f, 6.0f, 8.0f));

	RenderExtractor ex;
	configure(ex);
	RenderWorld rw;
	RenderExtractor::FrameScope frame(ex);
	ex.extract(world, rw, 1.5f, &cam);
	const size_t objects = rw.objects.size();
	const size_t lights  = rw.lights.size();
	REQUIRE(objects >= 40u);
	REQUIRE(lights >= 1u);

	rw.objects.pop_back();                      // somebody edited the walk's result
	ex.extract(world, rw, 1.5f, &cam);
	CHECK(ex.fullExtractCount() == 2u);
	CHECK(rw.objects.size() == objects);

	rw.lights.clear();
	ex.extract(world, rw, 1.5f, &cam);
	CHECK(ex.fullExtractCount() == 3u);
	CHECK(rw.lights.size() == lights);

	rw.clear();                                 // ...or wiped it
	ex.extract(world, rw, 1.5f, &cam);
	CHECK(ex.fullExtractCount() == 4u);
	CHECK(rw.objects.size() == objects);
	CHECK(ex.reusedExtractCount() == 0u);
}

TEST_CASE("RenderExtractor: a structure change between two extracts of a frame forces a new walk")
{
	// Thema 162 reads HorizonWorld::structureEpoch (Thema 164, plan 7.1/7.2). The frame copy is
	// only valid while the world holds the same entities in the same places: a cell streamed in
	// or out between two passes would otherwise be answered from the copy of the old world.
	HorizonWorld world;
	buildSmallScene(world);
	const EditorCameraOverride cam = makeCam(glm::vec3(-5.0f, 6.0f, 8.0f));

	RenderExtractor ex;
	configure(ex);
	RenderWorld rw;
	{
		RenderExtractor::FrameScope frame(ex);
		ex.extract(world, rw, 1.5f, &cam);
		const size_t before = rw.objects.size();
		ex.extract(world, rw, 1.5f, &cam);
		REQUIRE(ex.fullExtractCount() == 1u);
		REQUIRE(ex.reusedExtractCount() == 1u);

		// A cell arrives between two passes.
		const Entity late = world.createEntity("Late");
		tf(world, late).position = glm::vec3(0.0f, 50.0f, 0.0f);
		MeshComponent mc;
		mc.meshAssetId = HE::kDefaultCubeMeshId;
		world.addComponent(late, mc);
		ex.extract(world, rw, 1.5f, &cam);
		CHECK(ex.fullExtractCount() == 2u);        // not answered from the walk of the old world
		CHECK(rw.objects.size() == before + 1);

		// The new walk is the frame's from here on.
		ex.extract(world, rw, 1.5f, &cam);
		CHECK(ex.fullExtractCount() == 2u);
		CHECK(ex.reusedExtractCount() == 2u);
	}
}

TEST_CASE("RenderExtractor: every kind of structure change invalidates the frame copy")
{
	HorizonWorld world;
	buildSmallScene(world);
	const Entity a = world.createEntity("A");
	const Entity b = world.createEntity("B");
	const Entity c = world.createEntity("C");
	const EditorCameraOverride cam = makeCam(glm::vec3(-5.0f, 6.0f, 8.0f));

	struct Change { const char* what; std::function<void()> apply; };
	const Change changes[] = {
		{ "createEntity",         [&] { (void)world.createEntity("New"); } },
		{ "reparentEntity",       [&] { REQUIRE(world.reparentEntity(a, b)); } },
		{ "placeNextTo",          [&] { REQUIRE(world.placeNextTo(c, b, /*after=*/false)); } },
		{ "destroyEntity",        [&] { world.destroyEntity(c); } },
		{ "noteStructureChanged", [&] { world.noteStructureChanged(); } },
	};

	RenderExtractor ex;
	configure(ex);
	RenderWorld rw;
	uint64_t walks = 0;
	{
		RenderExtractor::FrameScope frame(ex);
		ex.extract(world, rw, 1.5f, &cam);
		++walks;
		for (const Change& ch : changes)
		{
			ex.extract(world, rw, 1.5f, &cam);   // answered from the copy: nothing moved yet
			CHECK_MESSAGE(ex.fullExtractCount() == walks, ch.what);
			const uint64_t before = world.structureEpoch();
			ch.apply();
			CHECK_MESSAGE(world.structureEpoch() != before, ch.what);   // the counter is what the key relies on
			ex.extract(world, rw, 1.5f, &cam);
			++walks;
			CHECK_MESSAGE(ex.fullExtractCount() == walks, ch.what);
		}
	}
}

// Thema 162, Schritt 2: what one extract costs against what the frame copy costs, at the sizes
// of the ladder. Every pass of a Metal or Vulkan frame after the first one is a "reuse", and a
// reuse is a deep copy of the RenderWorld (out = *frameCopy); the first walk of the frame also
// pays one (*frameCopy = out). This puts numbers on all three. Not part of the normal run:
//   out/build/release/tests/he_tests --no-skip --test-case='Extraction bench: the frame copy*'
TEST_CASE("Extraction bench: the frame copy against the walk it saves" * doctest::skip())
{
	using Clock = std::chrono::steady_clock;
	const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
	const auto median = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };

	for (const int n : { 1000, 10000, 50000, 100000 })
	{
		HorizonWorld world;
		const std::vector<Entity> leaves = buildGroupedWorld(world, n / 100, 100);
		for (const Entity e : leaves)
		{
			MeshComponent mc;
			mc.meshAssetId = HE::kDefaultCubeMeshId;
			world.addComponent(e, mc);
		}
		const Entity sun = world.createEntity("Sun");
		LightComponent lc;
		lc.type        = HE::LightType::Directional;
		lc.intensity   = 3.0f;
		lc.castsShadow = true;
		world.addComponent(sun, lc);

		RenderExtractor ex;
		RenderWorld rw, copy, second;
		ex.extract(world, rw, 16.0f / 9.0f);   // sizes every vector: the timings below are steady state
		copy   = rw;
		second = rw;

		std::vector<double> walk, deepCopy, framed;
		for (int i = 0; i < 7; ++i)
		{
			Clock::time_point t0 = Clock::now();
			ex.extract(world, rw, 16.0f / 9.0f);
			walk.push_back(ms(Clock::now() - t0));

			t0 = Clock::now();
			copy = rw;
			deepCopy.push_back(ms(Clock::now() - t0));

			// A frame of two passes: one walk (+ the snapshot) and one reuse.
			t0 = Clock::now();
			{
				RenderExtractor::FrameScope frame(ex);
				ex.extract(world, rw, 16.0f / 9.0f);
				ex.extract(world, second, 16.0f / 9.0f);
			}
			framed.push_back(ms(Clock::now() - t0));
		}
		MESSAGE(rw.objects.size() << " objects: walk " << median(walk) << " ms, deep copy of the result "
		        << median(deepCopy) << " ms, frame of walk + 1 reuse " << median(framed) << " ms");
	}
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

// A measurement, not a check (does not run in CI): what the id index costs when an
// entity is made, and what it saves when an id is looked up (Thema 164, step 2a).
// The "without" column is the same creation work on a registry nobody listens to,
// which is what createEntity cost before the index; the scan is the old
// findByEntityId, run for 200 ids only because all of them would take minutes.
//   out/build/release/tests/he_tests --no-skip --test-case='Id index bench*'
TEST_CASE("Id index bench: creating entities and resolving ids, against the scan it replaced" * doctest::skip())
{
	using Clock = std::chrono::steady_clock;
	const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
	for (const int n : { 50000, 200000 })
	{
		HorizonWorld world;
		std::vector<HE::UUID> ids;
		ids.reserve(static_cast<size_t>(n));
		const auto t0 = Clock::now();
		for (int i = 0; i < n; ++i) ids.push_back(world.entityId(world.createEntity("E")));
		const auto t1 = Clock::now();

		entt::registry bare;
		const Entity root = bare.create();
		bare.emplace<HierarchyComponent>(root);
		const auto t2 = Clock::now();
		for (int i = 0; i < n; ++i)
		{
			const Entity e = bare.create();
			bare.emplace<NameComponent>(e, NameComponent{ "E" });
			bare.emplace<HierarchyComponent>(e);
			bare.emplace<EntityIdComponent>(e, EntityIdComponent{ HE::UUID::generate() });
			bare.get<HierarchyComponent>(root).children.push_back(e);
			bare.get<HierarchyComponent>(e).parent = root;
		}
		const auto t3 = Clock::now();

		const auto t4 = Clock::now();
		size_t found = 0;
		for (const HE::UUID& id : ids) found += world.findByEntityId(id) != entt::null ? 1 : 0;
		const auto t5 = Clock::now();
		size_t scanned = 0;
		const int scans = 200;
		for (int i = 0; i < scans; ++i)
		{
			const HE::UUID& want = ids[static_cast<size_t>(i) * ids.size() / static_cast<size_t>(scans)];
			for (auto [e, c] : world.registry().view<const EntityIdComponent>().each())
				if (c.id == want) { ++scanned; break; }
		}
		const auto t6 = Clock::now();
		MESSAGE(n << " entities: create " << ms(t1 - t0) << " ms with the index, " << ms(t3 - t2)
		        << " ms without; " << n << " lookups " << ms(t5 - t4) << " ms by index (" << found
		        << " found), " << scans << " lookups " << ms(t6 - t5) << " ms by scan (" << scanned
		        << " found)");
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

// ─── Cell streaming (Thema 153, Schritt 5) ───────────────────────────────────

namespace
{
// A cell file as scripts/split_scene_cells.py writes one: a scene whose root
// holds the placed things of one grid square, at absolute positions.
void writeCell(const std::filesystem::path& file, const std::vector<glm::vec3>& positions)
{
	HorizonWorld cell;
	for (const glm::vec3& p : positions)
	{
		const Entity e = cell.createEntity("Prop");
		tf(cell, e).position = p;
		MeshComponent mc;
		mc.meshAssetId = HE::kDefaultCubeMeshId;
		cell.addComponent(e, mc);
	}
	std::filesystem::create_directories(file.parent_path());
	SceneSerializer ser;
	REQUIRE(ser.save(cell, file, SerializeFormat::JSON));
}

size_t countMeshes(HorizonWorld& world)
{
	size_t n = 0;
	for (auto e : world.registry().view<MeshComponent>()) { (void)e; ++n; }
	return n;
}

// Pumps the streamer until `done` or a few seconds have passed. No sleep: in
// the low-power mode of the measuring Mac a 2 ms sleep takes ~150 ms.
template <class Done>
void pumpCells(HE::CellStreamer& s, HorizonWorld& world, const glm::dvec3& cam, const glm::vec3& vel, Done done)
{
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (std::chrono::steady_clock::now() < until)
	{
		s.update(world, cam, vel, 100.0);
		if (done() && s.stats().inFlight == 0 && s.stats().ready == 0 && s.stats().building == 0) return;
		std::this_thread::yield();
	}
}
} // namespace

TEST_CASE("CellManifest: reads the splitter's object, refuses a malformed one")
{
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(
		R"({"cellSize": 100, "loadRadius": 60, "unloadRadius": 120, "lookaheadSec": 1.5,
		    "dir": "Content/W.cells", "list": [[0, 0, 3], [-1, 2, 7]]})", m));
	CHECK(m.cellSize == 100.0f);
	CHECK(m.unloadRadius == 120.0f);
	CHECK(m.lookaheadSec == 1.5f);
	REQUIRE(m.cells.size() == 2);
	CHECK(m.cells[1].x == -1);
	CHECK(m.cells[1].z == 2);
	CHECK(m.cellPath(-1, 2) == "Content/W.cells/cell_-1_2.hescene");
	CHECK(HE::CellManifest::cellIndex(-0.5, 100.0f) == -1);   // floor, not truncation
	CHECK(HE::CellManifest::cellIndex(99.9, 100.0f) == 0);
	// A manifest from before the format had a version is version 1, and its rows
	// end at three columns.
	CHECK(m.version == 1);
	CHECK(m.cells[0].bodies == 0u);
	// Version 2 says so, and adds the number of entities that can own a body.
	REQUIRE(HE::CellManifest::parse(
		R"({"version": 2, "cellSize": 100, "dir": "d", "list": [[0, 0, 5, 3], [1, 0, 6]]})", m));
	CHECK(m.version == 2);
	REQUIRE(m.cells.size() == 2);
	CHECK(m.cells[0].entities == 5u);
	CHECK(m.cells[0].bodies == 3u);
	CHECK(m.cells[1].bodies == 0u);   // a short row is not an error

	CHECK_FALSE(HE::CellManifest::parse("not json", m));
	CHECK(m.empty());
	CHECK_FALSE(HE::CellManifest::parse(R"({"cellSize": 0, "dir": "d", "list": [[0,0]]})", m));
	CHECK_FALSE(HE::CellManifest::parse(R"({"cellSize": 10, "list": [[0,0]]})", m));          // no dir
	CHECK_FALSE(HE::CellManifest::parse(R"({"cellSize": 10, "dir": "d", "list": [["a",0]]})", m));
	// An unload radius below the load radius would flicker: it is raised to it.
	REQUIRE(HE::CellManifest::parse(R"({"cellSize": 10, "loadRadius": 50, "unloadRadius": 20,
	                                    "dir": "d", "list": []})", m));
	CHECK(m.unloadRadius == 50.0f);
}

TEST_CASE("CellManifest::around: what the game would load, keep or drop from a viewpoint")
{
	// Squares of 100 m; load within 60 m of a square, keep up to 120 m.
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(
		R"({"cellSize": 100, "loadRadius": 60, "unloadRadius": 120, "dir": "d",
		    "list": [[0, 0, 5], [1, 0, 6], [2, 0, 7], [3, 0, 8], [-1, -1, 9]]})", m));
	// Standing in cell 0,0 at x = 50: cell 1,0 starts 50 m away (load), 2,0 at
	// 150 m (out), -1,-1 is 50 m off in both axes: sqrt(50²+50²) ≈ 70.7 (keep).
	const glm::dvec3 eye(50.0, 0.0, 50.0);
	CHECK(m.distanceTo(eye, 0, 0) == 0.0);
	CHECK(m.distanceTo(eye, 1, 0) == doctest::Approx(50.0));
	const std::vector<HE::CellManifest::View> v = m.around(eye, 200.0);
	REQUIRE(v.size() == 4);   // 3,0 is 250 m away, beyond the range asked for
	CHECK(v[0].x == 0);   CHECK(v[0].reach == HE::CellManifest::View::Reach::Load);
	CHECK(v[0].entities == 5u);
	CHECK(v[1].x == 1);   CHECK(v[1].reach == HE::CellManifest::View::Reach::Load);
	CHECK(v[2].x == -1);  CHECK(v[2].reach == HE::CellManifest::View::Reach::Keep);
	CHECK(v[2].distance == doctest::Approx(std::sqrt(2.0) * 50.0));
	CHECK(v[3].x == 2);   CHECK(v[3].reach == HE::CellManifest::View::Reach::Out);
	// Nearest first, and nothing for an empty manifest.
	for (size_t i = 1; i < v.size(); ++i) CHECK(v[i - 1].distance <= v[i].distance);
	CHECK(HE::CellManifest{}.around(eye, 1e9).empty());
}

TEST_CASE("SceneSerializer::lastLoadTiming: the last whole-scene load, JSON and binary")
{
	HorizonWorld world;
	world.createEntity("A");
	world.createEntity("B");
	SceneSerializer ser;
	for (const SerializeFormat format : { SerializeFormat::JSON, SerializeFormat::Binary })
	{
		const auto file = std::filesystem::temp_directory_path() / "he_last_load_timing.hescene";
		REQUIRE(ser.save(world, file, format));
		HorizonWorld back;
		REQUIRE(ser.load(back, file, format));
		const SceneSerializer::LoadTiming t = SceneSerializer::lastLoadTiming();
		CHECK(t.path == file.string());
		CHECK(t.binary == (format == SerializeFormat::Binary));
		CHECK(t.entities >= 2u);
		CHECK(t.parseMs >= 0.0);
		CHECK(t.buildMs >= 0.0);
		he_test::removeQuiet(file);
	}
}

TEST_CASE("CellStreamer: cells come and go with the camera, ahead of it, and under a floating origin")
{
	const auto root = std::filesystem::temp_directory_path() / "he_cell_stream";
	he_test::removeAllQuiet(root);
	// A row of four 100 m cells along +X, three props each, in the middle of the square.
	for (int x = 0; x < 4; ++x)
		writeCell(root / "W.cells" / ("cell_" + std::to_string(x) + "_0.hescene"),
		          { { x * 100.0f + 40.0f, 0.0f, 50.0f }, { x * 100.0f + 50.0f, 0.0f, 50.0f },
		            { x * 100.0f + 60.0f, 0.0f, 50.0f } });
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(
		R"({"cellSize": 100, "loadRadius": 60, "unloadRadius": 120, "lookaheadSec": 2,
		    "dir": "W.cells", "list": [[0,0,3],[1,0,3],[2,0,3],[3,0,3],[4,0,3]]})", m));
	// Cell 4 is in the list but has no file.

	int loadedHook = 0, unloadingHook = 0;
	HE::CellStreamer::Hooks hooks;
	hooks.loaded    = [&](entt::entity, const std::vector<entt::entity>& created)
	{
		++loadedHook;
		CHECK(created.size() == 4);   // the cell's root and its three props
	};
	hooks.unloading = [&](entt::entity) { ++unloadingHook; };
	auto reader = [&root](const std::string& path) -> std::function<bool(std::vector<uint8_t>&)>
	{
		const auto file = root / path;
		if (!std::filesystem::exists(file)) return {};
		return [file](std::vector<uint8_t>& out)
		{
			std::ifstream in(file, std::ios::binary);
			out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			return !out.empty();
		};
	};

	HorizonWorld world;
	HE::CellStreamer s;
	s.begin(m, reader, hooks);
	REQUIRE(s.active());

	// Standing in cell 0: it and cell 1 (50 m away) come in, cell 2 (150 m) does not.
	pumpCells(s, world, { 50.0, 1.7, 50.0 }, glm::vec3(0.0f), [&] { return s.stats().loaded == 2; });
	CHECK(s.isLoaded(0, 0));
	CHECK(s.isLoaded(1, 0));
	CHECK_FALSE(s.isLoaded(2, 0));
	CHECK(countMeshes(world) == 6);
	CHECK(loadedHook == 2);
	bool absolute = true;
	for (auto [e, mc] : world.registry().view<MeshComponent>().each())
	{
		const glm::vec3 p = HE::worldPositionOf(world, e);
		absolute = absolute && p.z == 50.0f && p.x >= 40.0f && p.x <= 160.0f;
	}
	CHECK(absolute);

	// 70 m from cell 0: no new load would start there, but it stays (unload at 120 m).
	pumpCells(s, world, { 170.0, 1.7, 50.0 }, glm::vec3(0.0f), [] { return true; });
	CHECK(s.isLoaded(0, 0));

	// Over at cell 3: 0 and 1 go, 2 and 3 come.
	pumpCells(s, world, { 350.0, 1.7, 50.0 }, glm::vec3(0.0f),
	          [&] { return s.isLoaded(2, 0) && s.isLoaded(3, 0); });
	CHECK_FALSE(s.isLoaded(0, 0));
	CHECK_FALSE(s.isLoaded(1, 0));
	CHECK(s.isLoaded(2, 0));
	CHECK(s.isLoaded(3, 0));
	CHECK(countMeshes(world) == 6);
	CHECK(unloadingHook == 2);
	// Cell 4, now 50 m away, has no file: it fails once and is not asked for again.
	CHECK(s.stats().failed == 1);
	pumpCells(s, world, { 350.0, 1.7, 50.0 }, glm::vec3(0.0f), [] { return true; });
	CHECK(s.stats().failed == 1);

	s.clear(world);
	CHECK(countMeshes(world) == 0);
	CHECK_FALSE(s.active());

	// Lookahead: standing in cell 0 but moving +X at 100 m/s, 2 s ahead is x = 250,
	// so cell 3 (50 m from there) is wanted already.
	s.begin(m, reader, hooks);
	pumpCells(s, world, { 50.0, 1.7, 50.0 }, glm::vec3(100.0f, 0.0f, 0.0f),
	          [&] { return s.isLoaded(3, 0); });
	CHECK(s.isLoaded(0, 0));
	CHECK(s.isLoaded(3, 0));
	s.clear(world);

	// Floating origin: the world's 0,0,0 sits at absolute x = 200. The camera is
	// given absolute; the cell lands where it belongs in absolute terms.
	world.setOrigin({ 200.0, 0.0, 0.0 });
	s.begin(m, reader, hooks);
	pumpCells(s, world, { 250.0, 1.7, 50.0 }, glm::vec3(0.0f), [&] { return s.isLoaded(2, 0); });
	REQUIRE(s.isLoaded(2, 0));
	bool placed = false;
	for (auto [e, mc] : world.registry().view<MeshComponent>().each())
	{
		const glm::dvec3 abs = glm::dvec3(HE::worldPositionOf(world, e)) + world.origin();
		if (std::fabs(abs.x - 250.0) < 1e-3) placed = true;   // cell 2's middle prop
	}
	CHECK(placed);
	s.clear(world);

	// A world cleared under it (a level change): the streamer notices and loads again.
	s.begin(m, reader, hooks);
	pumpCells(s, world, { 50.0, 1.7, 50.0 }, glm::vec3(0.0f), [&] { return s.stats().loaded == 2; });
	world.clear();
	pumpCells(s, world, { 50.0, 1.7, 50.0 }, glm::vec3(0.0f), [&] { return s.stats().loaded == 2; });
	CHECK(countMeshes(world) == 6);
	s.clear(world);
	he_test::removeAllQuiet(root);
}

TEST_CASE("Cell manifest: survives saving and loading the base scene, JSON and binary; clear() drops it")
{
	const std::string cells = R"({"cellSize":100.0,"dir":"W.cells","list":[[0,0,3]],"loadRadius":60.0})";
	HorizonWorld world;
	world.setCellManifestJson(cells);
	world.createEntity("Sky stand-in");
	SceneSerializer ser;
	for (const SerializeFormat format : { SerializeFormat::JSON, SerializeFormat::Binary })
	{
		const auto file = std::filesystem::temp_directory_path() / "he_cell_manifest.hescene";
		REQUIRE(ser.save(world, file, format));
		HorizonWorld back;
		REQUIRE(ser.load(back, file, format));
		CHECK(nlohmann::json::parse(back.cellManifestJson()) == nlohmann::json::parse(cells));
		HE::CellManifest m;
		CHECK(HE::CellManifest::parse(back.cellManifestJson(), m));
		back.clear();
		CHECK(back.cellManifestJson().empty());
		he_test::removeQuiet(file);
	}
}

// ─── Cells in slices, several anchors, the structure epoch (Thema 164, 2b) ────

namespace
{
// A cell as the C++ splitter writes one: the cell's root with `houses` subtrees below it,
// each a house with `parts` meshes. Positions are absolute, in the square that starts at
// x0. With `head` the "streaming" head of format version 2 is added, so that the streamer
// keeps the ids; without it the cell loads with fresh ones, like one from the Python script.
void writeHouses(const std::filesystem::path& file, int houses, int parts, float x0, bool head = true)
{
	HorizonWorld cell;
	for (int h = 0; h < houses; ++h)
	{
		const Entity house = cell.createEntity("House " + std::to_string(h));
		tf(cell, house).position = { x0 + 1.0f + static_cast<float>(h) * 0.5f, 0.0f, 50.0f };
		tf(cell, house).rotation = { 0.0f, static_cast<float>(h * 7), 0.0f };
		for (int p = 0; p < parts; ++p)
		{
			const Entity part = cell.createEntity("Part " + std::to_string(p));
			cell.reparentEntity(part, house);
			tf(cell, part).position = { static_cast<float>(p), 1.0f, 0.5f };
			MeshComponent mc;
			mc.meshAssetId = HE::kDefaultCubeMeshId;
			cell.addComponent(part, mc);
		}
	}
	std::filesystem::create_directories(file.parent_path());
	SceneSerializer ser;
	REQUIRE(ser.save(cell, file, SerializeFormat::JSON));
	if (!head) return;
	nlohmann::json j;
	{
		std::ifstream in(file);
		j = nlohmann::json::parse(in);
	}
	j["streaming"] = { { "version", 2 } };
	std::ofstream out(file, std::ios::trunc);
	out << j.dump();
}

nlohmann::json readScene(const std::filesystem::path& file)
{
	std::ifstream in(file);
	return nlohmann::json::parse(in);
}

HE::CellStreamer::Reader diskReader(const std::filesystem::path& root)
{
	return [root](const std::string& path) -> std::function<bool(std::vector<uint8_t>&)>
	{
		const auto file = root / path;
		if (!std::filesystem::exists(file)) return {};
		return [file](std::vector<uint8_t>& out)
		{
			std::ifstream in(file, std::ios::binary);
			out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			return !out.empty();
		};
	};
}

HE::CellAnchor anchorAt(double x, float scale = 1.0f, double z = 50.0)
{
	HE::CellAnchor a;
	a.position    = { x, 1.7, z };
	a.radiusScale = scale;
	return a;
}

// Updates until `done` (and nothing reading, parsed or half built) or twenty seconds.
template <class Done>
void pumpAnchors(HE::CellStreamer& s, HorizonWorld& world, const std::vector<HE::CellAnchor>& anchors,
                 double budgetMs, Done done)
{
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (std::chrono::steady_clock::now() < until)
	{
		s.update(world, anchors, budgetMs);
		if (done() && s.stats().inFlight == 0 && s.stats().ready == 0 && s.stats().building == 0) return;
		std::this_thread::yield();
	}
}

size_t entityCount(HorizonWorld& world) { return world.registry().storage<entt::entity>().in_use(); }
} // namespace

TEST_CASE("HorizonWorld::structureEpoch: the set of entities, parents and sibling order move it; values do not")
{
	HorizonWorld w;
	const uint64_t start = w.structureEpoch();
	CHECK(start != 0);   // a cache that remembers 0 never matches by accident

	uint64_t last = start;
	const auto moved = [&]
	{
		const bool changed = w.structureEpoch() != last;
		last = w.structureEpoch();
		return changed;
	};

	const Entity a = w.createEntity("A");
	CHECK(moved());
	const Entity b = w.createEntity("B");
	CHECK(moved());
	REQUIRE(w.reparentEntity(b, a));
	CHECK(moved());
	REQUIRE(w.reparentEntity(b, a));   // already there: nothing happened
	CHECK_FALSE(moved());
	const Entity c = w.createEntity("C");
	moved();
	REQUIRE(w.reparentEntity(c, a));
	moved();
	CHECK(w.moveChild(c, -1));         // sibling order: C before B
	CHECK(moved());
	CHECK_FALSE(w.moveChild(c, -1));   // already first
	CHECK_FALSE(moved());
	CHECK(w.sortChildrenByName(a));    // B before C again
	CHECK(moved());
	CHECK_FALSE(w.sortChildrenByName(a));
	CHECK_FALSE(moved());

	// What it does not cover: values, and components of a live entity.
	tf(w, a).position = { 1.0f, 2.0f, 3.0f };
	MeshComponent mc;
	mc.meshAssetId = HE::kDefaultCubeMeshId;
	w.addComponent(b, mc);
	w.renameEntity(b, "B2");
	w.markHierarchyDirty();
	w.setEntityId(b, HE::UUID::generate());
	CHECK_FALSE(moved());

	w.destroyEntity(b);
	CHECK(moved());
	// An additive load builds entities, so it moves it, and so does clear().
	{
		HorizonWorld src;
		src.createEntity("X");
		const auto file = std::filesystem::temp_directory_path() / "he_epoch_scene.hescene";
		SceneSerializer ser;
		REQUIRE(ser.save(src, file, SerializeFormat::JSON));
		CHECK(ser.loadAdditive(w, file, SerializeFormat::JSON));
		he_test::removeQuiet(file);
		CHECK(moved());
	}
	w.clear();
	CHECK(moved());
}

// MUTATION: in applyAdditiveJson, drop the `rootChildren.resize(kept)` that takes the entries
// rebuildHierarchy re-parented out of the world root's list: this case, "Sliced load: the cell
// built in slices is the cell built whole" and the half-built-cell case go red.
TEST_CASE("SceneSerializer: an additive load leaves the world root listing only the load's tops")
{
	// createEntity lists every new entity under the world root, and the record links
	// only re-pointed `parent`: every entity of a loaded cell stayed in the root's list as
	// well. propagateTransforms walked it twice, and a floating-origin shift, which moves
	// every child of the root, moved it twice (once by itself, once with its cell).
	const auto root = std::filesystem::temp_directory_path() / "he_additive_root";
	he_test::removeAllQuiet(root);
	writeHouses(root / "a.hescene", 5, 3, 0.0f);

	HorizonWorld world;
	const Entity keep = world.createEntity("Already there");
	(void)keep;
	const auto rootKids = [&]() -> const std::vector<Entity>&
	{
		return world.registry().get<HierarchyComponent>(world.rootEntity()).children;
	};
	const size_t before = rootKids().size();
	std::vector<Entity> created;
	SceneSerializer ser;
	REQUIRE(ser.loadAdditiveFromJson(world, readScene(root / "a.hescene"), &created));
	CHECK(created.size() == 1u + 5u * 4u);
	CHECK(rootKids().size() == before + 1);   // the cell's root, not its twenty-odd entities
	for (const Entity e : rootKids())
		CHECK(world.registry().get<HierarchyComponent>(e).parent == world.rootEntity());

	// A second load, and then the same through destroying the first: no dead handles stay.
	std::vector<Entity> second;
	REQUIRE(ser.loadAdditiveFromJson(world, readScene(root / "a.hescene"), &second));
	CHECK(rootKids().size() == before + 2);
	Entity firstRoot = entt::null;
	for (const Entity e : created)
		if (world.registry().get<HierarchyComponent>(e).parent == world.rootEntity()) firstRoot = e;
	REQUIRE((firstRoot != entt::null));
	world.destroyEntity(firstRoot);
	CHECK(rootKids().size() == before + 1);
	for (const Entity e : rootKids()) CHECK(world.registry().valid(e));

	// The shift: every mesh keeps its absolute position. The streamer gives the root of a
	// cell the transform that puts it at -origin; this load did not go through it.
	for (const Entity e : second)
		if (world.registry().get<HierarchyComponent>(e).parent == world.rootEntity())
			tf(world, e).position = -glm::vec3(world.origin());
	HE::propagateTransforms(world);
	std::vector<std::pair<Entity, glm::dvec3>> absolute;
	for (auto [e, mc] : world.registry().view<MeshComponent>().each())
		absolute.emplace_back(e, glm::dvec3(HE::worldPositionOf(world, e)) + world.origin());
	REQUIRE(absolute.size() == 5u * 3u);   // the first cell is gone, the second is left
	HE::shiftWorldOrigin(world, nullptr, glm::vec3(1000.0f, 0.0f, 0.0f));
	HE::propagateTransforms(world);
	for (const auto& [e, was] : absolute)
	{
		// Within a float's step at a kilometre (6e-5 m): a mesh moved twice is a kilometre off.
		const glm::dvec3 now = glm::dvec3(HE::worldPositionOf(world, e)) + world.origin();
		CHECK(std::fabs(now.x - was.x) < 1e-3);
		CHECK(std::fabs(now.z - was.z) < 1e-3);
		// And the matrix the renderer reads, not only the chain walk.
		const glm::vec3 viaMatrix = glm::vec3(world.registry().get<TransformComponent>(e).worldMatrix[3]);
		CHECK(std::fabs(double(viaMatrix.x) + world.origin().x - was.x) < 1e-3);
	}
	he_test::removeAllQuiet(root);
}

TEST_CASE("SceneSerializer::sliceForAdditiveLoad: whole subtrees, in order, or the cell as it was")
{
	const auto root = std::filesystem::temp_directory_path() / "he_slice_cut";
	he_test::removeAllQuiet(root);
	writeHouses(root / "c.hescene", 40, 9, 0.0f);   // 1 + 40 * 10 = 401 records
	const nlohmann::json cell = readScene(root / "c.hescene");
	REQUIRE(cell["entities"].size() == 401u);

	// The order of the cell root's children, as the file has it.
	std::vector<nlohmann::json> rootKids;
	for (const auto& e : cell["entities"])
		if (e["parent"].is_null()) rootKids = e["children"].get<std::vector<nlohmann::json>>();
	REQUIRE(rootKids.size() == 40u);

	std::vector<nlohmann::json> slices = SceneSerializer::sliceForAdditiveLoad(nlohmann::json(cell), 128);
	// The root alone, then houses of ten records, twelve to a slice: 40 houses in four slices.
	REQUIRE(slices.size() == 5u);
	CHECK(slices[0]["entities"].size() == 1u);
	size_t total = 0;
	std::vector<nlohmann::json> topsInOrder;
	for (size_t i = 0; i < slices.size(); ++i)
	{
		const auto& ents = slices[i]["entities"];
		total += ents.size();
		if (i > 0) CHECK(ents.size() <= 128u);
		std::vector<nlohmann::json> inSlice;
		for (const auto& e : ents) inSlice.push_back(e["uuid"]);
		for (const auto& e : ents)
		{
			if (i == 0 || e["parent"] == slices[0]["entities"][0]["uuid"])
			{
				if (i > 0) topsInOrder.push_back(e["uuid"]);
				continue;
			}
			// Never half a house: a part's parent is in its own slice.
			CHECK(std::find(inSlice.begin(), inSlice.end(), e["parent"]) != inSlice.end());
		}
	}
	CHECK(total == 401u);
	CHECK(topsInOrder == rootKids);   // the sibling order survives the cut

	// A cell that is no larger than a slice is not cut, and comes back unchanged.
	std::vector<nlohmann::json> one = SceneSerializer::sliceForAdditiveLoad(nlohmann::json(cell), 401);
	REQUIRE(one.size() == 1u);
	CHECK(one[0] == cell);
	CHECK(SceneSerializer::sliceForAdditiveLoad(nlohmann::json(cell), 0).size() == 1u);

	// A subtree larger than a slice is not divided: it is a slice of its own.
	writeHouses(root / "big.hescene", 3, 200, 0.0f);   // houses of 201
	std::vector<nlohmann::json> big = SceneSerializer::sliceForAdditiveLoad(readScene(root / "big.hescene"), 128);
	REQUIRE(big.size() == 4u);   // the root, then one slice per house
	for (size_t i = 1; i < big.size(); ++i) CHECK(big[i]["entities"].size() == 201u);

	// What the whole load would do differently from the slices is not cut: a record the
	// root does not reach (the loader leaves it under the world root), a second root.
	nlohmann::json orphan = cell;
	nlohmann::json stray  = orphan["entities"][0];   // some part, or a house: any record will do
	stray["uuid"][1]      = 0xDEADBEEFull;
	orphan["entities"].push_back(stray);
	CHECK(SceneSerializer::sliceForAdditiveLoad(std::move(orphan), 128).size() == 1u);
	nlohmann::json twoRoots = cell;
	nlohmann::json root2    = twoRoots["entities"][0];
	root2["uuid"][1]        = 0xBADF00Dull;
	root2["parent"]         = nullptr;
	root2["children"]       = nlohmann::json::array();
	twoRoots["entities"].push_back(root2);
	CHECK(SceneSerializer::sliceForAdditiveLoad(std::move(twoRoots), 128).size() == 1u);
	he_test::removeAllQuiet(root);
}

TEST_CASE("Sliced load: the cell built in slices is the cell built whole")
{
	const auto root = std::filesystem::temp_directory_path() / "he_slice_equal";
	he_test::removeAllQuiet(root);
	writeHouses(root / "c.hescene", 40, 9, 0.0f);
	const nlohmann::json cell = readScene(root / "c.hescene");

	SceneSerializer::AdditiveOptions keepIds;
	keepIds.preserveIds = true;

	HorizonWorld whole;
	std::vector<Entity> wholeCreated;
	REQUIRE(SceneSerializer().loadAdditiveFromJson(whole, cell, &wholeCreated, keepIds));

	HorizonWorld cut;
	std::vector<nlohmann::json> slices = SceneSerializer::sliceForAdditiveLoad(nlohmann::json(cell), 64);
	REQUIRE(slices.size() > 2u);
	Entity cellRoot = entt::null;
	std::vector<Entity> cutCreated;
	for (size_t i = 0; i < slices.size(); ++i)
	{
		SceneSerializer::AdditiveOptions o = keepIds;
		o.attachTo = cellRoot;
		std::vector<Entity> made;
		REQUIRE(SceneSerializer().loadAdditiveFromJson(cut, slices[i], &made, o));
		if (i == 0)
		{
			REQUIRE(made.size() == 1u);
			cellRoot = made[0];
		}
		cutCreated.insert(cutCreated.end(), made.begin(), made.end());
	}

	CHECK(cutCreated.size() == wholeCreated.size());
	CHECK(entityCount(cut) == entityCount(whole));
	// The world root lists only the cell's root, both ways.
	CHECK(cut.registry().get<HierarchyComponent>(cut.rootEntity()).children.size() == 1u);
	CHECK(whole.registry().get<HierarchyComponent>(whole.rootEntity()).children.size() == 1u);

	// Entity for entity, by id: name, parent, children in order, local transform.
	size_t compared = 0;
	bool   same     = true;
	for (const Entity e : wholeCreated)
	{
		const HE::UUID id = whole.entityId(e);
		const Entity   o  = cut.findByEntityId(id);
		if (o == entt::null) { same = false; continue; }
		++compared;
		const auto& hw = whole.registry().get<HierarchyComponent>(e);
		const auto& hc = cut.registry().get<HierarchyComponent>(o);
		same = same && whole.registry().get<NameComponent>(e).name == cut.registry().get<NameComponent>(o).name;
		same = same && (hw.parent == whole.rootEntity()) == (hc.parent == cut.rootEntity());
		if (hw.parent != whole.rootEntity())
			same = same && whole.entityId(hw.parent) == cut.entityId(hc.parent);
		same = same && hw.children.size() == hc.children.size();
		for (size_t i = 0; same && i < hw.children.size(); ++i)
			same = same && whole.entityId(hw.children[i]) == cut.entityId(hc.children[i]);
		// The cell's root has no transform (the streamer gives it one), the rest do.
		const auto* tw = whole.registry().try_get<TransformComponent>(e);
		const auto* tc = cut.registry().try_get<TransformComponent>(o);
		same = same && (tw == nullptr) == (tc == nullptr);
		if (tw && tc)
			same = same && tw->position == tc->position && tw->rotation == tc->rotation && tw->scale == tc->scale;
	}
	CHECK(compared == wholeCreated.size());
	CHECK(same);
	he_test::removeAllQuiet(root);
}

TEST_CASE("CellStreamer: a cell is built in slices, one per update at no budget, and is loaded when the last is in")
{
	const auto root = std::filesystem::temp_directory_path() / "he_cell_slices";
	he_test::removeAllQuiet(root);
	writeHouses(root / "W.cells" / "cell_0_0.hescene", 40, 9, 0.0f);   // 401 entities, houses of ten
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(
		R"({"cellSize": 100, "loadRadius": 60, "unloadRadius": 120, "lookaheadSec": 0,
		    "dir": "W.cells", "list": [[0,0,401]]})", m));

	for (const size_t cap : { size_t(64), size_t(0) })
	{
		size_t sliceHooks = 0, loadedHooks = 0, inSlices = 0, inLoaded = 0;
		HE::CellStreamer::Hooks hooks;
		hooks.loadedSlice = [&](entt::entity, const std::vector<entt::entity>& created)
		{
			++sliceHooks;
			inSlices += created.size();
			CHECK_FALSE(created.empty());
		};
		hooks.loaded = [&](entt::entity, const std::vector<entt::entity>& created)
		{
			++loadedHooks;
			inLoaded = created.size();
			CHECK(sliceHooks > 0);   // after every slice, never before
		};
		HorizonWorld world;
		HE::CellStreamer s;
		s.setSliceEntities(cap);
		s.begin(m, diskReader(root), hooks);

		// Budget zero: each update builds exactly one slice (one is always built).
		size_t maxPerUpdate = 0, updates = 0;
		bool   sawBuilding = false, loadedWhileBuilding = false;
		const std::vector<HE::CellAnchor> at = { anchorAt(50.0) };
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (!s.isLoaded(0, 0) && std::chrono::steady_clock::now() < until)
		{
			const size_t before = s.stats().slicesDone;
			s.update(world, at, 0.0);
			++updates;
			maxPerUpdate = std::max(maxPerUpdate, s.stats().slicesDone - before);
			if (s.isBuilding(0, 0))
			{
				sawBuilding = true;
				loadedWhileBuilding = loadedWhileBuilding || s.isLoaded(0, 0) || loadedHooks > 0;
			}
			std::this_thread::yield();
		}
		REQUIRE(s.isLoaded(0, 0));
		CHECK(maxPerUpdate <= 1u);
		CHECK(loadedHooks == 1u);
		CHECK(inLoaded == 401u);
		CHECK(inSlices == 401u);
		CHECK(entityCount(world) == 402u);   // the 401 and the world's root
		CHECK(s.stats().failed == 0u);
		CHECK(s.stats().loaded == 1u);
		CHECK(s.stats().building == 0u);
		if (cap == 64)
		{
			// The root alone, then houses of ten, six to a slice: 40 houses in seven.
			CHECK(sawBuilding);
			CHECK_FALSE(loadedWhileBuilding);
			CHECK(sliceHooks == 8u);
			CHECK(s.stats().slicesDone == 8u);
			CHECK(s.stats().largestSlice == 60u);
		}
		else
		{
			// The control: with the slices off, one update builds the whole cell. The bound
			// the sliced run keeps is the one this breaks.
			CHECK(sliceHooks == 1u);
			CHECK(s.stats().largestSlice == 401u);
		}
		s.clear(world);
		CHECK(entityCount(world) == 1u);
	}
	he_test::removeAllQuiet(root);
}

TEST_CASE("CellStreamer: a sliced cell keeps its ids across an unload, a head-less one does not")
{
	const auto root = std::filesystem::temp_directory_path() / "he_cell_slice_ids";
	he_test::removeAllQuiet(root);
	writeHouses(root / "W.cells" / "cell_0_0.hescene", 12, 5, 0.0f, /*head=*/true);   // 73 entities
	writeHouses(root / "W.cells" / "cell_3_0.hescene", 12, 5, 300.0f, /*head=*/false);
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(
		R"({"cellSize": 100, "loadRadius": 60, "unloadRadius": 120, "lookaheadSec": 0,
		    "dir": "W.cells", "list": [[0,0,73],[3,0,73]]})", m));

	HorizonWorld world;
	HE::CellStreamer s;
	s.setSliceEntities(20);
	s.begin(m, diskReader(root), {});

	const auto idsOf = [&](int cx)
	{
		std::vector<HE::UUID> ids;
		for (auto [e, mc] : world.registry().view<MeshComponent>().each())
		{
			const double x = HE::worldPositionOf(world, e).x;
			if (x >= cx * 100.0 && x < (cx + 1) * 100.0) ids.push_back(world.entityId(e));
		}
		std::sort(ids.begin(), ids.end(), [](const HE::UUID& a, const HE::UUID& b)
		          { return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo; });
		return ids;
	};

	std::vector<HE::CellAnchor> at = { anchorAt(50.0), anchorAt(350.0) };
	pumpAnchors(s, world, at, 0.0, [&] { return s.isLoaded(0, 0) && s.isLoaded(3, 0); });
	REQUIRE(s.isLoaded(0, 0));
	REQUIRE(s.isLoaded(3, 0));
	const std::vector<HE::UUID> withHead = idsOf(0), withoutHead = idsOf(3);
	CHECK(withHead.size() == 60u);
	CHECK(withoutHead.size() == 60u);

	// Everybody leaves, both cells go; everybody comes back.
	at = { anchorAt(5000.0) };
	pumpAnchors(s, world, at, 100.0, [&] { return !s.isLoaded(0, 0) && !s.isLoaded(3, 0); });
	REQUIRE_FALSE(s.isLoaded(0, 0));
	CHECK(entityCount(world) == 1u);
	at = { anchorAt(50.0), anchorAt(350.0) };
	pumpAnchors(s, world, at, 0.0, [&] { return s.isLoaded(0, 0) && s.isLoaded(3, 0); });
	REQUIRE(s.isLoaded(3, 0));
	CHECK(idsOf(0) == withHead);        // the same entities, slice by slice
	CHECK(idsOf(3) != withoutHead);     // version 1: fresh ids, as it always was
	CHECK(s.stats().idCollisions == 0u);
	s.clear(world);
	he_test::removeAllQuiet(root);
}

// MUTATION: in nearestReach (CellStreamer.cpp), leave the loop after the first anchor, which is
// what the streamer did before the list (only the first camera counted): this case, the sliced-ids
// case and the viewpoints case go red.
TEST_CASE("CellStreamer: several anchors, a cell is wanted by any and dropped only when every anchor is beyond")
{
	const auto root = std::filesystem::temp_directory_path() / "he_cell_anchors";
	he_test::removeAllQuiet(root);
	for (int x = 0; x < 6; ++x)
		writeCell(root / "W.cells" / ("cell_" + std::to_string(x) + "_0.hescene"),
		          { { x * 100.0f + 40.0f, 0.0f, 50.0f }, { x * 100.0f + 50.0f, 0.0f, 50.0f },
		            { x * 100.0f + 60.0f, 0.0f, 50.0f } });
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(
		R"({"cellSize": 100, "loadRadius": 60, "unloadRadius": 120, "lookaheadSec": 2,
		    "dir": "W.cells", "list": [[0,0,3],[1,0,3],[2,0,3],[3,0,3],[4,0,3],[5,0,3]]})", m));

	HorizonWorld world;
	HE::CellStreamer s;
	s.begin(m, diskReader(root), {});
	const auto loadedSet = [&]
	{
		std::string set;
		for (int x = 0; x < 6; ++x) set += s.isLoaded(x, 0) ? char('0' + x) : '.';
		return set;
	};
	const auto settle = [&](const std::vector<HE::CellAnchor>& anchors, const std::string& want)
	{
		pumpAnchors(s, world, anchors, 100.0, [&] { return loadedSet() == want; });
		return loadedSet();
	};

	// Nobody to keep cells around: nothing is read, nothing built.
	s.update(world, std::vector<HE::CellAnchor>{}, 100.0);
	CHECK(s.stats().anchors == 0u);
	CHECK(s.stats().inFlight == 0u);
	CHECK(loadedSet() == "......");

	// A in cell 0, B in cell 4: the cells around each, and nothing between.
	CHECK(settle({ anchorAt(50.0), anchorAt(450.0) }, "01.345") == "01.345");
	CHECK(s.stats().anchors == 2u);
	// A leaves for good. B keeps what it holds, and what only A held goes.
	CHECK(settle({ anchorAt(-1000.0), anchorAt(450.0) }, "...345") == "...345");
	// B walks on to cell 2 and A is still away: 4 and 5 are 150 m and more behind B now.
	CHECK(settle({ anchorAt(-1000.0), anchorAt(250.0) }, ".123..") == ".123..");
	// A shows up beside cell 4 while B is away: B's cell 3 stays, held by A alone now.
	CHECK(settle({ anchorAt(390.0), anchorAt(-1000.0) }, "..234.") == "..234.");
	// The lookahead counts per anchor: A at cell 0 moving +x at 100 m/s is, in two seconds, at
	// x = 250, so cells 2 and 3 are wanted as well as 0 and 1.
	HE::CellAnchor runner = anchorAt(50.0);
	runner.velocity = { 100.0f, 0.0f, 0.0f };
	CHECK(settle({ runner, anchorAt(-1000.0) }, "0123..") == "0123..");

	// Radius scale: an anchor that reaches three times as far loads cells three times as far out
	// (cell 2 is 150 m from x = 50: 50 m in its units), and one that reaches half as far, half.
	s.clear(world);
	s.begin(m, diskReader(root), {});
	CHECK(settle({ anchorAt(50.0, 3.0f) }, "012...") == "012...");
	// Shrunk back to 1, the same anchor keeps cell 1 (50 m) but cell 2 (150 m) is beyond 120.
	CHECK(settle({ anchorAt(50.0, 1.0f) }, "01....") == "01....");
	s.clear(world);
	he_test::removeAllQuiet(root);
}

TEST_CASE("CellStreamer: a cell half built when its anchor leaves is taken out again, bodies hook included")
{
	const auto root = std::filesystem::temp_directory_path() / "he_cell_abandon";
	he_test::removeAllQuiet(root);
	writeHouses(root / "W.cells" / "cell_0_0.hescene", 40, 9, 0.0f);
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(
		R"({"cellSize": 100, "loadRadius": 60, "unloadRadius": 120, "lookaheadSec": 0,
		    "dir": "W.cells", "list": [[0,0,401]]})", m));

	size_t sliceHooks = 0, loadedHooks = 0, unloadingHooks = 0;
	HE::CellStreamer::Hooks hooks;
	hooks.loadedSlice = [&](entt::entity, const std::vector<entt::entity>&) { ++sliceHooks; };
	hooks.loaded      = [&](entt::entity, const std::vector<entt::entity>&) { ++loadedHooks; };
	hooks.unloading   = [&](entt::entity) { ++unloadingHooks; };
	HorizonWorld world;
	HE::CellStreamer s;
	s.setSliceEntities(64);
	s.begin(m, diskReader(root), hooks);

	const std::vector<HE::CellAnchor> near = { anchorAt(50.0) };
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (sliceHooks < 3 && std::chrono::steady_clock::now() < until)
	{
		s.update(world, near, 0.0);
		std::this_thread::yield();
	}
	REQUIRE(sliceHooks >= 3u);
	REQUIRE(s.isBuilding(0, 0));
	CHECK_FALSE(s.isLoaded(0, 0));
	CHECK(entityCount(world) > 1u);

	// Away: the half cell is dropped as a loaded one would be.
	s.update(world, std::vector<HE::CellAnchor>{ anchorAt(5000.0) }, 0.0);
	CHECK_FALSE(s.isBuilding(0, 0));
	CHECK_FALSE(s.isLoaded(0, 0));
	CHECK(s.stats().abandoned == 1u);
	CHECK(s.stats().building == 0u);
	CHECK(unloadingHooks == 1u);   // the teardown a built cell gets (its bodies, its asset loads)
	CHECK(loadedHooks == 0u);
	CHECK(entityCount(world) == 1u);
	CHECK(world.registry().get<HierarchyComponent>(world.rootEntity()).children.empty());

	// And back: it builds again, completely.
	pumpAnchors(s, world, near, 100.0, [&] { return s.isLoaded(0, 0); });
	CHECK(s.isLoaded(0, 0));
	CHECK(entityCount(world) == 402u);
	CHECK(loadedHooks == 1u);

	// Cleared (a level change) while half built: the same teardown, every slice's entities gone.
	s.clear(world);
	CHECK(entityCount(world) == 1u);
	sliceHooks = 0;
	s.begin(m, diskReader(root), hooks);
	const auto again = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (sliceHooks < 2 && std::chrono::steady_clock::now() < again)
	{
		s.update(world, near, 0.0);
		std::this_thread::yield();
	}
	REQUIRE(sliceHooks >= 2u);
	CHECK(s.isBuilding(0, 0));
	const size_t unloadingBefore = unloadingHooks;
	s.clear(world);
	CHECK(unloadingHooks == unloadingBefore + 1);
	CHECK(entityCount(world) == 1u);
	he_test::removeAllQuiet(root);
}

TEST_CASE("CellStreamer::isSettled: true once every cell within the radius is built or has failed for good")
{
	const auto root = std::filesystem::temp_directory_path() / "he_cell_settled";
	he_test::removeAllQuiet(root);
	writeHouses(root / "W.cells" / "cell_0_0.hescene", 10, 3, 0.0f);
	writeHouses(root / "W.cells" / "cell_1_0.hescene", 10, 3, 100.0f);
	// Cell 2 is in the list and has no file.
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(
		R"({"cellSize": 100, "loadRadius": 60, "unloadRadius": 120, "lookaheadSec": 0,
		    "dir": "W.cells", "list": [[0,0,41],[1,0,41],[2,0,41]]})", m));
	HorizonWorld world;
	HE::CellStreamer s;
	s.setSliceEntities(10);
	CHECK(s.isSettled({ 50.0, 0.0, 50.0 }, 60.0));   // inactive: nothing to wait for
	s.begin(m, diskReader(root), {});
	CHECK_FALSE(s.isSettled({ 50.0, 0.0, 50.0 }, 60.0));   // cells 0 and 1 are not there yet
	CHECK(s.isSettled({ 5000.0, 0.0, 50.0 }, 60.0));       // no cell near: nothing to wait for

	const std::vector<HE::CellAnchor> at = { anchorAt(150.0) };   // cells 0 (50 m), 1 and 2 (50 m)
	// Half built is not settled.
	bool sawHalfBuilt = false;
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (std::chrono::steady_clock::now() < until)
	{
		s.update(world, at, 0.0);
		if (s.stats().building > 0 || (s.isLoaded(0, 0) && !s.isLoaded(1, 0)))
		{
			sawHalfBuilt = true;
			CHECK_FALSE(s.isSettled({ 150.0, 0.0, 50.0 }, 60.0));
		}
		if (s.isSettled({ 150.0, 0.0, 50.0 }, 60.0) && s.stats().inFlight == 0 && s.stats().ready == 0
		    && s.stats().building == 0)
			break;
		std::this_thread::yield();
	}
	CHECK(sawHalfBuilt);
	CHECK(s.isLoaded(0, 0));
	CHECK(s.isLoaded(1, 0));
	CHECK_FALSE(s.isLoaded(2, 0));
	CHECK(s.stats().failed == 1u);   // cell 2, which has no file, will never come
	CHECK(s.isSettled({ 150.0, 0.0, 50.0 }, 60.0));
	s.clear(world);
	he_test::removeAllQuiet(root);
}

TEST_CASE("CellManifest::around with several viewpoints: each cell by its nearest")
{
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(
		R"({"cellSize": 100, "loadRadius": 60, "unloadRadius": 120, "dir": "d",
		    "list": [[0, 0, 5], [1, 0, 6], [2, 0, 7], [3, 0, 8], [4, 0, 300]]})", m));
	// One at x = 50 (cell 0) and one at x = 450 (cell 4).
	const std::vector<HE::CellAnchor> two = { anchorAt(50.0), anchorAt(450.0) };
	const std::vector<HE::CellManifest::View> v = m.around(two, 1000.0);
	REQUIRE(v.size() == 5u);
	const auto reachOf = [&](int x)
	{
		for (const auto& c : v) if (c.x == x) return c.reach;
		return HE::CellManifest::View::Reach::Out;
	};
	CHECK(reachOf(0) == HE::CellManifest::View::Reach::Load);
	CHECK(reachOf(1) == HE::CellManifest::View::Reach::Load);
	CHECK(reachOf(2) == HE::CellManifest::View::Reach::Out);   // 150 m from both
	CHECK(reachOf(3) == HE::CellManifest::View::Reach::Load);
	CHECK(reachOf(4) == HE::CellManifest::View::Reach::Load);
	// The distance the cell counts at is the nearest anchor's, and a scale divides it.
	CHECK(m.distanceTo(two, 3, 0) == doctest::Approx(50.0));
	CHECK(m.distanceTo(std::vector<HE::CellAnchor>{ anchorAt(50.0, 2.0f) }, 3, 0) == doctest::Approx(125.0));   // 250 m / 2
	CHECK(m.distanceTo(std::vector<HE::CellAnchor>{}, 3, 0) == std::numeric_limits<double>::infinity());
	// The same single viewpoint through the old call.
	const auto single = m.around(glm::dvec3(50.0, 0.0, 50.0), 200.0);
	REQUIRE(single.size() >= 2u);
	CHECK(single[0].x == 0);
	// How many slices the game builds a cell in: one up to a slice's size, else the root's and the rest.
	for (const auto& c : v)
	{
		if (c.x == 4) CHECK(c.slicesEstimate == 1u + (300u + 127u) / 128u);
		else          CHECK(c.slicesEstimate == 1u);
	}
}

// A measurement, not a check (does not run in CI): the reference world loaded
// whole against its base plus the cells around the camera. Needs the scene and
// its split on disk:
//   python3 scripts/perf/gen_reference_world.py --count 200000 --extent 8000 --groups 2000 \
//       --lights 64 --template docs/perf-audit/scenes/landscape_noclouds.hescene --out /tmp/ref.hescene
//   mkdir -p /tmp/cellproj/Content && touch /tmp/cellproj/P.heproj
//   python3 scripts/split_scene_cells.py /tmp/ref.hescene --out /tmp/cellproj/Content/World.hescene
//   HE_CELL_BENCH_WHOLE=/tmp/ref.hescene HE_CELL_BENCH_PROJECT=/tmp/cellproj \
//       out/build/release/tests/he_tests --no-skip --test-case='Cell streaming bench*'
// HE_CELL_BENCH_SLICE=0 (entities per slice) builds each cell whole, the way it was built
// before the slices; unset is the default of 128. The line to read is "worst main-thread frame".
TEST_CASE("Cell streaming bench: whole reference world against base plus nearby cells" * doctest::skip())
{
	const char* wholeEnv   = std::getenv("HE_CELL_BENCH_WHOLE");
	const char* projectEnv = std::getenv("HE_CELL_BENCH_PROJECT");
	REQUIRE_MESSAGE((wholeEnv && projectEnv), "set HE_CELL_BENCH_WHOLE and HE_CELL_BENCH_PROJECT");
	const std::filesystem::path project = projectEnv;
	using Clock = std::chrono::steady_clock;
	const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
	const glm::dvec3 camera(0.0, 25.0, 90.0);   // the ladder's --cam

	for (int run = 0; run < 3; ++run)
	{
		SceneSerializer ser;
		size_t wholeEntities = 0;
		Clock::duration wholeTime{};
		{
			HorizonWorld whole;
			const Clock::time_point t0 = Clock::now();
			REQUIRE(ser.load(whole, wholeEnv, SerializeFormat::JSON));
			wholeTime     = Clock::now() - t0;
			wholeEntities = whole.registry().storage<entt::entity>().in_use();
		}

		HorizonWorld world;
		const Clock::time_point t0 = Clock::now();
		REQUIRE(ser.load(world, project / "Content/World.hescene", SerializeFormat::JSON));
		const Clock::time_point t1 = Clock::now();
		HE::CellManifest m;
		REQUIRE(HE::CellManifest::parse(world.cellManifestJson(), m));
		HE::CellStreamer s;
		// HE_CELL_BENCH_SLICE: entities per slice; 0 builds every cell whole, as before the slices.
		if (const char* slice = std::getenv("HE_CELL_BENCH_SLICE"))
			s.setSliceEntities(static_cast<size_t>(std::strtoull(slice, nullptr, 10)));
		s.begin(m, [&project](const std::string& path) -> std::function<bool(std::vector<uint8_t>&)>
		{
			const auto file = project / path;
			return [file](std::vector<uint8_t>& out)
			{
				std::ifstream in(file, std::ios::binary);
				out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
				return !out.empty();
			};
		}, {});
		// Until every cell the camera wants is built; frames of 4 ms budget, like the game.
		size_t frames = 0;
		double worstFrameMs = 0.0;
		// What the worst frame did: slices it built, entities it made. A slow frame that built
		// nothing is not the build's doing (the main thread waiting for a core, a lock).
		size_t worstSlices = 0, worstEntities = 0, worstFrame = 0;
		// The main thread's own CPU time over the same frame: a frame that took 90 ms of wall
		// clock and 5 of CPU was waiting (for a core, a lock, a page), not building.
		const auto threadCpuMs = []
		{
#if defined(_WIN32)
			return 0.0;   // the bench is read on the Mac; no per-thread clock worth porting
#else
			timespec ts{};
			clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
			return static_cast<double>(ts.tv_sec) * 1e3 + static_cast<double>(ts.tv_nsec) * 1e-6;
#endif
		};
		double worstCpuMs = 0.0;
		for (;;)
		{
			const size_t slicesBefore = s.stats().slicesDone;
			const size_t entsBefore   = world.registry().storage<entt::entity>().in_use();
			const double cpu0 = threadCpuMs();
			const Clock::time_point f0 = Clock::now();
			s.update(world, camera, glm::vec3(0.0f), 4.0);
			const double frameMs = ms(Clock::now() - f0);
			if (frameMs > worstFrameMs)
			{
				worstFrameMs  = frameMs;
				worstCpuMs    = threadCpuMs() - cpu0;
				worstFrame    = frames;
				worstSlices   = s.stats().slicesDone - slicesBefore;
				worstEntities = world.registry().storage<entt::entity>().in_use() - entsBefore;
			}
			++frames;
			if (s.stats().loaded > 0 && s.stats().inFlight == 0 && s.stats().ready == 0
			    && s.stats().building == 0) break;
			std::this_thread::yield();
		}
		const Clock::time_point t2 = Clock::now();
		MESSAGE("run " << run << ": whole " << wholeEntities << " entities in " << ms(wholeTime)
		        << " ms | base " << ms(t1 - t0) << " ms + " << s.stats().loaded << " of "
		        << m.cells.size() << " cells in " << ms(t2 - t1) << " ms (" << frames
		        << " frames, worst main-thread frame " << worstFrameMs << " ms (" << worstCpuMs
		        << " ms of thread CPU) = frame " << worstFrame
		        << ", which built " << worstSlices << " slices / " << worstEntities << " entities; "
		        << s.stats().slicesDone << " slices of " << s.sliceEntities() << " entities, the largest "
		        << s.stats().largestSlice << "), "
		        << world.registry().storage<entt::entity>().in_use() << " entities");
		s.clear(world);
	}
}

// A measurement, not a check (does not run in CI), Thema 164 step 5: what
// RenderExtractor::extract costs on the reference world loaded whole, against the
// same world split by HE::splitSceneIntoCells and streamed around the camera; on
// the streamed world also a walk across the map at a given speed. One mode per
// process, so that the peak resident size of the process belongs to one of them.
//   python3 scripts/perf/gen_reference_world.py --count 100000 --extent 8000 --groups 1000 \
//       --lights 64 --template docs/perf-audit/scenes/landscape_noclouds.hescene --out /tmp/ref.hescene
//   HE_CELL_BENCH_WHOLE=/tmp/ref.hescene HE_CELL_BENCH_MODE=whole|streamed \
//   [HE_CELL_BENCH_CELL=512] [HE_CELL_BENCH_LOAD=768] \
//   [HE_CELL_BENCH_WALK=<metres>] [HE_CELL_BENCH_SPEED=<m/s>] [HE_CELL_BENCH_DIR=<keep dir>] \
//       out/build/release/tests/he_tests --no-skip --test-case='Cell streaming bench: extract*'
// Neither physics bodies nor assets are in the picture: the reference world has none of
// its own (built-in cube and sphere only), so the cells' hooks are empty.
TEST_CASE("Cell streaming bench: extract on the whole world against the streamed cells around the camera" * doctest::skip())
{
	using Clock = std::chrono::steady_clock;
	const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
	const auto envNum = [](const char* key, double fallback)
	{
		const char* v = std::getenv(key);
		return (v && *v) ? std::atof(v) : fallback;
	};
	const auto peakRssMb = []
	{
#if defined(_WIN32)
		return 0.0;
#else
		rusage ru{};
		getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
		return static_cast<double>(ru.ru_maxrss) / (1024.0 * 1024.0);   // bytes there
#else
		return static_cast<double>(ru.ru_maxrss) / 1024.0;              // kilobytes there
#endif
#endif
	};
	const auto threadCpuMs = []
	{
#if defined(_WIN32)
		return 0.0;
#else
		timespec ts{};
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
		return static_cast<double>(ts.tv_sec) * 1e3 + static_cast<double>(ts.tv_nsec) * 1e-6;
#endif
	};

	const char* wholeEnv = std::getenv("HE_CELL_BENCH_WHOLE");
	REQUIRE_MESSAGE(wholeEnv, "set HE_CELL_BENCH_WHOLE and HE_CELL_BENCH_MODE=whole|streamed");
	const std::string mode = std::getenv("HE_CELL_BENCH_MODE") ? std::getenv("HE_CELL_BENCH_MODE") : "streamed";
	REQUIRE((mode == "whole" || mode == "streamed"));
	// HE_CELL_BENCH_DIR keeps the split (Bench.hescene + Bench.cells/) for the next run, which then
	// loads the base and streams the cells without reading or splitting the big scene: the process
	// holds nothing but the streamed world, so its peak resident size is the streaming's.
	const char* dirEnv = std::getenv("HE_CELL_BENCH_DIR");
	const bool keep = dirEnv && *dirEnv;
	const std::filesystem::path root = keep ? std::filesystem::path(dirEnv)
	                                        : std::filesystem::temp_directory_path() / "he_cell_bench_extract";
	const bool presplit = keep && mode == "streamed" && std::filesystem::exists(root / "Bench.hescene");

	HorizonWorld world;
	SceneSerializer ser;
	HE::CellStreamer s;
	HE::CellManifest m;
	glm::dvec3 camera(0.0, 25.0, 90.0);   // the ladder's --cam

	const auto settle = [&](const glm::dvec3& at, const glm::vec3& vel)
	{
		size_t frames = 0;
		double worst = 0.0;
		for (;;)
		{
			const Clock::time_point f0 = Clock::now();
			s.update(world, at, vel, 4.0);
			worst = std::max(worst, ms(Clock::now() - f0));
			++frames;
			if (s.stats().loaded > 0 && s.stats().inFlight == 0 && s.stats().ready == 0 && s.stats().building == 0) break;
			std::this_thread::yield();
		}
		MESSAGE("settled in " << frames << " frames, worst update() " << worst << " ms, "
		        << s.stats().loaded << " of " << m.cells.size() << " cells built");
	};

	const auto beginStreaming = [&]
	{
		s.begin(m, [&root](const std::string& path) -> std::function<bool(std::vector<uint8_t>&)>
		{
			const auto file = root / path;
			return [file](std::vector<uint8_t>& out)
			{
				std::ifstream in(file, std::ios::binary);
				out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
				return !out.empty();
			};
		}, {});
	};

	if (mode == "whole")
	{
		const Clock::time_point t0 = Clock::now();
		REQUIRE(ser.load(world, wholeEnv, SerializeFormat::JSON));
		MESSAGE("whole: loaded in " << ms(Clock::now() - t0) << " ms");
	}
	else if (presplit)
	{
		const Clock::time_point t0 = Clock::now();
		REQUIRE(ser.load(world, root / "Bench.hescene", SerializeFormat::JSON));
		REQUIRE(HE::CellManifest::parse(world.cellManifestJson(), m));
		MESSAGE("streamed: split from " << root.string() << ", " << m.cells.size() << " cells (cell " << m.cellSize
		        << " m, load " << m.loadRadius << " m, unload " << m.unloadRadius << " m), base loaded in "
		        << ms(Clock::now() - t0) << " ms, base " << world.registry().storage<entt::entity>().in_use()
		        << " entities");
		beginStreaming();
		settle(camera, glm::vec3(0.0f));
	}
	else
	{
		std::ifstream in(wholeEnv, std::ios::binary);
		const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		const nlohmann::json scene = HE::parseSceneText(text);
		REQUIRE_FALSE(scene.is_discarded());
		HE::CellSplitOptions opt;
		opt.cellSize   = static_cast<float>(envNum("HE_CELL_BENCH_CELL", 512.0));
		opt.loadRadius = static_cast<float>(envNum("HE_CELL_BENCH_LOAD", 0.0));
		opt.dir        = "Bench.cells";
		const Clock::time_point t0 = Clock::now();
		const HE::CellSplitResult split = HE::splitSceneIntoCells(scene, opt);
		REQUIRE_MESSAGE(split.error.empty(), split.error);
		const Clock::time_point t1 = Clock::now();
		REQUIRE(HE::CellManifest::parse(split.base["cells"].dump(), m));
		he_test::removeAllQuiet(keep ? root / opt.dir : root);
		std::filesystem::create_directories(root / opt.dir);
		if (keep)
		{
			std::ofstream out(root / "Bench.hescene", std::ios::binary);
			out << split.base.dump();
			REQUIRE(out.good());
		}
		for (const HE::CellSplitResult::Cell& c : split.cells)
		{
			std::ofstream out(root / m.cellPath(c.x, c.z), std::ios::binary);
			out << c.scene.dump();
			REQUIRE(out.good());
		}
		const Clock::time_point t2 = Clock::now();
		REQUIRE(ser.loadFromMemory(world, nlohmann::json::to_cbor(split.base)));
		MESSAGE("streamed: split " << ms(t1 - t0) << " ms into " << split.cells.size() << " cells ("
		        << split.moved << " entities moved, " << split.keptForRefs << " kept for refs, "
		        << split.clusters << " clusters; cell " << m.cellSize << " m, load " << m.loadRadius
		        << " m, unload " << m.unloadRadius << " m), written in " << ms(t2 - t1)
		        << " ms, base loaded in " << ms(Clock::now() - t2) << " ms, base " << world.registry().storage<entt::entity>().in_use()
		        << " entities");
		beginStreaming();
		settle(camera, glm::vec3(0.0f));
	}
	const size_t entities = world.registry().storage<entt::entity>().in_use();
	MESSAGE(mode << ": " << entities << " entities in the world, peak RSS so far " << peakRssMb() << " MB");

	// RenderExtractor::extract, a full walk every time (no FrameScope), the way the profiler's
	// scope of that name is entered. The first call also propagates the transforms nobody has.
	RenderExtractor ex;
	configure(ex);
	RenderWorld rw;
	EditorCameraOverride cam;
	const auto lookFrom = [&](const glm::dvec3& at)
	{
		const glm::vec3 eye(at);
		cam.active   = true;
		cam.position = eye;
		cam.view     = glm::lookAt(eye, eye + glm::vec3(0.0f, -0.25f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	};
	const auto measureExtract = [&](const std::string& label)
	{
		lookFrom(camera);
		const Clock::time_point c0 = Clock::now();
		ex.extract(world, rw, 16.0f / 9.0f, &cam);
		const double cold = ms(Clock::now() - c0);
		std::vector<double> t;
		for (int i = 0; i < 60; ++i)
		{
			const Clock::time_point e0 = Clock::now();
			ex.extract(world, rw, 16.0f / 9.0f, &cam);
			t.push_back(ms(Clock::now() - e0));
		}
		std::sort(t.begin(), t.end());
		MESSAGE("[" << label << "] extract p50 " << t[t.size() / 2] << " ms, p90 " << t[t.size() * 9 / 10]
		        << " ms, min " << t.front() << " ms, max " << t.back() << " ms (first call " << cold
		        << " ms), " << rw.objects.size() << " objects drawn of " << entities << " entities, peak RSS "
		        << peakRssMb() << " MB");
	};
	measureExtract(mode);

	const double walk = envNum("HE_CELL_BENCH_WALK", 0.0);
	if (mode == "streamed" && walk > 0.0)
	{
		const double speed = envNum("HE_CELL_BENCH_SPEED", 50.0);
		std::set<std::pair<int, int>> inManifest;
		for (const auto& c : m.cells) inManifest.insert({ c.x, c.z });
		camera = glm::dvec3(0.0, 25.0, -walk * 0.5);
		settle(camera, glm::vec3(0.0f));
		// 60 Hz frames, the camera moving by what the clock says it moved. A frame counts as "ground
		// missing" when a cell of the manifest within 150 m of the camera is not built, "own cell
		// missing" when the one it stands in is not.
		const double groundRadius = 150.0;
		size_t frames = 0, groundMissing = 0, ownMissing = 0, run = 0, longestRun = 0;
		size_t minEntities = SIZE_MAX, maxEntities = 0, minCells = SIZE_MAX, maxCells = 0;
		double worstUpdate = 0.0, worstCpu = 0.0, sumUpdate = 0.0;
		std::vector<double> updates;
		const size_t loadsBefore = s.stats().loadsDone, unloadsBefore = s.stats().unloads;
		const Clock::time_point start = Clock::now();
		Clock::time_point last = start;
		while (camera.z < walk * 0.5)
		{
			const Clock::time_point f0 = Clock::now();
			const double dt = std::chrono::duration<double>(f0 - last).count();
			last = f0;
			camera.z += speed * dt;
			const double cpu0 = threadCpuMs();
			s.update(world, camera, glm::vec3(0.0f, 0.0f, static_cast<float>(speed)), 4.0);
			const double u = ms(Clock::now() - f0);
			const double cpu = threadCpuMs() - cpu0;
			updates.push_back(u);
			sumUpdate += u;
			if (u > worstUpdate) { worstUpdate = u; worstCpu = cpu; }
			++frames;
			bool ground = false;
			for (const auto& c : m.cells)
				if (!s.isLoaded(c.x, c.z) && m.distanceTo(camera, c.x, c.z) <= groundRadius) { ground = true; break; }
			const int cx = HE::CellManifest::cellIndex(camera.x, m.cellSize), cz = HE::CellManifest::cellIndex(camera.z, m.cellSize);
			if (inManifest.count({ cx, cz }) && !s.isLoaded(cx, cz)) ++ownMissing;
			if (ground) { ++groundMissing; ++run; longestRun = std::max(longestRun, run); }
			else run = 0;
			const size_t n = world.registry().storage<entt::entity>().in_use();
			minEntities = std::min(minEntities, n);
			maxEntities = std::max(maxEntities, n);
			minCells    = std::min(minCells, s.stats().loaded);
			maxCells    = std::max(maxCells, s.stats().loaded);
			while (Clock::now() - f0 < std::chrono::microseconds(16667)) std::this_thread::yield();
		}
		std::sort(updates.begin(), updates.end());
		MESSAGE("walk " << walk << " m at " << speed << " m/s: " << frames << " frames in "
		        << ms(Clock::now() - start) / 1000.0 << " s, " << s.stats().loadsDone - loadsBefore << " cells loaded and "
		        << s.stats().unloads - unloadsBefore << " unloaded, loaded cells " << minCells << ".." << maxCells
		        << ", entities " << minEntities << ".." << maxEntities << ", update() p50 " << updates[updates.size() / 2]
		        << " ms p99 " << updates[updates.size() * 99 / 100] << " ms worst " << worstUpdate << " ms ("
		        << worstCpu << " ms of thread CPU), ground within " << groundRadius << " m missing in " << groundMissing
		        << " frames (longest run " << longestRun << " frames), own cell missing in " << ownMissing
		        << " frames, failed " << s.stats().failed << ", peak RSS " << peakRssMb() << " MB");
		measureExtract("streamed at the end of the walk");
	}
	if (mode == "streamed") s.clear(world);
	if (!keep) he_test::removeAllQuiet(root);
}

// A measurement, not a check (does not run in CI): one physics step with a few
// thousand dynamic bodies in contact, the case Jolt's job system parallelises.
//   out/build/release/tests/he_tests --no-skip --test-case='Physics step bench*'
TEST_CASE("Physics step bench: 4000 dynamic boxes settling on a floor" * doctest::skip())
{
	HorizonWorld world;
	const Entity floor = world.createEntity("Floor");
	tf(world, floor).scale = glm::vec3(400.0f, 1.0f, 400.0f);
	RigidBodyComponent frb; frb.type = RigidBodyType::Static;
	world.addComponent(floor, frb);
	for (int i = 0; i < 4000; ++i)
	{
		const Entity e = world.createEntity("Box");
		tf(world, e).position = glm::vec3(static_cast<float>(i % 40) * 1.2f - 24.0f,
		                                  2.0f + static_cast<float>(i / 1600) * 1.1f,
		                                  static_cast<float>((i / 40) % 40) * 1.2f - 24.0f);
		RigidBodyComponent rb; rb.type = RigidBodyType::Dynamic; rb.mass = 1.0f;
		world.addComponent(e, rb);
	}
	PhysicsWorld phys;
	phys.initialize(world);
	// From the drop on: falling, landing and piling up, before anything sleeps.
	std::vector<double> steps;
	for (int i = 0; i < 180; ++i)
	{
		const auto t0 = std::chrono::steady_clock::now();
		phys.step(world, 1.0f / 60.0f);
		steps.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
	}
	std::sort(steps.begin(), steps.end());
	MESSAGE("4000 bodies: step p50 " << steps[steps.size() / 2] << " ms, p90 " << steps[steps.size() * 9 / 10]
	        << " ms, max " << steps.back() << " ms");
}

TEST_CASE("CellStreamer: a cell's static bodies stand where the cell is, under a floating origin")
{
	// What GameApplication's loaded hook does: a body for every entity the cell
	// brought, after the cell's root has been put at -origin. The body pose must
	// come through the parent chain, not from a world matrix nothing has
	// propagated yet.
	const auto root = std::filesystem::temp_directory_path() / "he_cell_bodies";
	he_test::removeAllQuiet(root);
	{
		HorizonWorld cell;
		const Entity box = cell.createEntity("Crate");
		tf(cell, box).position = glm::vec3(250.0f, 0.0f, 50.0f);   // absolute
		tf(cell, box).scale    = glm::vec3(2.0f);
		RigidBodyComponent rb; rb.type = RigidBodyType::Static;
		cell.addComponent(box, rb);
		std::filesystem::create_directories(root / "W.cells");
		SceneSerializer ser;
		REQUIRE(ser.save(cell, root / "W.cells" / "cell_2_0.hescene", SerializeFormat::JSON));
	}
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(
		R"({"cellSize": 100, "loadRadius": 60, "dir": "W.cells", "list": [[2,0,1]]})", m));

	HorizonWorld world;
	world.setOrigin({ 200.0, 0.0, 0.0 });
	PhysicsWorld phys;
	phys.initialize(world);
	HE::CellStreamer::Hooks hooks;
	hooks.loaded = [&](entt::entity, const std::vector<entt::entity>& created)
	{
		for (entt::entity e : created) phys.addEntity(world, static_cast<uint32_t>(e));
	};
	HE::CellStreamer s;
	s.begin(m, [&root](const std::string& path) -> std::function<bool(std::vector<uint8_t>&)>
	{
		const auto file = root / path;
		return [file](std::vector<uint8_t>& out)
		{
			std::ifstream in(file, std::ios::binary);
			out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			return !out.empty();
		};
	}, hooks);
	// The hold's test is asked in world positions (relative to the origin): local x 50 is absolute
	// 250, in cell (2,0), which is not built yet; local x -150 is absolute 50, where the manifest has
	// no cell at all.
	CHECK(HE::cellHolds(s, world, { 50.0f, 0.0f, 50.0f }));
	CHECK_FALSE(HE::cellHolds(s, world, { -150.0f, 0.0f, 50.0f }));
	pumpCells(s, world, { 250.0, 1.7, 50.0 }, glm::vec3(0.0f), [&] { return s.isLoaded(2, 0); });
	REQUIRE(s.isLoaded(2, 0));
	CHECK_FALSE(HE::cellHolds(s, world, { 50.0f, 0.0f, 50.0f }));   // built now

	// Local x 50 is absolute 250: the ray straight down meets the crate's top.
	const PhysicsWorld::RaycastHit hit = phys.raycast({ 50.0f, 10.0f, 50.0f }, { 0.0f, -1.0f, 0.0f }, 20.0f);
	REQUIRE(hit.hit);
	CHECK(hit.point.x == doctest::Approx(50.0f));
	CHECK(hit.point.y == doctest::Approx(1.0f).epsilon(0.05));   // half of the 2 m box
	// Where the absolute number would put it if the origin were ignored: nothing.
	CHECK_FALSE(phys.raycast({ 250.0f, 10.0f, 50.0f }, { 0.0f, -1.0f, 0.0f }, 20.0f).hit);
	s.clear(world);
	he_test::removeAllQuiet(root);
}

// ─── Cells and the physics world (Thema 164, step 3a) ─────────────────────────

namespace
{
// Updates until `done` or twenty seconds; unlike pumpAnchors it does not insist that nothing is
// left waiting, which is the very thing a cell put off for lack of room is.
template <class Done>
void pumpUntil(HE::CellStreamer& s, HorizonWorld& world, const std::vector<HE::CellAnchor>& anchors, Done done)
{
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (std::chrono::steady_clock::now() < until && !done())
	{
		s.update(world, anchors, 100.0);
		std::this_thread::yield();
	}
}

// A cell with a floor: a static box over the 100 m square that starts at x0, its top face at
// y = 0, and `boxes` more static boxes on it. Written with the version-2 head, so that the
// streamer keeps the ids. Returns the floor's id.
HE::UUID writePhysicsCell(const std::filesystem::path& file, float x0, int boxes = 0)
{
	HorizonWorld cell;
	const Entity floor = cell.createEntity("Floor");
	tf(cell, floor).position = { x0 + 50.0f, -0.5f, 50.0f };
	tf(cell, floor).scale    = { 100.0f, 1.0f, 100.0f };
	RigidBodyComponent frb;
	frb.type = RigidBodyType::Static;
	cell.addComponent(floor, frb);
	for (int i = 0; i < boxes; ++i)
	{
		const Entity b = cell.createEntity("Box " + std::to_string(i));
		tf(cell, b).position = { x0 + 2.0f + static_cast<float>(i % 40) * 2.4f, 0.5f,
		                         2.0f + static_cast<float>(i / 40) * 2.4f };
		RigidBodyComponent rb;
		rb.type = RigidBodyType::Static;
		cell.addComponent(b, rb);
	}
	std::filesystem::create_directories(file.parent_path());
	SceneSerializer ser;
	REQUIRE(ser.save(cell, file, SerializeFormat::JSON));
	nlohmann::json j = readScene(file);
	j["streaming"]   = { { "version", 2 } };
	std::ofstream out(file, std::ios::trunc);
	out << j.dump();
	return cell.entityId(floor);
}

// A cell with a frame in it: one static box, 10 m up. Returns its id.
HE::UUID writeFrameCell(const std::filesystem::path& file, float x0)
{
	HorizonWorld cell;
	const Entity frame = cell.createEntity("Frame");
	tf(cell, frame).position = { x0 + 50.0f, 10.0f, 50.0f };
	RigidBodyComponent rb;
	rb.type = RigidBodyType::Static;
	cell.addComponent(frame, rb);
	std::filesystem::create_directories(file.parent_path());
	SceneSerializer ser;
	REQUIRE(ser.save(cell, file, SerializeFormat::JSON));
	nlohmann::json j = readScene(file);
	j["streaming"]   = { { "version", 2 } };
	std::ofstream out(file, std::ios::trunc);
	out << j.dump();
	return cell.entityId(frame);
}

// What GameApplication does with a streamer and a physics world, through the same calls
// (CellPhysics.h): a slice's bodies in as one charge, a cell's out before it goes with its
// joints kept, the reserve, and the hold. `hold` and `requeue` are the switches the control
// runs turn off; `fits` replaces the reserve.
struct PhysicsCells
{
	HorizonWorld     world;
	PhysicsWorld     phys;
	HE::CellStreamer streamer;
	bool             hold    = true;
	bool             requeue = true;
	std::function<bool(uint32_t)> fits;

	void begin(const HE::CellManifest& m, const std::filesystem::path& root, size_t sliceEntities = 128)
	{
		HE::CellStreamer::Hooks hooks;
		hooks.loadedSlice = [this](entt::entity, const std::vector<entt::entity>& created)
		{
			HE::cellSliceBodies(phys, world, created);
		};
		hooks.unloading = [this](entt::entity r)
		{
			if (requeue) HE::cellUnloadBodies(phys, world, r);
			else         phys.removeEntityTree(world, static_cast<uint32_t>(r), /*requeueJoints=*/false);
		};
		hooks.bodiesFit = [this](uint32_t n) { return fits ? fits(n) : HE::cellBodiesFit(phys, n); };
		phys.setRegionHold([this](const glm::vec3& p) { return hold && HE::cellHolds(streamer, world, p); });
		streamer.setSliceEntities(sliceEntities);
		streamer.begin(m, diskReader(root), std::move(hooks));
	}
	void step(int n)
	{
		for (int i = 0; i < n; ++i) phys.step(world, 1.0f / 60.0f);
	}
	~PhysicsCells() { streamer.clear(world); }
};

Entity makeCrate(HorizonWorld& w, const char* name, const glm::vec3& at)
{
	const Entity e = w.createEntity(name);
	tf(w, e).position = at;
	RigidBodyComponent rb;
	rb.type = RigidBodyType::Dynamic;
	rb.mass = 1.0f;
	w.addComponent(e, rb);
	return e;
}
} // namespace

// MUTATION: in PhysicsWorld::applyRegionHold, return at the top (or install no test in
// PhysicsCells::begin): the "gone" and "not yet" runs with the hold still pass their
// control CHECKs, and the CHECKs on the held crate fail, since it is below the floor.
TEST_CASE("Cells and physics: nothing falls through the floor of a cell that has gone, or has not come yet")
{
	const auto root = std::filesystem::temp_directory_path() / "he_cell_floor";
	for (const bool gone : { false, true })
	for (const bool hold : { true, false })
	{
		CAPTURE(gone);
		CAPTURE(hold);
		he_test::removeAllQuiet(root);
		writePhysicsCell(root / "W.cells" / "cell_0_0.hescene", 0.0f);
		HE::CellManifest m;
		REQUIRE(HE::CellManifest::parse(
			R"({"cellSize": 100, "loadRadius": 60, "unloadRadius": 120, "lookaheadSec": 0,
			    "dir": "W.cells", "list": [[0,0,1,1]]})", m));

		PhysicsCells rig;
		rig.hold = hold;
		// A crate of the base, standing where the cell's floor is (or will be).
		const Entity crate = makeCrate(rig.world, "Crate", { 50.0f, 0.5f, 50.0f });
		rig.phys.initialize(rig.world);
		rig.begin(m, root);
		const std::vector<HE::CellAnchor> near = { anchorAt(50.0) }, away = { anchorAt(5000.0) };
		const auto crateY   = [&] { return tf(rig.world, crate).position.y; };
		const auto loadCell = [&]
		{
			pumpAnchors(rig.streamer, rig.world, near, 100.0, [&] { return rig.streamer.isLoaded(0, 0); });
			REQUIRE(rig.streamer.isLoaded(0, 0));
		};

		if (!gone)
		{
			// NOT YET: the crate stands over a cell that is not built, and time passes.
			rig.step(60);
			if (!hold)
			{
				CHECK(crateY() < -2.0f);   // the control: it has dropped out of the world
				continue;
			}
			CHECK(crateY() == doctest::Approx(0.5f).epsilon(0.05));
			CHECK(rig.phys.heldCount() == 1u);
			// The cell comes; the crate is let go onto its floor and stays on it.
			loadCell();
			rig.step(120);
			CHECK(crateY() == doctest::Approx(0.5f).epsilon(0.1));
			CHECK(rig.phys.heldCount() == 0u);
			continue;
		}

		// GONE: the cell is there, the crate rests on its floor...
		loadCell();
		rig.step(120);
		CHECK(crateY() == doctest::Approx(0.5f).epsilon(0.1));
		REQUIRE(rig.phys.raycast({ 10.0f, 5.0f, 10.0f }, { 0.0f, -1.0f, 0.0f }, 20.0f).hit);   // the floor is in the physics world
		// ...and then the anchor leaves, and the cell with it.
		rig.streamer.update(rig.world, away, 0.0);
		REQUIRE_FALSE(rig.streamer.isLoaded(0, 0));
		CHECK_FALSE(rig.phys.raycast({ 10.0f, 5.0f, 10.0f }, { 0.0f, -1.0f, 0.0f }, 20.0f).hit);   // and now it is not
		// The crate has lain still for two seconds, so it sleeps, and Jolt does not wake what lay on
		// a body that is taken away. Something touches it, as something does in a game sooner or
		// later (a gust, a nudge from an NPC): that is when the missing floor shows.
		CHECK(rig.phys.addImpulse(static_cast<uint32_t>(crate), { 0.0f, 0.01f, 0.0f }));
		rig.step(120);
		if (!hold)
		{
			CHECK(crateY() < -5.0f);       // the control: nothing is under it, it fell
			continue;
		}
		CHECK(crateY() == doctest::Approx(0.5f).epsilon(0.05));
		CHECK(rig.phys.heldCount() == 1u);
		// The anchor comes back: the floor again, and the crate, which was never gone, on it.
		loadCell();
		rig.step(120);
		CHECK(crateY() == doctest::Approx(0.5f).epsilon(0.1));
		CHECK(rig.phys.heldCount() == 0u);
		CHECK(rig.phys.raycast({ 10.0f, 5.0f, 10.0f }, { 0.0f, -1.0f, 0.0f }, 20.0f).hit);
	}
	he_test::removeAllQuiet(root);
}

TEST_CASE("Cells and physics: a character put down in a cell that is still loading stands where it was put")
{
	const auto root = std::filesystem::temp_directory_path() / "he_cell_char";
	he_test::removeAllQuiet(root);
	writePhysicsCell(root / "W.cells" / "cell_0_0.hescene", 0.0f);
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(
		R"({"cellSize": 100, "loadRadius": 60, "unloadRadius": 120, "lookaheadSec": 0,
		    "dir": "W.cells", "list": [[0,0,1,1]]})", m));

	PhysicsCells rig;
	// The player's shape as EntityHost builds it: a character that walks and the kinematic proxy.
	const Entity player = rig.world.createEntity("Player");
	tf(rig.world, player).position = { 50.0f, 1.2f, 50.0f };
	rig.world.addComponent(player, CharacterControllerComponent{});
	{ RigidBodyComponent rb; rb.type = RigidBodyType::Kinematic; rig.world.addComponent(player, rb); }
	rig.phys.initialize(rig.world);
	rig.begin(m, root);
	const auto playerY = [&] { return tf(rig.world, player).position.y; };

	// A spawn into a cell that is not there yet: a second passes and the player has not fallen.
	rig.step(60);
	CHECK(playerY() == doctest::Approx(1.2f).epsilon(0.001));
	CHECK(rig.phys.heldCount() == 1u);

	// The cell is built: the player is let go, lands on the floor and is grounded there.
	pumpAnchors(rig.streamer, rig.world, { anchorAt(50.0) }, 100.0, [&] { return rig.streamer.isLoaded(0, 0); });
	REQUIRE(rig.streamer.isLoaded(0, 0));
	rig.step(120);
	CHECK(rig.phys.heldCount() == 0u);
	CHECK(playerY() > 0.5f);
	CHECK(playerY() < 1.3f);
	CHECK(rig.world.registry().get<CharacterControllerComponent>(player).isGrounded);
	he_test::removeAllQuiet(root);
}

// MUTATION: in PhysicsWorld::removeEntityTree, map requeueJoints to JointFate::Drop, or let
// resolvePendingJoints count a patient entry against kMaxJointAttempts: the `requeue` run
// ends with a door that fell, and CHECK(hasJoint) fails.
TEST_CASE("Cells and physics: a joint from the base to a cell is built again when the cell is back")
{
	const auto root = std::filesystem::temp_directory_path() / "he_cell_joint";
	for (const bool requeue : { true, false })
	{
		CAPTURE(requeue);
		he_test::removeAllQuiet(root);
		const HE::UUID frame = writeFrameCell(root / "W.cells" / "cell_0_0.hescene", 0.0f);
		// Another cell, built in many slices: each one is a pass over the pending joints, and
		// together they are more of them than a joint that is merely early gets (eight).
		writePhysicsCell(root / "W.cells" / "cell_3_0.hescene", 300.0f, 60);
		HE::CellManifest m;
		REQUIRE(HE::CellManifest::parse(
			R"({"cellSize": 100, "loadRadius": 60, "unloadRadius": 120, "lookaheadSec": 0,
			    "dir": "W.cells", "list": [[0,0,1,1],[3,0,61,61]]})", m));

		PhysicsCells rig;
		rig.requeue = requeue;
		// The door is the base's, hinged to the frame, which is in a cell.
		const Entity door = makeCrate(rig.world, "Door", { 52.0f, 10.0f, 50.0f });
		{
			JointComponent j;
			j.type   = JointType::Fixed;
			j.target = frame;
			rig.world.registry().emplace_or_replace<JointComponent>(door, j);
		}
		rig.phys.initialize(rig.world);
		rig.begin(m, root, /*sliceEntities=*/4);
		const uint32_t doorId = static_cast<uint32_t>(door);
		const auto doorY      = [&] { return tf(rig.world, door).position.y; };
		CHECK_FALSE(rig.phys.hasJoint(doorId));   // the frame is not loaded

		// The cell comes: the joint, which waited for the frame since the scene started, is built.
		pumpAnchors(rig.streamer, rig.world, { anchorAt(50.0) }, 100.0, [&] { return rig.streamer.isLoaded(0, 0); });
		REQUIRE(rig.streamer.isLoaded(0, 0));
		CHECK(rig.phys.hasJoint(doorId));
		rig.step(120);
		CHECK(doorY() == doctest::Approx(10.0f).epsilon(0.02));

		// The player walks to the other cell: this one goes, that one is built slice by slice.
		const size_t slicesBefore = rig.streamer.stats().slicesDone;
		pumpAnchors(rig.streamer, rig.world, { anchorAt(350.0) }, 100.0,
		            [&] { return rig.streamer.isLoaded(3, 0) && !rig.streamer.isLoaded(0, 0); });
		REQUIRE(rig.streamer.isLoaded(3, 0));
		REQUIRE_FALSE(rig.streamer.isLoaded(0, 0));
		CHECK(rig.streamer.stats().slicesDone - slicesBefore >= 12u);   // each with bodies: a pass each, well over eight
		CHECK_FALSE(rig.phys.hasJoint(doorId));
		rig.step(60);
		CHECK(doorY() == doctest::Approx(10.0f).epsilon(0.02));   // held where it hung, not fallen

		// And back: the frame is the same entity again, and the joint is built again.
		pumpAnchors(rig.streamer, rig.world, { anchorAt(50.0) }, 100.0, [&] { return rig.streamer.isLoaded(0, 0); });
		REQUIRE(rig.streamer.isLoaded(0, 0));
		// The door has been held since the frame went; the frame is back and the door is let go by
		// the first step, which is also when its joint to the frame is built again.
		rig.step(1);
		CHECK(rig.phys.hasJoint(doorId) == requeue);
		rig.step(120);
		if (requeue)
			CHECK(doorY() == doctest::Approx(10.0f).epsilon(0.02));
		else
			CHECK(doorY() < 5.0f);   // the control: nothing holds the door, it fell
	}
	he_test::removeAllQuiet(root);
}

TEST_CASE("Cells and physics: three thousand bodies come in through cells under the table size, and a cell the reserve cannot take waits")
{
	const auto root = std::filesystem::temp_directory_path() / "he_cell_bodies3k";
	he_test::removeAllQuiet(root);
	constexpr int kCells = 12;   // 12 cells of 300 static bodies
	std::string list;
	for (int c = 0; c < kCells; ++c)
	{
		writePhysicsCell(root / "W.cells" / ("cell_" + std::to_string(c) + "_0.hescene"), static_cast<float>(c) * 100.0f, 299);
		list += std::string(c ? "," : "") + "[" + std::to_string(c) + ",0,300,300]";
	}
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(
		R"({"cellSize": 100, "loadRadius": 60, "unloadRadius": 120, "lookaheadSec": 0, "dir": "W.cells", "list": [)" + list + "]}", m));
	// An anchor whose reach (radiusScale 10) covers the whole row: every cell is wanted.
	const std::vector<HE::CellAnchor> wide = { anchorAt(600.0, 10.0f) };
	const auto allLoaded = [](HE::CellStreamer& s) { return s.stats().loaded == static_cast<size_t>(kCells); };

	SUBCASE("with the room there is, they all come in")
	{
		PhysicsCells rig;
		rig.phys.initialize(rig.world);
		rig.begin(m, root);
		pumpAnchors(rig.streamer, rig.world, wide, 100.0, [&] { return allLoaded(rig.streamer); });
		REQUIRE(allLoaded(rig.streamer));
		CHECK(rig.phys.bodyCount() == 3600u);
		CHECK(rig.phys.bodyCount() < PhysicsWorld::kMaxBodies);
		CHECK(rig.streamer.stats().deferredForBodies == 0u);
		// The reserve itself: nine tenths of the table, counted from what the world owns.
		CHECK(HE::cellBodiesFit(rig.phys, 1000));
		CHECK_FALSE(HE::cellBodiesFit(rig.phys, PhysicsWorld::kMaxBodies));
		CHECK_FALSE(HE::cellBodiesFit(rig.phys, PhysicsWorld::kMaxBodies / 10 * 9 - 3600 + 1));
		CHECK(HE::cellBodiesFit(rig.phys, PhysicsWorld::kMaxBodies / 10 * 9 - 3600));
	}

	SUBCASE("a cell that does not fit the reserve waits, and nothing behind it takes its room")
	{
		PhysicsCells rig;
		size_t limit = 1000;   // room for three cells of 300 and a bit
		rig.fits = [&](uint32_t n) { return rig.phys.bodyCount() + n <= limit; };
		rig.phys.initialize(rig.world);
		rig.begin(m, root);
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (std::chrono::steady_clock::now() < until
		       && !(rig.streamer.stats().deferredForBodies > 0 && rig.streamer.stats().loaded >= 3))
		{
			rig.streamer.update(rig.world, wide, 100.0);
			std::this_thread::yield();
		}
		for (int i = 0; i < 20; ++i) rig.streamer.update(rig.world, wide, 100.0);   // time to try again, and not to take it
		CHECK(rig.streamer.stats().loaded == 3u);
		CHECK(rig.phys.bodyCount() == 900u);
		CHECK(rig.streamer.stats().deferredForBodies > 0u);
		CHECK(rig.streamer.stats().ready > 0u);   // parsed, waiting for room, not dropped
		// A cell that is put off is not built, and so not settled either.
		CHECK_FALSE(rig.streamer.isSettled({ 600.0, 0.0, 50.0 }, 500.0));

		// Room again: the rest comes in, all of it.
		limit = 100000;
		pumpAnchors(rig.streamer, rig.world, wide, 100.0, [&] { return allLoaded(rig.streamer); });
		REQUIRE(allLoaded(rig.streamer));
		CHECK(rig.phys.bodyCount() == 3600u);
		CHECK(rig.streamer.isSettled({ 600.0, 0.0, 50.0 }, 500.0));
	}

	SUBCASE("a base that fills the reserve alone does not freeze the first cell")
	{
		PhysicsCells rig;
		rig.fits = [](uint32_t) { return false; };   // nothing ever fits
		rig.phys.initialize(rig.world);
		rig.begin(m, root);
		// Only cell 0 is wanted from x = -30 (cell 1 is 130 m away); it is built because nothing is
		// built yet, whatever the reserve says. Which of two cells wanted together is parsed first is
		// up to the pool, so the test does not leave that to chance.
		pumpUntil(rig.streamer, rig.world, { anchorAt(-30.0) }, [&] { return rig.streamer.isLoaded(0, 0); });
		CHECK(rig.streamer.isLoaded(0, 0));
		CHECK(rig.streamer.stats().deferredForBodies == 0u);
		// Cell 1 is wanted as well from x = 50, and now something is built, so it waits.
		pumpUntil(rig.streamer, rig.world, { anchorAt(50.0) }, [&] { return rig.streamer.stats().deferredForBodies > 0; });
		CHECK_FALSE(rig.streamer.isLoaded(1, 0));
		CHECK(rig.streamer.stats().deferredForBodies > 0u);
	}

	SUBCASE("a cell under way is finished while a nearer one waits for room")
	{
		PhysicsCells rig;
		rig.fits = [](uint32_t) { return false; };
		rig.phys.initialize(rig.world);
		rig.begin(m, root, /*sliceEntities=*/64);
		const auto tick = [&](double x) { rig.streamer.update(rig.world, { anchorAt(x) }, 0.0); std::this_thread::yield(); };
		// Cell 0 alone is wanted from x = -30; with a slice per update it is under way for a while.
		const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (std::chrono::steady_clock::now() < limit && !rig.streamer.isBuilding(0, 0)) tick(-30.0);
		REQUIRE(rig.streamer.isBuilding(0, 0));
		// Then cell 1 is the nearer one (the anchor stands in it) and does not fit.
		while (std::chrono::steady_clock::now() < limit && rig.streamer.stats().deferredForBodies == 0) tick(150.0);
		REQUIRE(rig.streamer.stats().deferredForBodies > 0u);
		// Cell 0 is not left half built behind it.
		while (std::chrono::steady_clock::now() < limit && !rig.streamer.isLoaded(0, 0)) tick(150.0);
		CHECK(rig.streamer.isLoaded(0, 0));
		CHECK_FALSE(rig.streamer.isLoaded(1, 0));
	}
	he_test::removeAllQuiet(root);
}

TEST_CASE("SceneSerializer::sliceForAdditiveLoad: subtrees named as a cluster in the head are never cut apart")
{
	const auto root = std::filesystem::temp_directory_path() / "he_slice_cluster";
	he_test::removeAllQuiet(root);
	writeHouses(root / "c.hescene", 12, 9, 0.0f);   // 1 + 12 * 10 records; two houses fit a slice of 25
	nlohmann::json cell = readScene(root / "c.hescene");
	std::vector<nlohmann::json> tops;
	for (const auto& e : cell["entities"])
		if (e["parent"].is_null()) tops = e["children"].get<std::vector<nlohmann::json>>();
	REQUIRE(tops.size() == 12u);

	// Which slice holds the subtree whose top is `top`; -1 when none does.
	const auto sliceOf = [](const std::vector<nlohmann::json>& slices, const nlohmann::json& top)
	{
		for (size_t i = 0; i < slices.size(); ++i)
			for (const auto& e : slices[i]["entities"])
				if (e["uuid"] == top) return static_cast<int>(i);
		return -1;
	};

	// Without a head, houses go two to a slice: the second and the third fall apart.
	const std::vector<nlohmann::json> plain = SceneSerializer::sliceForAdditiveLoad(nlohmann::json(cell), 25);
	REQUIRE(plain.size() == 7u);
	CHECK(sliceOf(plain, tops[1]) != sliceOf(plain, tops[2]));

	// With the two named as a cluster they are one unit: together in one slice, and the
	// order of everything is the order of the whole file.
	nlohmann::json withHead = cell;
	withHead["streaming"]   = { { "version", 2 }, { "clusters", nlohmann::json::array({ nlohmann::json::array({ tops[1], tops[2] }) }) } };
	const std::vector<nlohmann::json> cut = SceneSerializer::sliceForAdditiveLoad(nlohmann::json(withHead), 25);
	CHECK(sliceOf(cut, tops[1]) == sliceOf(cut, tops[2]));
	CHECK(sliceOf(cut, tops[1]) > 0);
	size_t total = 0;
	std::vector<nlohmann::json> order;
	for (size_t i = 0; i < cut.size(); ++i)
	{
		total += cut[i]["entities"].size();
		if (i == 0) continue;
		for (const auto& e : cut[i]["entities"])
			if (e["parent"] == cut[0]["entities"][0]["uuid"]) order.push_back(e["uuid"]);
	}
	CHECK(total == 121u);
	CHECK(order == tops);

	// A cluster larger than a slice is a slice of its own, like a large subtree.
	withHead["streaming"]["clusters"] = nlohmann::json::array({ nlohmann::json::array({ tops[4], tops[5], tops[6], tops[7] }) });
	const std::vector<nlohmann::json> big = SceneSerializer::sliceForAdditiveLoad(nlohmann::json(withHead), 25);
	CHECK(sliceOf(big, tops[4]) == sliceOf(big, tops[7]));
	CHECK(sliceOf(big, tops[3]) != sliceOf(big, tops[4]));

	// Members that do not stand side by side in the root's children are not pulled together: the
	// order of the whole load wins.
	withHead["streaming"]["clusters"] = nlohmann::json::array({ nlohmann::json::array({ tops[1], tops[9] }) });
	const std::vector<nlohmann::json> apart = SceneSerializer::sliceForAdditiveLoad(nlohmann::json(withHead), 25);
	CHECK(sliceOf(apart, tops[1]) != sliceOf(apart, tops[9]));
	he_test::removeAllQuiet(root);
}
