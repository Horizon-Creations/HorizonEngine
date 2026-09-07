#include "doctest.h"

#include "McpToolsApi.h"
#include "EditorCommands.h"     // uuidOf / entityByUuid — the same index the editor uses

#include <HorizonScene/Components/TransformComponent.h>

#include <cstring>
#include <string>
#include <vector>

// ─── The engine's API, as tools ──────────────────────────────────────────────
// This file is generated from a table rather than written out, so the questions
// worth asking are about the GENERATOR, not about any one tool:
//
//   • does every row the policy admits come out as a legal, unique MCP name —
//     the '.' → '_' map could collide, and a collision would silently drop a
//     function rather than fail loudly;
//   • does the policy hold in both directions: no tool for a row that writes to
//     the world (that is the whole reason EditorCommands exists), and no tool
//     for the two groups that read past the project;
//   • does a call actually reach the engine and come back typed — a number in,
//     a number out, a uuid in, a position out;
//   • does a wrong call get told what is wrong, rather than acting on entity 0.

using HE::Ed::McpTool;
using HE::Ed::McpToolRegistry;
using HE::Ed::ToolResult;
using nlohmann::json;

namespace {

// The editor's half of a call, minus the editor: a world on the stack and the
// two uuid lookups the gateway already provides as free functions.
struct Fixture
{
	HorizonWorld    world;
	McpToolRegistry registry;

	Fixture()
	{
		HE::Ed::McpApiHooks hooks;
		hooks.makeCtx = [this] {
			HE::api::Ctx c;
			c.world = &world;
			return c;
		};
		hooks.entityByUuid = [this](const std::string& uuid) -> std::int64_t {
			const Entity e = HE::Ed::entityByUuid(world, uuid);
			if (e == entt::null) return -1;
			return static_cast<std::int64_t>(entt::to_integral(e));
		};
		hooks.uuidOf = [this](std::uint32_t handle) -> std::string {
			const Entity e = static_cast<Entity>(handle);
			return world.registry().valid(e) ? HE::Ed::uuidOf(world, e) : std::string();
		};
		HE::Ed::registerApiTools(registry, std::move(hooks));
	}

	ToolResult call(const char* name, const json& args)
	{
		const McpTool* t = registry.find(name);
		REQUIRE(t != nullptr);
		return t->handler(args);
	}
};

int callableRows()
{
	int n = 0;
	for (const HE::api::ApiFn& fn : HE::api::registry())
		if (HE::Ed::apiRowCallable(fn)) ++n;
	return n;
}

} // namespace

TEST_CASE("Every admitted registry row becomes exactly one legal tool")
{
	McpToolRegistry reg;
	HE::Ed::registerApiTools(reg, HE::Ed::McpApiHooks{});

	// api_list plus one tool per admitted row. An equality rather than a "at
	// least": `add` refuses a duplicate name, so a '.' → '_' collision between
	// two ids would show up here as a missing tool and nowhere else.
	CHECK(reg.size() == static_cast<std::size_t>(callableRows()) + 1);
	CHECK(callableRows() > 100);   // the surface is large; a policy that ate it would show

	for (const HE::api::ApiFn& fn : HE::api::registry())
	{
		if (!HE::Ed::apiRowCallable(fn)) continue;
		const std::string name = HE::Ed::apiToolName(fn.id);
		CHECK(McpToolRegistry::enforceNameRule(name));
		const McpTool* t = reg.find(name);
		REQUIRE_MESSAGE(t != nullptr, fn.id);
		// The schema is what the model reads before it calls, so it has to name
		// every parameter and demand every one of them: the registry's own
		// tolerance (a missing argument becomes a zero) is right for an unwired
		// pin in a graph and wrong for a client that forgot the entity.
		CHECK(t->inputSchema["type"] == "object");
		for (const auto& p : fn.params)
		{
			CHECK(t->inputSchema["properties"].contains(p.name));
			bool required = false;
			for (const auto& r : t->inputSchema["required"])
				if (r == p.name) required = true;
			CHECK(required);
		}
		// None of them mutates: that is the policy, and `mutates` is what the
		// bridge reads to refuse a tool during play-in-editor.
		CHECK_FALSE(t->mutates);
	}
}

TEST_CASE("Rows that change the world get no tool")
{
	McpToolRegistry reg;
	HE::Ed::registerApiTools(reg, HE::Ed::McpApiHooks{});

	// The four that would be the obvious shortcut past the gateway.
	CHECK(reg.find("api_transform_setPosition") == nullptr);
	CHECK(reg.find("api_entity_spawn") == nullptr);
	CHECK(reg.find("api_entity_destroy") == nullptr);
	CHECK(reg.find("api_scene_load") == nullptr);
	// Reading the same things is fine.
	CHECK(reg.find("api_transform_getPosition") != nullptr);
	CHECK(reg.find("api_entity_getName") != nullptr);

	// The two pure groups that read past the project.
	CHECK(reg.find("api_clipboard_getText") == nullptr);
	CHECK(reg.find("api_process_which") == nullptr);

	for (const HE::api::ApiFn& fn : HE::api::registry())
		if (fn.isExec) CHECK(reg.find(HE::Ed::apiToolName(fn.id)) == nullptr);
}

TEST_CASE("A pure call reaches the engine and comes back typed")
{
	Fixture f;
	const ToolResult r = f.call("api_math_clamp", json{ { "x", 12.0 }, { "lo", 0.0 },
	                                                    { "hi", 5.0 } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["result"].get<float>() == doctest::Approx(5.0f));
}

TEST_CASE("An entity is addressable by uuid, and comes back as one")
{
	Fixture f;
	const Entity crate = f.world.createEntity("Crate");
	f.world.registry().emplace<TransformComponent>(crate, TransformComponent{});
	f.world.registry().get<TransformComponent>(crate).position = { 1.0f, 2.0f, 3.0f };
	const std::string uuid = HE::Ed::uuidOf(f.world, crate);
	REQUIRE_FALSE(uuid.empty());

	SUBCASE("by uuid")
	{
		const ToolResult r = f.call("api_transform_getPosition", json{ { "entity", uuid } });
		REQUIRE_FALSE(r.isError);
		CHECK(r.content["position"][0].get<float>() == doctest::Approx(1.0f));
		CHECK(r.content["position"][2].get<float>() == doctest::Approx(3.0f));
	}
	SUBCASE("by raw handle, which is what an earlier answer hands back")
	{
		const auto handle = static_cast<std::int64_t>(entt::to_integral(crate));
		const ToolResult r = f.call("api_transform_getPosition", json{ { "entity", handle } });
		REQUIRE_FALSE(r.isError);
		CHECK(r.content["position"][1].get<float>() == doctest::Approx(2.0f));
	}
	SUBCASE("a result that IS an entity carries the uuid alongside")
	{
		const ToolResult r = f.call("api_entity_findByName", json{ { "name", "Crate" } });
		REQUIRE_FALSE(r.isError);
		CHECK(r.content["entity"].get<std::int64_t>()
		      == static_cast<std::int64_t>(entt::to_integral(crate)));
		CHECK(r.content["entityUuid"].get<std::string>() == uuid);
	}
}

TEST_CASE("A wrong call is told what is wrong instead of acting on entity 0")
{
	Fixture f;
	f.world.createEntity("Crate");

	SUBCASE("a uuid nothing answers to")
	{
		const ToolResult r = f.call("api_transform_getPosition",
		                            json{ { "entity", "0123456789abcdef0123456789abcdef" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "not_found");
	}
	SUBCASE("a missing parameter")
	{
		const ToolResult r = f.call("api_math_clamp", json{ { "x", 1.0 }, { "lo", 0.0 } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_argument");
	}
	SUBCASE("a parameter of the wrong shape")
	{
		const ToolResult r = f.call("api_math_clamp",
		                            json{ { "x", "twelve" }, { "lo", 0.0 }, { "hi", 5.0 } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_argument");
	}
	SUBCASE("no project open at all")
	{
		McpToolRegistry reg;
		HE::Ed::registerApiTools(reg, HE::Ed::McpApiHooks{});   // no makeCtx
		const McpTool* t = reg.find("api_math_clamp");
		REQUIRE(t != nullptr);
		const ToolResult r = t->handler(json{ { "x", 1.0 }, { "lo", 0.0 }, { "hi", 5.0 } });
		CHECK(r.isError);
		CHECK(r.errorCode == "no_context");
	}
}

TEST_CASE("api_list explains the rows it will not run")
{
	Fixture f;
	const ToolResult r = f.call("api_list", json{ { "query", "setposition" },
	                                              { "limit", 20 } });
	REQUIRE_FALSE(r.isError);

	bool sawSetPosition = false;
	for (const json& row : r.content["functions"])
	{
		if (row["id"] != "transform.setPosition") continue;
		sawSetPosition = true;
		CHECK(row["callable"] == false);
		CHECK(row["reason"] == "exec");
		// The absence has to point somewhere, or a client only learns that it
		// cannot do the thing — not that another tool does it properly.
		CHECK(row["note"].get<std::string>().find("entity_set_transform")
		      != std::string::npos);
	}
	CHECK(sawSetPosition);

	// The callable half names the tool it became, so a client can go straight
	// from the catalogue to the call.
	const ToolResult pure = f.call("api_list", json{ { "query", "clamp" } });
	REQUIRE_FALSE(pure.isError);
	bool sawClamp = false;
	for (const json& row : pure.content["functions"])
		if (row["id"] == "math.clamp")
		{
			sawClamp = true;
			CHECK(row["callable"] == true);
			CHECK(row["tool"] == "api_math_clamp");
		}
	CHECK(sawClamp);
}

TEST_CASE("api_list can be asked for the callable half only")
{
	Fixture f;
	const ToolResult r = f.call("api_list", json{ { "callableOnly", true },
	                                              { "limit", 5000 } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["matched"].get<int>() == callableRows());
	for (const json& row : r.content["functions"])
	{
		CHECK(row["callable"] == true);
		CHECK(row["isExec"] == false);
	}
}
