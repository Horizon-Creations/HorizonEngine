#include "doctest.h"

#include "McpToolRegistry.h"
#include "EditorCommands.h"
#include "EditorUndo.h"
#include "CollabUndo.h"

#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/TerrainSculpt.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// ─── Shaping the ground from outside the editor ──────────────────────────────
// The four terrain tools exist because entity_set_components can only hand a
// landscape over as base64. What is worth asking of them is what a running
// editor could not be asked without a project, a window and a pair of eyes:
//
//   • does a dab at a WORLD position move the ground under that position and
//     nowhere else — the terrain sits at a non-zero origin in most of these
//     cases on purpose, because a landscape at [0,0,0] hides every missing
//     conversion,
//   • is the edit really a gateway command (does undo put the ground back),
//   • do the never-serialised runtime fields survive the round trip through the
//     scene JSON — the weightmap texture uuid above all, because losing it
//     registers a SECOND texture on the next tick and abandons the first, once
//     per call, invisibly,
//   • does a request that touches nothing come back as "nothing happened"
//     rather than as a refusal, and does a refusal arrive under the code a
//     client is supposed to branch on.

using HE::Ed::EditorCommands;
using HE::Ed::McpTerrainHooks;
using HE::Ed::McpTool;
using HE::Ed::McpToolRegistry;
using HE::Ed::Origin;
using HE::Ed::ToolResult;
using nlohmann::json;

namespace {

// The gateway wired the way EditorApplication wires it, minus the editor —
// modelled on tests/test_mcp_tools_entity.cpp's harness on purpose, so there is
// one theory of how the editor behaves rather than two.
struct Fixture
{
	HorizonWorld    world;
	EditorCommands  cmds;
	EditorUndo      snapshotUndo;
	CollabUndo      collabUndo;
	McpToolRegistry registry;

	HE::Ed::SnapshotUndoSink snapshotSink{ &snapshotUndo, [this] { return playing; } };
	HE::Ed::CollabUndoSink   collabSink{ &collabUndo };

	bool playing   = false;
	int  regenCalls = 0;

	Fixture()
	{
		snapshotUndo.setWorld(&world);
		cmds.setWorld(&world);

		EditorCommands::Hooks h;
		h.isPlaying = [this] { return playing; };
		h.inSession = [] { return false; };
		h.nowMs     = [] { return std::uint64_t{ 0 }; };
		cmds.setHooks(std::move(h));
		cmds.setUndoSinks(&snapshotSink, &collabSink);

		McpTerrainHooks th;
		th.regenerate = [this] { ++regenCalls; };
		HE::Ed::registerTerrainTools(registry, cmds, std::move(th));
	}

	// A landscape at a deliberately non-zero origin: 100×100 metres centred on
	// (200, 0, -50), flat (seed 0) unless a test says otherwise.
	Entity makeTerrain(const glm::vec3& at = glm::vec3(200.0f, 0.0f, -50.0f),
	                   std::uint32_t res = 17)
	{
		const Entity e = world.createEntity("Landscape");
		auto& reg = world.registry();
		TransformComponent xf;
		xf.position = at;
		reg.emplace<TransformComponent>(e, xf);
		TerrainComponent tc;
		tc.sizeX = 100.0f; tc.sizeZ = 100.0f;
		tc.resolution = res;
		tc.seed = 0;
		reg.emplace<TerrainComponent>(e, tc);
		return e;
	}

	ToolResult call(const std::string& name, const json& args = json::object())
	{
		const McpTool* t = registry.find(name);
		REQUIRE_MESSAGE(t != nullptr, "no such tool registered: " << name);
		return t->handler(args);
	}

	std::string uuid(Entity e) { return HE::Ed::uuidOf(world, e); }

	TerrainComponent& terrain(Entity e) { return world.registry().get<TerrainComponent>(e); }
};

std::string codeOf(const ToolResult& r) { return r.isError ? r.errorCode : std::string("ok"); }

// World Y of the ground under a world XZ, read the way the renderer reads it.
float groundAt(Fixture& f, Entity e, float wx, float wz)
{
	const auto& tc = f.terrain(e);
	const glm::vec3 wp = HE::worldPositionOf(f.world, e);
	return wp.y + terrainHeightAt(tc, wx - wp.x, wz - wp.z);
}

} // namespace

TEST_CASE("Every terrain tool arrives with a schema and a name a client can use")
{
	Fixture f;

	const char* expected[] = { "terrain_info", "terrain_heightmap",
	                           "terrain_sculpt", "terrain_paint" };
	for (const char* name : expected)
	{
		const McpTool* t = f.registry.find(name);
		REQUIRE_MESSAGE(t != nullptr, "missing tool: " << name);
		CHECK(McpToolRegistry::enforceNameRule(t->name));
		CHECK(t->inputSchema.is_object());
		CHECK(t->inputSchema["type"] == "object");
		CHECK_FALSE(t->description.empty());
	}

	// The two that change the scene are marked — that flag is what makes the
	// bridge write the `MCP:` console line a human searches for afterwards.
	CHECK(f.registry.find("terrain_sculpt")->mutates);
	CHECK(f.registry.find("terrain_paint")->mutates);
	CHECK_FALSE(f.registry.find("terrain_info")->mutates);
	CHECK_FALSE(f.registry.find("terrain_heightmap")->mutates);
}

TEST_CASE("terrain_info finds the landscapes and reports the world rectangle they cover")
{
	Fixture f;
	const Entity e = f.makeTerrain();

	const ToolResult all = f.call("terrain_info");
	REQUIRE_MESSAGE(!all.isError, codeOf(all));
	REQUIRE(all.content["count"] == 1);
	const json& t = all.content["terrains"][0];

	CHECK(t["uuid"] == f.uuid(e));
	CHECK(t["name"] == "Landscape");
	// Bounds are WORLD, so the terrain's own position has to be in them.
	CHECK(t["bounds"]["minX"].get<float>() == doctest::Approx(150.0f));
	CHECK(t["bounds"]["maxX"].get<float>() == doctest::Approx(250.0f));
	CHECK(t["bounds"]["minZ"].get<float>() == doctest::Approx(-100.0f));
	CHECK(t["bounds"]["maxZ"].get<float>() == doctest::Approx(0.0f));
	CHECK(t["gridStep"][0].get<float>() == doctest::Approx(100.0f / 16.0f));
	CHECK(t["sculpted"] == false);
	CHECK(t["painted"] == false);
	// 17 is already 2⁴+1, so nothing is going to move under the client.
	CHECK_FALSE(t.contains("resolutionSnapsTo"));

	// The same call for one uuid answers with just that one.
	const ToolResult one = f.call("terrain_info", json{ { "uuid", f.uuid(e) } });
	REQUIRE_MESSAGE(!one.isError, codeOf(one));
	CHECK(one.content["count"] == 1);
}

TEST_CASE("terrain_info warns that a non-2ⁿ+1 resolution will move under the client")
{
	Fixture f;
	f.makeTerrain(glm::vec3(0.0f), /*res=*/128);

	const ToolResult info = f.call("terrain_info");
	REQUIRE_MESSAGE(!info.isError, codeOf(info));
	CHECK(info.content["terrains"][0]["resolutionSnapsTo"] == 129);
}

TEST_CASE("terrain_sculpt raises the ground under the world position it was given")
{
	Fixture f;
	const Entity e = f.makeTerrain();

	// Dead centre of the landscape, in WORLD coordinates.
	const ToolResult r = f.call("terrain_sculpt", json{
		{ "uuid", f.uuid(e) }, { "x", 200.0 }, { "z", -50.0 },
		{ "op", "raise" }, { "radius", 10.0 }, { "falloff", 0.0 }, { "amount", 5.0 },
	});
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content["changed"].get<int>() > 0);
	CHECK(r.content["heightAtCenter"].get<float>() == doctest::Approx(5.0f));

	// Under the brush: raised. Well outside it: untouched. A brush that got the
	// world→local conversion wrong would land 200 metres away, i.e. off the
	// landscape entirely — which is exactly why the terrain is not at the origin.
	CHECK(groundAt(f, e, 200.0f, -50.0f) == doctest::Approx(5.0f));
	CHECK(groundAt(f, e, 240.0f, -90.0f) == doctest::Approx(0.0f));

	// It went through the gateway, so the editor can take it back. Undo restores
	// a whole-world snapshot, which mints new entt handles — the uuid is the only
	// address that survives it, which is exactly why the tools take one.
	CHECK(f.regenCalls == 1);
	const std::string id = f.uuid(e);
	REQUIRE(f.snapshotUndo.undo());
	const Entity after = HE::Ed::entityByUuid(f.world, id);
	REQUIRE((after != entt::null));
	CHECK(groundAt(f, after, 200.0f, -50.0f) == doctest::Approx(0.0f));
}

TEST_CASE("terrain_sculpt 'set' lands on the world height it was asked for, offset and all")
{
	Fixture f;
	// The landscape itself sits 30 metres up: a tool that forgot to subtract the
	// entity's own Y would put the ground at 42 instead of 12.
	const Entity e = f.makeTerrain(glm::vec3(0.0f, 30.0f, 0.0f));

	const ToolResult r = f.call("terrain_sculpt", json{
		{ "uuid", f.uuid(e) }, { "x", 0.0 }, { "z", 0.0 },
		{ "op", "set" }, { "height", 42.0 }, { "radius", 12.0 }, { "falloff", 0.0 },
	});
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content["heightAtCenter"].get<float>() == doctest::Approx(42.0f));
	CHECK(groundAt(f, e, 0.0f, 0.0f) == doctest::Approx(42.0f));
	// Stored terrain-LOCAL, which is what the mesh generator reads.
	const auto& tc = f.terrain(e);
	CHECK(tc.sculptHeights[8 * 17 + 8] == doctest::Approx(12.0f));

	// And 'set' without a height is refused rather than guessed at.
	const ToolResult bad = f.call("terrain_sculpt", json{
		{ "uuid", f.uuid(e) }, { "x", 0.0 }, { "z", 0.0 }, { "op", "set" },
	});
	CHECK(codeOf(bad) == "invalid_payload");
}

TEST_CASE("terrain_sculpt keeps the fBm shape it found instead of flattening it")
{
	Fixture f;
	const Entity e = f.makeTerrain(glm::vec3(0.0f), /*res=*/33);
	{
		auto& tc = f.terrain(e);
		tc.seed = 1337;          // a noisy landscape that was never sculpted
		tc.heightScale = 20.0f;
	}
	// Exactly on a grid vertex (100 m over 32 cells = 3.125 m), so the value the
	// sampler interpolates and the value the field stores are the same number
	// and the comparison below is not measuring bilinear error.
	const float probeX = -40.625f, probeZ = 40.625f;
	const float farAway = groundAt(f, e, probeX, probeZ);

	const ToolResult r = f.call("terrain_sculpt", json{
		{ "uuid", f.uuid(e) }, { "x", 0.0 }, { "z", 0.0 },
		{ "op", "raise" }, { "radius", 5.0 }, { "falloff", 0.0 }, { "amount", 1.0 },
	});
	REQUIRE_MESSAGE(!r.isError, codeOf(r));

	// The corner the brush never reached still has the noise it had. Baking the
	// height field from a flat grid instead of from computeTerrainHeightField
	// would have levelled the whole landscape on the first dab.
	CHECK(groundAt(f, e, probeX, probeZ) == doctest::Approx(farAway));

	// …and "still has the noise" only says something if there was noise. Asserted
	// over the whole baked field rather than at the one probe, which could sit on
	// a zero crossing by luck.
	const auto& hs = f.terrain(e).sculptHeights;
	REQUIRE_FALSE(hs.empty());
	const float lo = *std::min_element(hs.begin(), hs.end());
	const float hi = *std::max_element(hs.begin(), hs.end());
	CHECK(hi - lo > 1.0f);
}

TEST_CASE("terrain_sculpt outside the landscape changes nothing and says so")
{
	Fixture f;
	const Entity e = f.makeTerrain();

	const ToolResult r = f.call("terrain_sculpt", json{
		{ "uuid", f.uuid(e) }, { "x", 9000.0 }, { "z", 9000.0 },
		{ "op", "raise" }, { "radius", 5.0 }, { "falloff", 0.0 }, { "amount", 5.0 },
	});
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content["changed"] == 0);
	CHECK(r.content["regenerated"] == false);
	// No command, so no undo entry for a change that never happened.
	CHECK(f.regenCalls == 0);
}

TEST_CASE("terrain_sculpt refuses what a client can correct, under the code it branches on")
{
	Fixture f;
	const Entity e = f.makeTerrain();

	CHECK(codeOf(f.call("terrain_sculpt", json{
		{ "uuid", "not-a-uuid" }, { "x", 0.0 }, { "z", 0.0 } })) == "not_found");

	// An entity that exists but is not a landscape — the mistake a "not found"
	// would send a client in a circle over.
	const Entity crate = f.world.createEntity("Crate");
	CHECK(codeOf(f.call("terrain_sculpt", json{
		{ "uuid", f.uuid(crate) }, { "x", 0.0 }, { "z", 0.0 } })) == "invalid_payload");

	CHECK(codeOf(f.call("terrain_sculpt", json{
		{ "uuid", f.uuid(e) }, { "x", 200.0 }, { "z", -50.0 },
		{ "op", "melt" } })) == "invalid_payload");

	CHECK(codeOf(f.call("terrain_sculpt", json{
		{ "uuid", f.uuid(e) }, { "x", 200.0 }, { "z", -50.0 },
		{ "radius", 0.0 }, { "falloff", 0.0 } })) == "invalid_payload");

	// x and z are the position, and a missing one is not the same as zero.
	CHECK(codeOf(f.call("terrain_sculpt", json{ { "uuid", f.uuid(e) },
	                                            { "x", 200.0 } })) == "invalid_payload");

	f.playing = true;
	CHECK(codeOf(f.call("terrain_sculpt", json{
		{ "uuid", f.uuid(e) }, { "x", 200.0 }, { "z", -50.0 },
		{ "amount", 5.0 } })) == "play_mode");
}

TEST_CASE("terrain_heightmap reads back what terrain_sculpt wrote, as numbers")
{
	Fixture f;
	const Entity e = f.makeTerrain();

	f.call("terrain_sculpt", json{
		{ "uuid", f.uuid(e) }, { "x", 200.0 }, { "z", -50.0 },
		{ "op", "set" }, { "height", 7.0 }, { "radius", 20.0 }, { "falloff", 0.0 },
	});

	const ToolResult r = f.call("terrain_heightmap", json{
		{ "uuid", f.uuid(e) },
		{ "minX", 190.0 }, { "maxX", 210.0 },
		{ "minZ", -60.0 }, { "maxZ", -40.0 },
		{ "samples", 5 },
	});
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content["samples"] == 5);
	REQUIRE(r.content["heights"].size() == 25);
	// The whole rectangle sits inside the 20 m disc, so every sample is on it.
	for (const auto& h : r.content["heights"])
		CHECK(h.get<float>() == doctest::Approx(7.0f));
	CHECK(r.content["origin"][0].get<float>() == doctest::Approx(190.0f));
	CHECK(r.content["step"][0].get<float>() == doctest::Approx(5.0f));
	CHECK(r.content["max"].get<float>() == doctest::Approx(7.0f));
}

TEST_CASE("terrain_heightmap is row-major with Z as the outer axis")
{
	Fixture f;
	const Entity e = f.makeTerrain();

	// A plateau over the LOW-Z half only: that is what tells the two axes apart.
	// A tool that laid the rows out along X would read it the other way round.
	f.call("terrain_sculpt", json{
		{ "uuid", f.uuid(e) }, { "x", 200.0 }, { "z", -90.0 },
		{ "op", "set" }, { "height", 20.0 }, { "radius", 30.0 }, { "falloff", 0.0 },
	});

	const ToolResult rows = f.call("terrain_heightmap", json{
		{ "uuid", f.uuid(e) },
		{ "minX", 199.0 }, { "maxX", 201.0 },
		{ "minZ", -90.0 }, { "maxZ", -10.0 },
		{ "samples", 2 },
	});
	REQUIRE_MESSAGE(!rows.isError, codeOf(rows));
	REQUIRE(rows.content["heights"].size() == 4);
	CHECK(rows.content["heights"][0].get<float>() == doctest::Approx(20.0f));  // row 0 = minZ
	CHECK(rows.content["heights"][1].get<float>() == doctest::Approx(20.0f));
	CHECK(rows.content["heights"][2].get<float>() == doctest::Approx(0.0f));   // row 1 = maxZ
	CHECK(rows.content["heights"][3].get<float>() == doctest::Approx(0.0f));
}

TEST_CASE("terrain_heightmap defaults to the whole landscape and clamps a rectangle that overhangs it")
{
	Fixture f;
	const Entity e = f.makeTerrain();

	const ToolResult full = f.call("terrain_heightmap", json{ { "uuid", f.uuid(e) } });
	REQUIRE_MESSAGE(!full.isError, codeOf(full));
	CHECK(full.content["samples"] == 33);
	CHECK(full.content["heights"].size() == 33 * 33);
	CHECK(full.content["rect"]["minX"].get<float>() == doctest::Approx(150.0f));

	// Asking over the edge is a legal question near a border; the rect that was
	// actually read comes back rather than a refusal.
	const ToolResult over = f.call("terrain_heightmap", json{
		{ "uuid", f.uuid(e) },
		{ "minX", -9000.0 }, { "maxX", 9000.0 },
		{ "minZ", -9000.0 }, { "maxZ", 9000.0 },
		{ "samples", 4 },
	});
	REQUIRE_MESSAGE(!over.isError, codeOf(over));
	CHECK(over.content["rect"]["minX"].get<float>() == doctest::Approx(150.0f));
	CHECK(over.content["rect"]["maxZ"].get<float>() == doctest::Approx(0.0f));

	// And the grid is capped, so a client cannot ask for a megabyte of numbers.
	const ToolResult capped = f.call("terrain_heightmap", json{
		{ "uuid", f.uuid(e) }, { "samples", 10000 } });
	REQUIRE_MESSAGE(!capped.isError, codeOf(capped));
	CHECK(capped.content["samples"] == 128);
}

TEST_CASE("terrain_paint puts weight on the layer it was given, at the world position it was given")
{
	Fixture f;
	const Entity e = f.makeTerrain();

	const ToolResult r = f.call("terrain_paint", json{
		{ "uuid", f.uuid(e) }, { "x", 200.0 }, { "z", -50.0 },
		{ "layer", 2 }, { "radius", 12.0 }, { "falloff", 0.0 }, { "strength", 1.0 },
	});
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content["changed"] == true);
	CHECK(r.content["mixAtCenter"][2].get<float>() == doctest::Approx(1.0f));
	CHECK(r.content["mixAtCenter"][0].get<float>() == doctest::Approx(0.0f));

	// The stored map agrees, and a corner far from the brush is still layer 0.
	const auto& tc = f.terrain(e);
	REQUIRE(tc.layerWeights.size() == static_cast<size_t>(tc.weightRes) * tc.weightRes * 4);
	const std::uint32_t wr = tc.weightRes;
	const std::uint8_t* mid = &tc.layerWeights[((wr / 2) * wr + wr / 2) * 4];
	CHECK(static_cast<int>(mid[2]) == 255);
	const std::uint8_t* corner = &tc.layerWeights[0];
	CHECK(static_cast<int>(corner[0]) == 255);

	CHECK(f.regenCalls == 1);
	const std::string id = f.uuid(e);
	REQUIRE(f.snapshotUndo.undo());
	const Entity after = HE::Ed::entityByUuid(f.world, id);
	REQUIRE((after != entt::null));
	CHECK(f.terrain(after).layerWeights.empty());
}

TEST_CASE("terrain_paint refuses a layer a landscape does not have")
{
	Fixture f;
	const Entity e = f.makeTerrain();

	CHECK(codeOf(f.call("terrain_paint", json{
		{ "uuid", f.uuid(e) }, { "x", 200.0 }, { "z", -50.0 },
		{ "layer", 4 } })) == "invalid_payload");
	CHECK(codeOf(f.call("terrain_paint", json{
		{ "uuid", f.uuid(e) }, { "x", 200.0 }, { "z", -50.0 },
		{ "layer", 0 }, { "strength", 3.0 } })) == "invalid_payload");

	f.playing = true;
	CHECK(codeOf(f.call("terrain_paint", json{
		{ "uuid", f.uuid(e) }, { "x", 200.0 }, { "z", -50.0 },
		{ "layer", 1 } })) == "play_mode");
}

TEST_CASE("The runtime state the scene format cannot carry survives an edit")
{
	Fixture f;
	const Entity e = f.makeTerrain();
	{
		// What a landscape looks like after a tick of TerrainSystem: a registered
		// weightmap texture and a chunk grid that was built once.
		auto& tc = f.terrain(e);
		tc.weightmapTextureId = HE::UUID{ 0x1234u, 0x5678u };
		tc.builtRes            = 17;
		tc.builtChunksPerSide  = 4;
		tc.dirty               = false;
	}

	const ToolResult r = f.call("terrain_sculpt", json{
		{ "uuid", f.uuid(e) }, { "x", 200.0 }, { "z", -50.0 },
		{ "op", "raise" }, { "radius", 6.0 }, { "falloff", 0.0 }, { "amount", 3.0 },
	});
	REQUIRE_MESSAGE(!r.isError, codeOf(r));

	// The component the gateway put back was rebuilt from the scene JSON, which
	// carries none of this. Losing the texture uuid would register a second
	// texture on the next tick and abandon the first, once per call; losing the
	// chunk grid would rebuild all of them for a six-metre dab.
	const auto& tc = f.terrain(e);
	CHECK(tc.weightmapTextureId == HE::UUID{ 0x1234u, 0x5678u });
	CHECK(tc.builtRes == 17);
	CHECK(tc.builtChunksPerSide == 4);
	CHECK_FALSE(tc.dirty);
	CHECK(tc.regionDirty);
	// The dirty rect is terrain-LOCAL and covers the brush: centre 0,0 ± 6.
	CHECK(tc.dirtyMinX == doctest::Approx(-6.0f));
	CHECK(tc.dirtyMaxZ == doctest::Approx(6.0f));

	// A paint marks the weightmap for re-upload but leaves the chunk meshes
	// alone — no height moved.
	const ToolResult p = f.call("terrain_paint", json{
		{ "uuid", f.uuid(e) }, { "x", 200.0 }, { "z", -50.0 }, { "layer", 1 } });
	REQUIRE_MESSAGE(!p.isError, codeOf(p));
	const auto& tc2 = f.terrain(e);
	CHECK(tc2.weightsDirty);
	CHECK_FALSE(tc2.regionDirty);
	CHECK(tc2.weightmapTextureId == HE::UUID{ 0x1234u, 0x5678u });
	CHECK_FALSE(tc2.dirty);
}

TEST_CASE("A landscape whose grid was never built is rebuilt whole, not by region")
{
	Fixture f;
	const Entity e = f.makeTerrain();
	CHECK(f.terrain(e).builtRes == 0);

	const ToolResult r = f.call("terrain_sculpt", json{
		{ "uuid", f.uuid(e) }, { "x", 200.0 }, { "z", -50.0 },
		{ "op", "raise" }, { "radius", 6.0 }, { "falloff", 0.0 }, { "amount", 3.0 },
	});
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	// There is nothing to skip yet: the chunk entities do not exist.
	CHECK(f.terrain(e).dirty);
}

TEST_CASE("Sculpting snaps the height grid before it writes, not after")
{
	Fixture f;
	const Entity e = f.makeTerrain(glm::vec3(0.0f), /*res=*/128);

	const ToolResult r = f.call("terrain_sculpt", json{
		{ "uuid", f.uuid(e) }, { "x", 0.0 }, { "z", 0.0 },
		{ "op", "set" }, { "height", 9.0 }, { "radius", 15.0 }, { "falloff", 0.0 },
	});
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	// The chunk builder would have snapped 128 → 129 and resampled the field on
	// its next pass, moving every height the client had just written. Taking the
	// snap first is what keeps the number it reads back the number it asked for.
	CHECK(r.content["resolution"] == 129);
	CHECK(f.terrain(e).resolution == 129);
	CHECK(f.terrain(e).sculptHeights.size() == 129u * 129u);
	CHECK(groundAt(f, e, 0.0f, 0.0f) == doctest::Approx(9.0f));
}

// ── The brush maths on its own ───────────────────────────────────────────────
// TerrainSculpt is a value in, a value out — worth asking directly, because the
// tools above can only show that SOMETHING moved.

TEST_CASE("TerrainSculpt::apply falls off linearly and stops at radius + falloff")
{
	TerrainComponent tc;
	tc.sizeX = 100.0f; tc.sizeZ = 100.0f; tc.resolution = 101; tc.seed = 0;

	const TerrainSculpt::Result r = TerrainSculpt::apply(
		tc, 0.0f, 0.0f, TerrainSculpt::Op::Raise, 10.0f, 10.0f, 4.0f);
	REQUIRE(r.ok);
	CHECK(r.changed > 0);

	// Sampled between grid points, so the tolerance is the grid's, not the
	// brush's: 101 snaps to 129 and one cell is 0.78 m of a 4 m ramp.
	CHECK(terrainHeightAt(tc, 0.0f, 0.0f)  == doctest::Approx(4.0f));   // full strength
	CHECK(terrainHeightAt(tc, 9.0f, 0.0f)  == doctest::Approx(4.0f));   // inside the disc
	CHECK(terrainHeightAt(tc, 15.0f, 0.0f) == doctest::Approx(2.0f).epsilon(0.05)); // halfway
	CHECK(terrainHeightAt(tc, 25.0f, 0.0f) == doctest::Approx(0.0f));   // past it
}

TEST_CASE("TerrainSculpt::apply smooths toward the neighbourhood, not toward zero")
{
	TerrainComponent tc;
	tc.sizeX = 100.0f; tc.sizeZ = 100.0f; tc.resolution = 33; tc.seed = 0;
	TerrainSculpt::ensureHeights(tc);
	// A single spike on an otherwise flat field.
	tc.sculptHeights[16 * 33 + 16] = 30.0f;

	const TerrainSculpt::Result r = TerrainSculpt::apply(
		tc, 0.0f, 0.0f, TerrainSculpt::Op::Smooth, 8.0f, 0.0f, 1.0f);
	REQUIRE(r.ok);
	// The spike came down, its neighbours came up, and the mean is roughly kept.
	CHECK(tc.sculptHeights[16 * 33 + 16] < 10.0f);
	CHECK(tc.sculptHeights[16 * 33 + 17] > 0.0f);
}

TEST_CASE("TerrainSculpt::apply is honest about a terrain it cannot touch")
{
	TerrainComponent tc;
	tc.sizeX = 0.0f;   // degenerate
	CHECK_FALSE(TerrainSculpt::apply(tc, 0, 0, TerrainSculpt::Op::Raise, 5, 0, 1).ok);

	TerrainComponent ok;
	ok.sizeX = 50.0f; ok.sizeZ = 50.0f; ok.resolution = 17;
	// A legal request that lands nowhere: ok, and nothing changed.
	const TerrainSculpt::Result off = TerrainSculpt::apply(
		ok, 900.0f, 900.0f, TerrainSculpt::Op::Raise, 5, 0, 1);
	CHECK(off.ok);
	CHECK(off.changed == 0);
	CHECK_FALSE(ok.regionDirty);
}

TEST_CASE("TerrainSculpt op names round-trip and an unknown one is refused")
{
	using Op = TerrainSculpt::Op;
	for (Op op : { Op::Raise, Op::Lower, Op::Set, Op::Flatten, Op::Smooth, Op::Roughen })
	{
		Op back{};
		REQUIRE(TerrainSculpt::opFromName(TerrainSculpt::opName(op), back));
		CHECK(static_cast<int>(back) == static_cast<int>(op));
	}
	Op unused{};
	CHECK_FALSE(TerrainSculpt::opFromName("erode", unused));
	CHECK_FALSE(TerrainSculpt::opFromName(nullptr, unused));
}
