#include "doctest.h"
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/RenderGraph.h>
#include <HorizonRendering/RenderPass.h>
#include <HorizonRendering/CommandBuffer.h>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>

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

TEST_CASE("Inert passes record nothing")
{
	RenderWorld world;
	world.objects.push_back(makeObj(1, { 0, 0, 0 }));
	std::vector<uint32_t> sorted = { 0 };

	CommandBuffer cmds;
	ShadowPass{}.execute(world, sorted, cmds);
	PostProcessPass{}.execute(world, sorted, cmds);
	CHECK(cmds.drawCalls().empty());
}
