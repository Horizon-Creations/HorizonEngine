#include "doctest.h"

#include "McpToolRegistry.h"
#include "EditorCommands.h"
#include "EditorUndo.h"
#include "CollabUndo.h"

#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/EnvironmentLightComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/LightComponent.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

// ─── What an external client can do to the scene ─────────────────────────────
// The tools themselves are thin, and that is the point of testing them: the
// thinness is a claim (everything goes through EditorCommands, nothing here
// touches the world) and a test is the only thing that keeps it true. So the
// questions here are the ones that would break if a handler took a shortcut:
//
//   • does a mutation actually reach the gateway (does undo put it back),
//   • is a partial component patch a MERGE, or does it reset the fields the
//     client did not mention,
//   • does every refusal arrive under the code a client is supposed to branch
//     on — and does `lock_pending` really mean "ask again" rather than "no".

using HE::Ed::CmdError;
using HE::Ed::Command;
using HE::Ed::EditorCommands;
using HE::Ed::McpTool;
using HE::Ed::McpToolRegistry;
using HE::Ed::Origin;
using HE::Ed::ToolResult;
using nlohmann::json;

namespace {

Entity findByName(HorizonWorld& w, const std::string& name)
{
	for (auto [e, n] : w.registry().view<NameComponent>().each())
		if (n.name == name) return e;
	return entt::null;
}

Entity parentOf(HorizonWorld& w, Entity e)
{
	const auto* h = w.registry().try_get<HierarchyComponent>(e);
	return (h && w.registry().valid(h->parent)) ? h->parent : entt::null;
}

// The gateway wired the way EditorApplication wires it, minus the editor:
// the two undo stacks are real, the session is a set of counters. Modelled on
// test_editor_commands.cpp's harness on purpose — a second, differently-shaped
// fake would be a second theory of how the editor behaves.
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
	bool sessionOn = false;

	std::map<std::string, std::uint64_t> subjectByUuid;
	std::uint64_t nextSubject = 1000;

	std::vector<std::uint64_t> ownedLocks;
	std::vector<std::uint64_t> lockedElsewhere;
	std::vector<std::uint64_t> lockRequests;

	int transformPublishes = 0;
	int componentPublishes = 0;

	Fixture()
	{
		snapshotUndo.setWorld(&world);
		cmds.setWorld(&world);

		EditorCommands::Hooks h;
		h.isPlaying  = [this] { return playing; };
		h.inSession  = [this] { return sessionOn; };
		h.nowMs      = [] { return std::uint64_t{ 0 }; };
		h.subjectFor = [this](std::uint32_t handle) { return subjectFor(handle); };
		h.ownsLock   = [this](std::uint64_t s) {
			return std::find(ownedLocks.begin(), ownedLocks.end(), s) != ownedLocks.end();
		};
		h.lockedByOther = [this](std::uint64_t s) {
			return std::find(lockedElsewhere.begin(), lockedElsewhere.end(), s)
			       != lockedElsewhere.end();
		};
		h.requestLock = [this](std::uint64_t s) { lockRequests.push_back(s); return true; };
		h.publishTransform = [this](std::uint64_t, const float[3], const float[3],
		                            const float[3], std::uint64_t) { ++transformPublishes; };
		h.publishComponents = [this](std::uint32_t, const std::vector<std::uint8_t>&) {
			++componentPublishes;
		};
		cmds.setHooks(std::move(h));
		cmds.setUndoSinks(&snapshotSink, &collabSink);

		HE::Ed::registerEntityTools(registry, cmds);
	}

	std::uint64_t subjectFor(std::uint32_t handle)
	{
		const auto e = static_cast<Entity>(static_cast<entt::id_type>(handle));
		const std::string uuid = HE::Ed::uuidOf(world, e);
		if (uuid.empty()) return 0;
		auto it = subjectByUuid.find(uuid);
		if (it != subjectByUuid.end()) return it->second;
		const std::uint64_t s = ++nextSubject;
		subjectByUuid.emplace(uuid, s);
		return s;
	}

	// Calling a tool the way the bridge calls it: by name, out of the registry,
	// with a plain arguments object. Going through `find` rather than holding a
	// handler keeps the name in the test — a renamed tool has to break something.
	ToolResult call(const std::string& name, const json& args = json::object())
	{
		const McpTool* t = registry.find(name);
		REQUIRE_MESSAGE(t != nullptr, "no such tool registered: " << name);
		return t->handler(args);
	}

	std::string uuid(Entity e) { return HE::Ed::uuidOf(world, e); }
};

// A refusal names its code in the failure message, for the same reason the
// gateway's own tests stringify CmdError: "expected ok, got error" would hide
// which of six refusals arrived.
std::string codeOf(const ToolResult& r) { return r.isError ? r.errorCode : std::string("ok"); }

} // namespace

TEST_CASE("Every entity tool arrives with a schema and a name a client can use")
{
	Fixture f;

	const char* expected[] = { "entity_list", "entity_get", "entity_create",
	                           "entity_destroy", "entity_reparent",
	                           "entity_set_transform", "entity_set_components" };
	for (const char* name : expected)
	{
		const McpTool* t = f.registry.find(name);
		REQUIRE_MESSAGE(t != nullptr, "missing tool: " << name);
		CHECK(McpToolRegistry::enforceNameRule(t->name));
		CHECK(t->inputSchema.is_object());
		CHECK(t->inputSchema["type"] == "object");
		CHECK_FALSE(t->description.empty());
	}

	// The five that change the scene are marked, because that flag is what makes
	// the bridge write the `MCP:` console line a human searches for afterwards.
	CHECK(f.registry.find("entity_create")->mutates);
	CHECK(f.registry.find("entity_destroy")->mutates);
	CHECK(f.registry.find("entity_reparent")->mutates);
	CHECK(f.registry.find("entity_set_transform")->mutates);
	CHECK(f.registry.find("entity_set_components")->mutates);
	CHECK_FALSE(f.registry.find("entity_list")->mutates);
	CHECK_FALSE(f.registry.find("entity_get")->mutates);
}

TEST_CASE("Create, move, read back — the round trip a client actually makes")
{
	Fixture f;

	const ToolResult made = f.call("entity_create", json{
		{ "name",     "Crate" },
		{ "position", json::array({ 1.0, 2.0, 3.0 }) },
	});
	REQUIRE_MESSAGE(!made.isError, codeOf(made));
	const std::string uuid = made.content["uuid"];
	CHECK_FALSE(uuid.empty());

	const Entity crate = HE::Ed::entityByUuid(f.world, uuid);
	REQUIRE((crate != entt::null));
	CHECK(f.world.registry().get<NameComponent>(crate).name == "Crate");
	CHECK(f.world.registry().get<TransformComponent>(crate).position.y == doctest::Approx(2.0f));

	// Only the position is given, so the scale the create set has to survive.
	const ToolResult moved = f.call("entity_set_transform", json{
		{ "uuid",     uuid },
		{ "position", json::array({ 10.0, 0.0, -5.0 }) },
	});
	REQUIRE_MESSAGE(!moved.isError, codeOf(moved));

	const auto& tc = f.world.registry().get<TransformComponent>(crate);
	CHECK(tc.position.x == doctest::Approx(10.0f));
	CHECK(tc.position.z == doctest::Approx(-5.0f));
	CHECK(tc.scale.x    == doctest::Approx(1.0f));

	const ToolResult got = f.call("entity_get", json{ { "uuid", uuid } });
	REQUIRE_MESSAGE(!got.isError, codeOf(got));
	CHECK(got.content["name"] == "Crate");
	CHECK(got.content["parent"] == "");
	CHECK(got.content["components"].contains("transform"));
	// No parent, so world and local agree — the point is that the field is
	// composed at all rather than read out of a worldMatrix nothing wrote.
	CHECK(got.content["worldPosition"][0].get<float>() == doctest::Approx(10.0f));
}

TEST_CASE("A tool's change is on the undo stack like any other")
{
	Fixture f;

	const ToolResult made = f.call("entity_create", json{ { "name", "Barrel" } });
	REQUIRE(!made.isError);
	const std::string uuid = made.content["uuid"];
	REQUIRE((HE::Ed::entityByUuid(f.world, uuid) != entt::null));

	f.call("entity_set_transform", json{
		{ "uuid",     uuid },
		{ "position", json::array({ 4.0, 0.0, 0.0 }) },
	});

	// Two commands, two snapshots: back over the move, then back over the create.
	REQUIRE(f.snapshotUndo.undo());
	{
		const Entity e = HE::Ed::entityByUuid(f.world, uuid);
		REQUIRE((e != entt::null));
		CHECK(f.world.registry().get<TransformComponent>(e).position.x == doctest::Approx(0.0f));
	}
	REQUIRE(f.snapshotUndo.undo());
	CHECK((HE::Ed::entityByUuid(f.world, uuid) == entt::null));
}

TEST_CASE("A component patch merges into what is there instead of replacing it")
{
	Fixture f;
	const Entity lamp = f.world.createEntity("Lamp");
	auto& lc = f.world.registry().emplace<LightComponent>(lamp);
	lc.color     = glm::vec3(1.0f, 0.0f, 0.0f);
	lc.intensity = 1.0f;

	const std::string uuid = f.uuid(lamp);
	const ToolResult patched = f.call("entity_set_components", json{
		{ "uuid",  uuid },
		{ "patch", json{ { "light", json{ { "intensity", 7.5 } } } } },
	});
	REQUIRE_MESSAGE(!patched.isError, codeOf(patched));

	// The field that was patched moved; the colour, which the patch never
	// mentioned, is still red. Without the merge in the tool, applyComponents
	// would have rebuilt the whole component from a one-field object and the
	// colour would be back at the component's default.
	const auto& after = f.world.registry().get<LightComponent>(lamp);
	CHECK(after.intensity == doctest::Approx(7.5f));
	CHECK(after.color.r   == doctest::Approx(1.0f));
	CHECK(after.color.g   == doctest::Approx(0.0f));

	// And the answer reports the state read back out of the world.
	CHECK(patched.content["components"]["light"]["intensity"].get<float>()
	      == doctest::Approx(7.5f));

	// Re-found by uuid, not by the handle: a snapshot undo restores the whole
	// world and remaps every handle, so `lamp` may well name something else now.
	REQUIRE(f.snapshotUndo.undo());
	const Entity again = HE::Ed::entityByUuid(f.world, uuid);
	REQUIRE((again != entt::null));
	CHECK(f.world.registry().get<LightComponent>(again).intensity == doctest::Approx(1.0f));
}

TEST_CASE("Reparent and destroy move and remove whole subtrees")
{
	Fixture f;
	const Entity parentA = f.world.createEntity("A");
	const Entity parentB = f.world.createEntity("B");
	const Entity child   = f.world.createEntity("Child");
	REQUIRE(f.world.reparentEntity(child, parentA));

	const ToolResult moved = f.call("entity_reparent", json{
		{ "uuid",   f.uuid(child) },
		{ "parent", f.uuid(parentB) },
	});
	REQUIRE_MESSAGE(!moved.isError, codeOf(moved));
	CHECK(parentOf(f.world, child) == parentB);
	CHECK(moved.content["parent"] == f.uuid(parentB));

	// Back to the top level: an omitted parent is the world root, and the tool
	// reports that as an empty string rather than as the root's uuid.
	const ToolResult toTop = f.call("entity_reparent", json{ { "uuid", f.uuid(child) } });
	REQUIRE_MESSAGE(!toTop.isError, codeOf(toTop));
	CHECK(toTop.content["parent"] == "");

	REQUIRE(f.world.reparentEntity(child, parentB));
	const std::string childUuid = f.uuid(child);
	const ToolResult gone = f.call("entity_destroy", json{ { "uuid", f.uuid(parentB) } });
	REQUIRE_MESSAGE(!gone.isError, codeOf(gone));
	CHECK(gone.content["directChildren"].get<int>() == 1);
	CHECK((HE::Ed::entityByUuid(f.world, childUuid) == entt::null));
	CHECK(findByName(f.world, "A") == parentA);
}

TEST_CASE("entity_list is what makes a uuid findable in the first place")
{
	Fixture f;
	f.world.createEntity("Crate");
	const Entity lamp = f.world.createEntity("StreetLamp");
	f.world.registry().emplace<LightComponent>(lamp);

	const ToolResult all = f.call("entity_list");
	REQUIRE_MESSAGE(!all.isError, codeOf(all));
	CHECK(all.content["entities"].size() >= 2);

	const ToolResult byName = f.call("entity_list", json{ { "nameContains", "lamp" } });
	REQUIRE(!byName.isError);
	REQUIRE(byName.content["entities"].size() == 1);
	CHECK(byName.content["entities"][0]["name"] == "StreetLamp");
	CHECK(byName.content["entities"][0]["uuid"] == f.uuid(lamp));

	const ToolResult byComponent = f.call("entity_list", json{ { "component", "light" } });
	REQUIRE(!byComponent.isError);
	REQUIRE(byComponent.content["entities"].size() == 1);
	CHECK(byComponent.content["entities"][0]["uuid"] == f.uuid(lamp));

	// A clipped answer says so, rather than looking like a complete short one.
	const ToolResult clipped = f.call("entity_list", json{ { "limit", 1 } });
	REQUIRE(!clipped.isError);
	CHECK(clipped.content["entities"].size() == 1);
	CHECK(clipped.content["truncated"].get<int>() >= 1);
}

TEST_CASE("Refusals arrive under the code a client is supposed to branch on")
{
	Fixture f;
	const Entity crate = f.world.createEntity("Crate");
	// createEntity gives a name and a place in the hierarchy, not a transform —
	// the scene loader adds one, and so does entity_create.
	f.world.registry().emplace<TransformComponent>(crate);
	const std::string uuid = f.uuid(crate);

	SUBCASE("an unknown uuid")
	{
		const ToolResult r = f.call("entity_get", json{ { "uuid", "deadbeef" } });
		CHECK(codeOf(r) == "not_found");
	}

	SUBCASE("a missing uuid is a payload problem, not a lookup failure")
	{
		const ToolResult r = f.call("entity_destroy", json::object());
		CHECK(codeOf(r) == "invalid_payload");
	}

	SUBCASE("play mode refuses mutations and allows reads")
	{
		f.playing = true;
		const ToolResult r = f.call("entity_set_transform", json{
			{ "uuid",     uuid },
			{ "position", json::array({ 1.0, 1.0, 1.0 }) },
		});
		CHECK(codeOf(r) == "play_mode");
		CHECK(f.world.registry().get<TransformComponent>(crate).position.x == doctest::Approx(0.0f));
		CHECK_FALSE(f.call("entity_get", json{ { "uuid", uuid } }).isError);
	}

	SUBCASE("a built-in entity")
	{
		const Entity sun = f.world.createEntity("Sun");
		f.world.registry().emplace<EnvironmentLightComponent>(sun);
		REQUIRE(f.world.isBuiltin(sun));
		const ToolResult r = f.call("entity_destroy", json{ { "uuid", f.uuid(sun) } });
		CHECK(codeOf(r) == "builtin");
		CHECK(f.world.registry().valid(sun));
	}

	SUBCASE("a component key the scene loader does not restore")
	{
		const ToolResult r = f.call("entity_set_components", json{
			{ "uuid",  uuid },
			{ "patch", json{ { "sparkles", json{ { "amount", 3 } } } } },
		});
		CHECK(codeOf(r) == "invalid_payload");
		// The message has to name the key: "invalid payload" alone leaves a client
		// guessing which of the keys it sent was the wrong one.
		CHECK(r.errorMessage.find("sparkles") != std::string::npos);
	}

	SUBCASE("a create with an unknown component key changes nothing")
	{
		const ToolResult r = f.call("entity_create", json{
			{ "name",       "Ghost" },
			{ "components", json{ { "wobble", json::object() } } },
		});
		CHECK(codeOf(r) == "invalid_payload");
		CHECK((findByName(f.world, "Ghost") == entt::null));
	}

	SUBCASE("a set_transform that would change nothing")
	{
		const ToolResult r = f.call("entity_set_transform", json{ { "uuid", uuid } });
		CHECK(codeOf(r) == "invalid_payload");
	}

	SUBCASE("no scene open at all")
	{
		f.cmds.setWorld(nullptr);
		CHECK(codeOf(f.call("entity_list")) == "no_world");
		CHECK(codeOf(f.call("entity_get", json{ { "uuid", uuid } })) == "no_world");
		CHECK(codeOf(f.call("entity_create", json{ { "name", "X" } })) == "no_world");
	}
}

TEST_CASE("Session locks reach the client as two different answers")
{
	Fixture f;
	const Entity crate = f.world.createEntity("Crate");
	// createEntity gives a name and a place in the hierarchy, not a transform —
	// the scene loader adds one, and so does entity_create.
	f.world.registry().emplace<TransformComponent>(crate);
	const std::string uuid = f.uuid(crate);
	f.sessionOn = true;

	const json move{
		{ "uuid",     uuid },
		{ "position", json::array({ 5.0, 0.0, 0.0 }) },
	};

	SUBCASE("a peer holds it: no, and there is nothing to wait for")
	{
		f.lockedElsewhere.push_back(f.subjectFor(static_cast<std::uint32_t>(
			entt::to_integral(crate))));
		const ToolResult r = f.call("entity_set_transform", move);
		CHECK(codeOf(r) == "locked_by_other");
		CHECK(f.lockRequests.empty());
		CHECK(f.world.registry().get<TransformComponent>(crate).position.x == doctest::Approx(0.0f));
	}

	SUBCASE("nobody holds it: the lock is asked for and the client retries")
	{
		const ToolResult r = f.call("entity_set_transform", move);
		CHECK(codeOf(r) == "lock_pending");
		// The request went out — that is what makes "retry next frame" true rather
		// than an invitation to spin forever.
		CHECK(f.lockRequests.size() == 1);
		// And the message has to say so: a client that reads this as a hard failure
		// gives up one frame before it would have worked.
		CHECK(r.errorMessage.find("Retry") != std::string::npos);

		// Second attempt, with the grant arrived.
		f.ownedLocks = f.lockRequests;
		const ToolResult again = f.call("entity_set_transform", move);
		REQUIRE_MESSAGE(!again.isError, codeOf(again));
		CHECK(f.world.registry().get<TransformComponent>(crate).position.x == doctest::Approx(5.0f));
		// In a session an external edit is published, and it lands on the inverse
		// stack rather than as a whole-world snapshot.
		CHECK(f.transformPublishes == 1);
	}
}
