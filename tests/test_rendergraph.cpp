#include "doctest.h"
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/RenderGraph.h>
#include <HorizonRendering/RenderPass.h>
#include <HorizonRendering/RenderTarget.h>
#include <HorizonRendering/CommandBuffer.h>
#include <HorizonRendering/RenderSorter.h>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <string>
#include <vector>

// The pass pipeline is GPU-free: passes turn a RenderWorld + sorted index list
// into a CommandBuffer of DrawCalls, which the backends replay. That makes the
// foundation unit-testable. CommandBuffer/RenderGraph/RenderPass are compiled
// directly into the test target.

namespace
{
	RenderObject makeObj(uint32_t entityId, glm::vec3 pos)
	{
		RenderObject o;
		o.meshAssetId.hi = entityId;        // distinct id per object
		o.meshAssetId.lo = entityId * 7 + 1;
		o.transform      = glm::translate(glm::mat4(1.0f), pos);
		o.entityId       = entityId;
		o.lod            = static_cast<uint8_t>(entityId % 4);
		return o;
	}
}

TEST_CASE("GeometryPass records one draw call per sorted index")
{
	RenderWorld world;
	world.objects.push_back(makeObj(10, { 1, 0, 0 }));
	world.objects.push_back(makeObj(20, { 0, 2, 0 }));
	world.objects.push_back(makeObj(30, { 0, 0, 3 }));

	// Submit only objects 2 and 0, in that order (as the sorter would).
	std::vector<uint32_t> sorted = { 2, 0 };

	CommandBuffer cmds;
	GeometryPass  pass;
	pass.execute(world, sorted, cmds);

	REQUIRE(cmds.drawCalls().size() == 2);

	// Draw order and payload match the sorted indices.
	const DrawCall& d0 = cmds.drawCalls()[0];
	CHECK(d0.entityId == 30);
	CHECK(d0.meshAssetId.hi == 30);
	CHECK(d0.transform == world.objects[2].transform);

	const DrawCall& d1 = cmds.drawCalls()[1];
	CHECK(d1.entityId == 10);
	CHECK(d1.meshAssetId.hi == 10);
	CHECK(d1.transform == world.objects[0].transform);
}

TEST_CASE("GeometryPass carries the material override into the draw call")
{
	RenderWorld world;
	RenderObject withMat = makeObj(42, { 0, 0, 0 });
	withMat.materialAssetId.hi = 0xABCD;
	withMat.materialAssetId.lo = 0x1234;
	RenderObject noMat = makeObj(43, { 1, 0, 0 }); // materialAssetId left null
	world.objects.push_back(withMat);
	world.objects.push_back(noMat);

	std::vector<uint32_t> sorted = { 0, 1 };
	CommandBuffer cmds;
	GeometryPass{}.execute(world, sorted, cmds);

	REQUIRE(cmds.drawCalls().size() == 2);
	CHECK(cmds.drawCalls()[0].materialAssetId.hi == 0xABCD);
	CHECK(cmds.drawCalls()[0].materialAssetId.lo == 0x1234);
	CHECK(cmds.drawCalls()[1].materialAssetId == HE::UUID{}); // null = mesh's own material
}

TEST_CASE("GeometryPass skips out-of-range indices")
{
	RenderWorld world;
	world.objects.push_back(makeObj(1, { 0, 0, 0 }));

	std::vector<uint32_t> sorted = { 0, 99 }; // 99 is out of range

	CommandBuffer cmds;
	GeometryPass  pass;
	pass.execute(world, sorted, cmds);

	CHECK(cmds.drawCalls().size() == 1);
	CHECK(cmds.drawCalls()[0].entityId == 1);
}

TEST_CASE("RenderGraph executes passes and resets the buffer each frame")
{
	RenderWorld world;
	world.objects.push_back(makeObj(5, { 0, 0, 0 }));
	world.objects.push_back(makeObj(6, { 1, 1, 1 }));
	std::vector<uint32_t> sorted = { 0, 1 };

	RenderGraph graph;
	CHECK(graph.empty());
	graph.addPass(std::make_unique<GeometryPass>());
	CHECK_FALSE(graph.empty());

	CommandBuffer cmds;
	graph.execute(world, sorted, cmds);
	CHECK(cmds.drawCalls().size() == 2);

	// Running again must not accumulate — execute() resets the buffer.
	graph.execute(world, sorted, cmds);
	CHECK(cmds.drawCalls().size() == 2);

	graph.clear();
	CHECK(graph.empty());
	graph.execute(world, sorted, cmds); // no passes → empty buffer
	CHECK(cmds.drawCalls().empty());
}

TEST_CASE("ShadowPass with shadow disabled records no draws")
{
	RenderWorld world; // shadow.enabled = false by default
	std::vector<uint32_t> sorted = {};
	CommandBuffer cmds;
	ShadowPass{}.execute(world, sorted, cmds);
	CHECK(cmds.drawCalls().empty());
	CHECK_FALSE(cmds.hasPostProcess());
}

TEST_CASE("PostProcessPass signals the post-process flag and records no draws")
{
	RenderWorld world;
	std::vector<uint32_t> sorted = {};
	CommandBuffer cmds;
	PostProcessPass{}.execute(world, sorted, cmds);
	CHECK(cmds.drawCalls().empty());
	CHECK(cmds.hasPostProcess());

	// reset must clear the flag.
	cmds.reset();
	CHECK_FALSE(cmds.hasPostProcess());
}

TEST_CASE("ShadowPass casts from the light frustum, not the camera-culled set")
{
	RenderWorld world;
	world.objects.push_back(makeObj(1, { 0,  0, 0 }));   // inside the light frustum
	world.objects.push_back(makeObj(2, { 50, 0, 0 }));   // far outside it
	// Give each object a small valid world AABB so the frustum test is exercised.
	for (RenderObject& o : world.objects)
	{
		const glm::vec3 p = glm::vec3(o.transform[3]);
		o.worldBounds.expand(p - glm::vec3(0.5f));
		o.worldBounds.expand(p + glm::vec3(0.5f));
	}
	// Directional light looking down -Z with a tight ortho box around the origin:
	// object 1 is inside, object 2 (x = 50) lies outside.
	world.shadow.enabled  = true;
	world.shadow.viewProj = glm::ortho(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 100.0f)
	                      * glm::lookAt(glm::vec3(0, 0, 10), glm::vec3(0), glm::vec3(0, 1, 0));

	// The camera-culled set is empty (both objects off-screen). The shadow pass
	// must ignore it and decide casters from the light's frustum — otherwise an
	// off-screen object stops casting into the still-visible scene (the bug this
	// guards against).
	const std::vector<uint32_t> cameraCulled = {};
	CommandBuffer cmds;
	ShadowPass{}.execute(world, cameraCulled, cmds);

	REQUIRE(cmds.drawCalls().size() == 1);   // only the in-frustum caster
	CHECK(cmds.drawCalls()[0].entityId == 1);
}

// ─── GPU instancing batching tests ──────────────────────────────────────────
// These run on the CPU-side GeometryPass only; no GL context is needed.

namespace {
	// Helper: N objects that all share the same meshAssetId / materialAssetId.
	void addSameMeshObjects(RenderWorld& world, HE::UUID sharedMesh, int n)
	{
		for (int i = 0; i < n; ++i)
		{
			RenderObject o;
			o.meshAssetId = sharedMesh;
			o.transform   = glm::translate(glm::mat4(1.0f), glm::vec3(float(i), 0.0f, 0.0f));
			o.entityId    = static_cast<uint32_t>(100 + i);
			world.objects.push_back(o);
		}
	}
}

TEST_CASE("GeometryPass batches consecutive same-mesh objects into one instanced DrawCall")
{
	HE::UUID sharedMesh; sharedMesh.hi = 42; sharedMesh.lo = 7;

	RenderWorld world;
	addSameMeshObjects(world, sharedMesh, 3);
	std::vector<uint32_t> sorted = { 0, 1, 2 };

	CommandBuffer cmds;
	GeometryPass{}.execute(world, sorted, cmds);

	REQUIRE(cmds.drawCalls().size() == 1);
	const DrawCall& dc = cmds.drawCalls()[0];
	CHECK(dc.meshAssetId == sharedMesh);
	CHECK(dc.instanceCount == 3);
	REQUIRE(dc.instanceTransforms.size() == 3);
	CHECK(dc.instanceTransforms[0] == world.objects[0].transform);
	CHECK(dc.instanceTransforms[1] == world.objects[1].transform);
	CHECK(dc.instanceTransforms[2] == world.objects[2].transform);
}

TEST_CASE("GeometryPass does not batch objects with different materials")
{
	HE::UUID sharedMesh; sharedMesh.hi = 99; sharedMesh.lo = 1;
	HE::UUID matA; matA.hi = 1; matA.lo = 0;
	HE::UUID matB; matB.hi = 2; matB.lo = 0;

	RenderWorld world;
	for (int i = 0; i < 2; ++i)
	{
		RenderObject o;
		o.meshAssetId     = sharedMesh;
		o.materialAssetId = (i == 0) ? matA : matB;
		o.transform       = glm::mat4(1.0f);
		o.entityId        = static_cast<uint32_t>(i);
		world.objects.push_back(o);
	}
	std::vector<uint32_t> sorted = { 0, 1 };

	CommandBuffer cmds;
	GeometryPass{}.execute(world, sorted, cmds);

	// Different materials → two separate DrawCalls, each with a single instance.
	REQUIRE(cmds.drawCalls().size() == 2);
	CHECK(cmds.drawCalls()[0].instanceCount == 1);
	CHECK(cmds.drawCalls()[0].instanceTransforms.empty());
	CHECK(cmds.drawCalls()[1].instanceCount == 1);
	CHECK(cmds.drawCalls()[1].instanceTransforms.empty());
}

TEST_CASE("GeometryPass batches only contiguous runs (non-consecutive same mesh stays separate)")
{
	HE::UUID meshA; meshA.hi = 1; meshA.lo = 0;
	HE::UUID meshB; meshB.hi = 2; meshB.lo = 0;

	RenderWorld world;
	// Pattern: A B A  →  3 separate draws (A and the second A are not adjacent)
	for (int i = 0; i < 3; ++i)
	{
		RenderObject o;
		o.meshAssetId = (i == 1) ? meshB : meshA;
		o.transform   = glm::mat4(1.0f);
		o.entityId    = static_cast<uint32_t>(i);
		world.objects.push_back(o);
	}
	std::vector<uint32_t> sorted = { 0, 1, 2 };

	CommandBuffer cmds;
	GeometryPass{}.execute(world, sorted, cmds);

	CHECK(cmds.drawCalls().size() == 3);
	for (const DrawCall& dc : cmds.drawCalls())
	{
		CHECK(dc.instanceCount == 1);
		CHECK(dc.instanceTransforms.empty());
	}
}

TEST_CASE("GeometryPass produces one batch + one single for partial run (A A B)")
{
	HE::UUID meshA; meshA.hi = 5; meshA.lo = 0;
	HE::UUID meshB; meshB.hi = 6; meshB.lo = 0;

	RenderWorld world;
	for (int i = 0; i < 3; ++i)
	{
		RenderObject o;
		o.meshAssetId = (i < 2) ? meshA : meshB;
		o.transform   = glm::translate(glm::mat4(1.0f), glm::vec3(float(i), 0.0f, 0.0f));
		o.entityId    = static_cast<uint32_t>(i);
		world.objects.push_back(o);
	}
	std::vector<uint32_t> sorted = { 0, 1, 2 };

	CommandBuffer cmds;
	GeometryPass{}.execute(world, sorted, cmds);

	REQUIRE(cmds.drawCalls().size() == 2);
	// First draw: A×2 batch
	CHECK(cmds.drawCalls()[0].meshAssetId == meshA);
	CHECK(cmds.drawCalls()[0].instanceCount == 2);
	REQUIRE(cmds.drawCalls()[0].instanceTransforms.size() == 2);
	// Second draw: B single
	CHECK(cmds.drawCalls()[1].meshAssetId == meshB);
	CHECK(cmds.drawCalls()[1].instanceCount == 1);
	CHECK(cmds.drawCalls()[1].instanceTransforms.empty());
}

TEST_CASE("GeometryPass does not batch objects with different instanceTint")
{
	// Two particles at different points in their life share mesh + material but
	// carry distinct color/alpha-over-life tints — must not be instanced together,
	// or they would all draw with one shared tint (see RenderObject::instanceTint).
	HE::UUID sharedMesh; sharedMesh.hi = 11; sharedMesh.lo = 3;

	RenderWorld world;
	for (int i = 0; i < 2; ++i)
	{
		RenderObject o;
		o.meshAssetId  = sharedMesh;
		o.transform    = glm::mat4(1.0f);
		o.entityId     = static_cast<uint32_t>(i);
		o.instanceTint = (i == 0) ? glm::vec4(1.0f, 1.0f, 1.0f, 1.0f) : glm::vec4(1.0f, 0.5f, 0.2f, 0.6f);
		world.objects.push_back(o);
	}
	std::vector<uint32_t> sorted = { 0, 1 };

	CommandBuffer cmds;
	GeometryPass{}.execute(world, sorted, cmds);

	REQUIRE(cmds.drawCalls().size() == 2);
	CHECK(cmds.drawCalls()[0].instanceCount == 1);
	CHECK(cmds.drawCalls()[0].instanceTint == glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
	CHECK(cmds.drawCalls()[1].instanceCount == 1);
	CHECK(cmds.drawCalls()[1].instanceTint == glm::vec4(1.0f, 0.5f, 0.2f, 0.6f));
}

TEST_CASE("GeometryPass batches objects that share the same non-identity instanceTint")
{
	HE::UUID sharedMesh; sharedMesh.hi = 11; sharedMesh.lo = 4;
	const glm::vec4 tint(0.8f, 0.3f, 0.1f, 0.9f);

	RenderWorld world;
	for (int i = 0; i < 3; ++i)
	{
		RenderObject o;
		o.meshAssetId  = sharedMesh;
		o.transform    = glm::translate(glm::mat4(1.0f), glm::vec3(float(i), 0.0f, 0.0f));
		o.entityId     = static_cast<uint32_t>(i);
		o.instanceTint = tint;
		world.objects.push_back(o);
	}
	std::vector<uint32_t> sorted = { 0, 1, 2 };

	CommandBuffer cmds;
	GeometryPass{}.execute(world, sorted, cmds);

	REQUIRE(cmds.drawCalls().size() == 1);
	CHECK(cmds.drawCalls()[0].instanceCount == 3);
	CHECK(cmds.drawCalls()[0].instanceTint == tint);
}

TEST_CASE("RenderPass declares its render-target I/O")
{
	CHECK(GeometryPass{}.describe().output.id == kBackbufferTarget);
	CHECK(GeometryPass{}.describe().inputCount == 0);

	const RenderPassIO shadow = ShadowPass{}.describe();
	CHECK(shadow.output.format   == RenderTargetFormat::Depth);
	CHECK(shadow.output.sizeMode == RenderTargetSize::Fixed);
	CHECK(shadow.output.id       != kBackbufferTarget);

	const RenderPassIO post = PostProcessPass{}.describe();
	CHECK(post.output.id  == kBackbufferTarget);
	CHECK(post.inputCount == 1); // samples the scene color target
}

TEST_CASE("RenderGraph sink dispatches each pass with its declared target")
{
	RenderWorld world;
	world.objects.push_back(makeObj(1, { 0, 0, 0 }));
	std::vector<uint32_t> sorted = { 0 };

	RenderGraph graph;
	graph.addPass(std::make_unique<GeometryPass>());
	graph.addPass(std::make_unique<ShadowPass>());      // inert, declares a depth target
	graph.addPass(std::make_unique<PostProcessPass>()); // inert, declares backbuffer + input

	struct Rec { std::string name; RenderTargetId out; size_t draws; uint32_t inputs; };
	std::vector<Rec> recs;
	graph.execute(world, sorted,
		[&](const RenderPass& pass, const RenderPassIO& io, const CommandBuffer& cmds)
		{
			recs.push_back({ pass.name(), io.output.id, cmds.drawCalls().size(), io.inputCount });
		});

	REQUIRE(recs.size() == 3);
	// Order preserved; each pass gets its own freshly-reset command buffer.
	CHECK(recs[0].name == std::string("GeometryPass"));
	CHECK(recs[0].out  == kBackbufferTarget);
	CHECK(recs[0].draws == 1); // geometry recorded the one visible object
	CHECK(recs[1].name == std::string("ShadowPass"));
	CHECK(recs[1].out  != kBackbufferTarget);
	CHECK(recs[1].draws == 0); // inert — and the buffer was reset before it
	CHECK(recs[2].name == std::string("PostProcessPass"));
	CHECK(recs[2].out  == kBackbufferTarget);
	CHECK(recs[2].inputs == 1);
}

// ─── Mesh sections (material slots) ─────────────────────────────────────────
// A multi-section mesh reaches the pass as ONE RenderObject carrying its slot
// table (RenderObject::sections); GeometryPass expands it into one DrawCall per
// slot. The one-section shape — the only one every pre-section asset produces —
// must come out exactly as before: one whole-mesh draw.

namespace {
	std::vector<RenderSection> twoSlots()
	{
		HE::UUID matA; matA.hi = 11; matA.lo = 1;
		HE::UUID matB; matB.hi = 22; matB.lo = 2;
		RenderSection a; a.indexOffset = 0;  a.indexCount = 36; a.materialAssetId = matA;
		RenderSection b; b.indexOffset = 36; b.indexCount = 24; b.materialAssetId = matB;
		return { a, b };
	}
}

TEST_CASE("GeometryPass: a one-section object records exactly one whole-mesh draw (regression guard)")
{
	RenderWorld world;
	world.objects.push_back(makeObj(5, { 0, 0, 0 })); // sections empty = legacy shape
	std::vector<uint32_t> sorted = { 0 };

	CommandBuffer cmds;
	GeometryPass{}.execute(world, sorted, cmds);

	REQUIRE(cmds.drawCalls().size() == 1);
	const DrawCall& dc = cmds.drawCalls()[0];
	CHECK(dc.indexOffset  == 0);
	CHECK(dc.indexCount   == 0);   // 0 = the whole index buffer
	CHECK(dc.sectionIndex == -1);  // not a section draw
	CHECK(dc.materialAssetId == HE::UUID{});
}

TEST_CASE("GeometryPass expands a two-section object into one draw per slot")
{
	RenderWorld world;
	RenderObject o = makeObj(7, { 1, 2, 3 });
	o.sections = twoSlots();
	world.objects.push_back(o);
	std::vector<uint32_t> sorted = { 0 };

	CommandBuffer cmds;
	GeometryPass{}.execute(world, sorted, cmds);

	REQUIRE(cmds.drawCalls().size() == 2);
	const DrawCall& d0 = cmds.drawCalls()[0];
	const DrawCall& d1 = cmds.drawCalls()[1];
	// Both draws are the same entity + mesh + transform, differing only in the slot.
	CHECK(d0.meshAssetId == o.meshAssetId);
	CHECK(d1.meshAssetId == o.meshAssetId);
	CHECK(d0.entityId == 7);
	CHECK(d1.entityId == 7);
	CHECK(d0.transform == o.transform);
	CHECK(d1.transform == o.transform);
	CHECK(d0.sectionIndex == 0);
	CHECK(d0.indexOffset  == 0);
	CHECK(d0.indexCount   == 36);
	CHECK(d0.materialAssetId == o.sections[0].materialAssetId);
	CHECK(d1.sectionIndex == 1);
	CHECK(d1.indexOffset  == 36);
	CHECK(d1.indexCount   == 24);
	CHECK(d1.materialAssetId == o.sections[1].materialAssetId);
}

TEST_CASE("GeometryPass skips an empty slot instead of drawing the whole mesh with it")
{
	RenderWorld world;
	RenderObject o = makeObj(8, { 0, 0, 0 });
	o.sections = twoSlots();
	o.sections[1].indexCount = 0; // an empty section (allowed by the loader)
	world.objects.push_back(o);
	std::vector<uint32_t> sorted = { 0 };

	CommandBuffer cmds;
	GeometryPass{}.execute(world, sorted, cmds);

	REQUIRE(cmds.drawCalls().size() == 1);
	CHECK(cmds.drawCalls()[0].sectionIndex == 0);
	CHECK(cmds.drawCalls()[0].indexCount   == 36);
}

TEST_CASE("GeometryPass batches same-mesh objects with identical sections: one instanced draw per slot")
{
	HE::UUID sharedMesh; sharedMesh.hi = 77; sharedMesh.lo = 3;
	RenderWorld world;
	addSameMeshObjects(world, sharedMesh, 3);
	for (RenderObject& o : world.objects) o.sections = twoSlots();
	std::vector<uint32_t> sorted = { 0, 1, 2 };

	CommandBuffer cmds;
	GeometryPass{}.execute(world, sorted, cmds);

	REQUIRE(cmds.drawCalls().size() == 2);
	for (size_t s = 0; s < 2; ++s)
	{
		const DrawCall& dc = cmds.drawCalls()[s];
		CHECK(dc.sectionIndex == static_cast<int32_t>(s));
		CHECK(dc.instanceCount == 3);
		REQUIRE(dc.instanceTransforms.size() == 3);
		CHECK(dc.instanceTransforms[0] == world.objects[0].transform);
		CHECK(dc.instanceTransforms[2] == world.objects[2].transform);
		CHECK(dc.materialAssetId == world.objects[0].sections[s].materialAssetId);
	}
}

TEST_CASE("GeometryPass does not batch same-mesh objects whose section tables differ")
{
	HE::UUID sharedMesh; sharedMesh.hi = 78; sharedMesh.lo = 4;
	RenderWorld world;
	addSameMeshObjects(world, sharedMesh, 2);
	world.objects[0].sections = twoSlots();
	world.objects[1].sections = twoSlots();
	world.objects[1].sections[1].materialAssetId.lo = 99; // slot 1 re-pointed
	std::vector<uint32_t> sorted = { 0, 1 };

	CommandBuffer cmds;
	GeometryPass{}.execute(world, sorted, cmds);

	// Two objects × two slots, none instanced together.
	REQUIRE(cmds.drawCalls().size() == 4);
	for (const DrawCall& dc : cmds.drawCalls())
	{
		CHECK(dc.instanceCount == 1);
		CHECK(dc.instanceTransforms.empty());
	}
}

TEST_CASE("GeometryPass does not batch a sectioned object with a plain one of the same mesh")
{
	HE::UUID sharedMesh; sharedMesh.hi = 79; sharedMesh.lo = 5;
	RenderWorld world;
	addSameMeshObjects(world, sharedMesh, 2);
	world.objects[0].sections = twoSlots(); // e.g. no override
	// objects[1] keeps an empty table (an entity override made it draw whole)
	std::vector<uint32_t> sorted = { 0, 1 };

	CommandBuffer cmds;
	GeometryPass{}.execute(world, sorted, cmds);

	REQUIRE(cmds.drawCalls().size() == 3); // 2 slot draws + 1 whole-mesh draw
	CHECK(cmds.drawCalls()[0].sectionIndex == 0);
	CHECK(cmds.drawCalls()[1].sectionIndex == 1);
	CHECK(cmds.drawCalls()[2].sectionIndex == -1);
	CHECK(cmds.drawCalls()[2].indexCount == 0);
}

TEST_CASE("GeometryPass hands the param block only to the slots that draw the entity's material")
{
	// A whole-mesh override (materialAssetId) with a slot override on top: the
	// extractor leaves slot 0 on the whole-mesh material and slot 1 on the
	// other one. The HeParams block was merged for the former and must not
	// ride onto the latter.
	RenderWorld world;
	RenderObject o = makeObj(9, { 0, 0, 0 });
	o.sections        = twoSlots();
	o.materialAssetId = o.sections[0].materialAssetId;
	o.paramOverride.assign(64, 0.5f);
	world.objects.push_back(o);
	std::vector<uint32_t> sorted = { 0 };

	CommandBuffer cmds;
	GeometryPass{}.execute(world, sorted, cmds);

	REQUIRE(cmds.drawCalls().size() == 2);
	CHECK(cmds.drawCalls()[0].paramOverride.size() == 64);
	CHECK(cmds.drawCalls()[1].paramOverride.empty());
}

TEST_CASE("GeometryPass expands a two-section skinned object into one skinned draw per slot, bones and all")
{
	RenderWorld world;
	SkinnedRenderObject so;
	so.meshAssetId  = HE::UUID::generate();
	so.entityId     = 5;
	so.transform    = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f));
	so.boneMatrices = { glm::mat4(1.0f), glm::mat4(2.0f), glm::mat4(3.0f) };
	so.sections     = twoSlots();
	world.skinnedObjects.push_back(so);

	CommandBuffer cmds;
	GeometryPass{}.execute(world, {}, cmds);

	CHECK(cmds.drawCalls().empty());
	REQUIRE(cmds.skinnedDrawCalls().size() == 2);
	const SkinnedDrawCall& d0 = cmds.skinnedDrawCalls()[0];
	const SkinnedDrawCall& d1 = cmds.skinnedDrawCalls()[1];
	CHECK(d0.meshAssetId == so.meshAssetId);
	CHECK(d1.meshAssetId == so.meshAssetId);
	CHECK(d0.entityId == 5);
	CHECK(d1.transform == so.transform);
	CHECK(d0.sectionIndex == 0); CHECK(d0.indexOffset == 0);  CHECK(d0.indexCount == 36);
	CHECK(d1.sectionIndex == 1); CHECK(d1.indexOffset == 36); CHECK(d1.indexCount == 24);
	CHECK(d0.materialAssetId == so.sections[0].materialAssetId);
	CHECK(d1.materialAssetId == so.sections[1].materialAssetId);
	// The pose goes with every slot: the backends upload it per draw.
	REQUIRE(d0.boneMatrices.size() == 3);
	REQUIRE(d1.boneMatrices.size() == 3);
	CHECK(d1.boneMatrices[1] == glm::mat4(2.0f));

	// An empty slot is skipped, never drawn as "the whole mesh".
	world.skinnedObjects[0].sections[0].indexCount = 0;
	CommandBuffer cmds2;
	GeometryPass{}.execute(world, {}, cmds2);
	REQUIRE(cmds2.skinnedDrawCalls().size() == 1);
	CHECK(cmds2.skinnedDrawCalls()[0].sectionIndex == 1);

	// No table: the one whole-mesh skinned draw it always was.
	world.skinnedObjects[0].sections.clear();
	CommandBuffer cmds3;
	GeometryPass{}.execute(world, {}, cmds3);
	REQUIRE(cmds3.skinnedDrawCalls().size() == 1);
	CHECK(cmds3.skinnedDrawCalls()[0].sectionIndex == -1);
	CHECK(cmds3.skinnedDrawCalls()[0].indexCount == 0);
}

// ─── Depth-only (shadow) batching ───────────────────────────────────────────
// RenderSorter::batchDepthCasters is what the GL and Metal shadow passes feed
// their per-layer sorted list through: consecutive same-mesh casters become
// one instanced draw. CPU-only, like the GeometryPass tests above.

namespace {
	RenderObject depthObj(HE::UUID mesh, uint32_t entityId, float x, bool casts = true)
	{
		RenderObject o;
		o.meshAssetId = mesh;
		o.transform   = glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, 0.0f));
		o.entityId    = entityId;
		o.castsShadow = casts;
		return o;
	}
	HE::UUID meshId(uint64_t hi) { HE::UUID u; u.hi = hi; u.lo = 1; return u; }
}

TEST_CASE("batchDepthCasters collapses a same-mesh run into one batch in list order")
{
	const HE::UUID mesh = meshId(42);
	RenderWorld world;
	for (int i = 0; i < 4; ++i)
		world.objects.push_back(depthObj(mesh, 100 + i, float(i)));
	const std::vector<uint32_t> sorted = { 3, 1, 0, 2 }; // the sorter's order, not index order

	RenderSorter::DepthBatchList out;
	RenderSorter::batchDepthCasters(world, sorted, kNoOwnerEntity, out);

	REQUIRE(out.batches.size() == 1);
	CHECK(out.batches[0].meshAssetId == mesh);
	CHECK(out.batches[0].first == 0);
	CHECK(out.batches[0].count == 4);
	REQUIRE(out.transforms.size() == 4);
	// Transforms follow the sorted order (3,1,0,2 → x = 3,1,0,2).
	CHECK(out.transforms[0][3].x == doctest::Approx(3.0f));
	CHECK(out.transforms[1][3].x == doctest::Approx(1.0f));
	CHECK(out.transforms[2][3].x == doctest::Approx(0.0f));
	CHECK(out.transforms[3][3].x == doctest::Approx(2.0f));
}

TEST_CASE("batchDepthCasters splits on a mesh change and never merges non-adjacent runs")
{
	const HE::UUID a = meshId(1), b = meshId(2);
	RenderWorld world;
	world.objects.push_back(depthObj(a, 1, 0.0f));
	world.objects.push_back(depthObj(a, 2, 1.0f));
	world.objects.push_back(depthObj(b, 3, 2.0f));
	world.objects.push_back(depthObj(a, 4, 3.0f)); // A again, but not adjacent to the first run
	const std::vector<uint32_t> sorted = { 0, 1, 2, 3 };

	RenderSorter::DepthBatchList out;
	RenderSorter::batchDepthCasters(world, sorted, kNoOwnerEntity, out);

	REQUIRE(out.batches.size() == 3);
	CHECK(out.batches[0].meshAssetId == a); CHECK(out.batches[0].first == 0); CHECK(out.batches[0].count == 2);
	CHECK(out.batches[1].meshAssetId == b); CHECK(out.batches[1].first == 2); CHECK(out.batches[1].count == 1);
	CHECK(out.batches[2].meshAssetId == a); CHECK(out.batches[2].first == 3); CHECK(out.batches[2].count == 1);
	CHECK(out.transforms.size() == 4);
}

TEST_CASE("batchDepthCasters drops non-casters and the skipped entity without splitting the run")
{
	const HE::UUID mesh = meshId(7);
	RenderWorld world;
	world.objects.push_back(depthObj(mesh, 10, 0.0f));
	world.objects.push_back(depthObj(mesh, 11, 1.0f, /*casts=*/false)); // billboard: never in a depth map
	world.objects.push_back(depthObj(mesh, 12, 2.0f));
	world.objects.push_back(depthObj(mesh, 13, 3.0f));                  // the light's own mesh (skipped)
	world.objects.push_back(depthObj(mesh, 14, 4.0f));
	const std::vector<uint32_t> sorted = { 0, 1, 2, 3, 4 };

	RenderSorter::DepthBatchList out;
	RenderSorter::batchDepthCasters(world, sorted, /*skipEntity=*/13, out);

	// Filtering happens before run-forming: one run of the three survivors.
	REQUIRE(out.batches.size() == 1);
	CHECK(out.batches[0].count == 3);
	REQUIRE(out.transforms.size() == 3);
	CHECK(out.transforms[0][3].x == doctest::Approx(0.0f));
	CHECK(out.transforms[1][3].x == doctest::Approx(2.0f));
	CHECK(out.transforms[2][3].x == doctest::Approx(4.0f));

	// Every cascade passes kNoOwnerEntity → nothing is skipped on that account.
	RenderSorter::batchDepthCasters(world, sorted, kNoOwnerEntity, out);
	REQUIRE(out.batches.size() == 1);
	CHECK(out.batches[0].count == 4);
}

TEST_CASE("batchDepthRuns honours the pass's own opt-out flag and nothing else")
{
	// The SSAO / GI pre-passes on Metal feed the same batcher as the shadow
	// pass, but each pass has its own per-object opt-out: a billboard that
	// casts no shadow may still contribute AO and vice versa, and the GI
	// G-buffer pre-pass draws everything.
	const HE::UUID mesh = meshId(21);
	RenderWorld world;
	world.objects.push_back(depthObj(mesh, 1, 0.0f));
	{ RenderObject o = depthObj(mesh, 2, 1.0f, /*casts=*/false); o.contributesAO = true;  world.objects.push_back(o); }
	{ RenderObject o = depthObj(mesh, 3, 2.0f, /*casts=*/true);  o.contributesAO = false; world.objects.push_back(o); }
	{ RenderObject o = depthObj(mesh, 4, 3.0f, /*casts=*/false); o.contributesAO = false; world.objects.push_back(o); }
	const std::vector<uint32_t> sorted = { 0, 1, 2, 3 };
	RenderSorter::DepthBatchList out;

	RenderSorter::batchDepthRuns(world, sorted, RenderSorter::DepthFilter::ShadowCasters, kNoOwnerEntity, out);
	REQUIRE(out.batches.size() == 1);
	CHECK(out.batches[0].count == 2); // 0 and 2
	CHECK(out.transforms[1][3].x == doctest::Approx(2.0f));

	RenderSorter::batchDepthRuns(world, sorted, RenderSorter::DepthFilter::AoContributors, kNoOwnerEntity, out);
	REQUIRE(out.batches.size() == 1);
	CHECK(out.batches[0].count == 2); // 0 and 1
	CHECK(out.transforms[1][3].x == doctest::Approx(1.0f));

	RenderSorter::batchDepthRuns(world, sorted, RenderSorter::DepthFilter::All, kNoOwnerEntity, out);
	REQUIRE(out.batches.size() == 1);
	CHECK(out.batches[0].count == 4);

	// skipEntity still applies under every filter.
	RenderSorter::batchDepthRuns(world, sorted, RenderSorter::DepthFilter::All, /*skipEntity=*/1, out);
	REQUIRE(out.batches.size() == 1);
	CHECK(out.batches[0].count == 3);
}

TEST_CASE("batchDepthCasters ignores out-of-range indices and clears stale output")
{
	const HE::UUID mesh = meshId(9);
	RenderWorld world;
	world.objects.push_back(depthObj(mesh, 1, 0.0f));
	RenderSorter::DepthBatchList out;
	out.batches.resize(5); out.transforms.resize(5); // stale from a previous layer
	const std::vector<uint32_t> sorted = { 99, 0 };
	RenderSorter::batchDepthCasters(world, sorted, kNoOwnerEntity, out);
	REQUIRE(out.batches.size() == 1);
	CHECK(out.batches[0].count == 1);
	CHECK(out.transforms.size() == 1);
}
