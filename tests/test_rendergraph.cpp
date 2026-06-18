#include "doctest.h"
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/RenderGraph.h>
#include <HorizonRendering/RenderPass.h>
#include <HorizonRendering/RenderTarget.h>
#include <HorizonRendering/CommandBuffer.h>
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
