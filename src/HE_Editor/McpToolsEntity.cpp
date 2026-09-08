#include "McpToolRegistry.h"

#include "EditorCommands.h"
#include "StructuralSync.h"

#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// ─── Placing and moving objects from outside the editor ──────────────────────
// Seven tools: five that change the scene and two without which the five cannot
// be used, because every address here is a uuid and a client that cannot list or
// read one can only ever address what it created itself in the same breath.
//
// The one architectural rule of this file: nothing below writes to the world.
// Every mutation is a `Command` handed to `EditorCommands::execute(…,
// Origin::External)`, which is where undo, publishing to the session and the
// lock gate live. That is the reason the gateway was built before these tools
// and not after — a handler that reached into the registry itself would be the
// fourth wiring the whole step exists to avoid.
//
// Two consequences worth stating, because they look like omissions:
//
//   • There is no play-mode check here. `execute` refuses `Origin::External`
//     while play-in-editor runs, and a second check in front of it would be a
//     second place to forget one.
//   • There is no lock handling here either, for the same reason: `checkLock`
//     answers `locked_by_other` when a peer holds the subject and `lock_pending`
//     while our own request is still in flight. This file's job is to turn those
//     into a sentence a client can act on — "retry" versus "never".

namespace HE::Ed
{

using nlohmann::json;

namespace
{

// ── Refusals ─────────────────────────────────────────────────────────────────
// One sentence per code, and the sentences matter more than usual: they are the
// entire explanation the model gets. `lock_pending` in particular has to say
// "try again", because a client that reads it as a hard failure gives up one
// frame before it would have succeeded.
ToolResult failFor(CmdError e, const std::string& what)
{
	const std::string code = errorName(e);
	switch (e)
	{
	case CmdError::NoWorld:
		return ToolResult::fail(code, "No scene is open in the editor. Call scene_info "
		                              "first — until a project and a scene are loaded "
		                              "there is nothing to address.");
	case CmdError::NotFound:
		return ToolResult::fail(code, "No entity with uuid '" + what + "' in the open "
		                              "scene. Use entity_list to see what exists; a uuid "
		                              "from an earlier scene does not survive a reload.");
	case CmdError::Builtin:
		return ToolResult::fail(code, "'" + what + "' is a built-in entity (the "
		                              "environment sun and its kin). The editor does not "
		                              "let a human edit it either.");
	case CmdError::PlayMode:
		return ToolResult::fail(code, "Play-in-editor is running. Changes made now would "
		                              "be thrown away when it stops, so they are refused "
		                              "rather than lost. Ask the user to stop play mode.");
	case CmdError::InvalidPayload:
		return ToolResult::fail(code, "The payload could not be applied: " + what);
	case CmdError::LockedByOther:
		return ToolResult::fail(code, "Another participant in the collaboration session "
		                              "holds '" + what + "' right now. Wait until they "
		                              "let go, or work on something else.");
	case CmdError::LockPending:
		return ToolResult::fail(code, "The lock on '" + what + "' has been requested from "
		                              "the session host and the answer is still in "
		                              "flight. Retry next frame — this is a wait, not a "
		                              "refusal.");
	case CmdError::Failed:
		return ToolResult::fail(code, "The editor refused the operation on '" + what +
		                              "' (a reparent that would make a cycle, or a blob "
		                              "the scene loader could not read).");
	case CmdError::None:
		break;
	}
	return ToolResult::fail(code, what);
}

// ── Small readers ────────────────────────────────────────────────────────────

std::string strArg(const json& args, const char* key)
{
	if (!args.is_object()) return {};
	const auto it = args.find(key);
	return (it != args.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// A three-component vector, if the client sent one. Returns false for anything
// else — including a two-element array, which is the shape a client reaches for
// when it thinks in 2D and would otherwise leave one axis at whatever it was.
bool vec3Arg(const json& args, const char* key, float out[3])
{
	if (!args.is_object()) return false;
	const auto it = args.find(key);
	if (it == args.end() || !it->is_array() || it->size() != 3) return false;
	for (int i = 0; i < 3; ++i)
	{
		if (!(*it)[i].is_number()) return false;
		out[i] = (*it)[i].get<float>();
	}
	return true;
}

json vec3Json(const glm::vec3& v)
{
	return json::array({ v.x, v.y, v.z });
}

std::string parentUuidOf(HorizonWorld& world, Entity e)
{
	const Entity p = structParentOf(world.registry(), e);
	// The world root is a real entity but not an address: a client that got its
	// uuid could reparent to it, which is what `parent: null` already means, and
	// could try to destroy it. Reported as empty, which is the same thing the
	// tools accept for "top level".
	if (p == entt::null || p == world.rootEntity()) return {};
	return uuidOf(world, p);
}

std::string nameOf(HorizonWorld& world, Entity e)
{
	const auto* n = world.registry().try_get<NameComponent>(e);
	return n ? n->name : std::string();
}

// The component state of one entity as plain JSON. This is `serializeEntityComponents`
// read back through `from_cbor` — the exact object the `.hescene` writes, not a
// second description of the same thing. There is no second serializer in this
// file and there must not be one: a component added to the scene format would
// otherwise appear in saved scenes and be invisible over MCP.
json componentsOf(HorizonWorld& world, Entity e)
{
	SceneSerializer ser;
	const std::vector<std::uint8_t> cbor = ser.serializeEntityComponents(world, e);
	json j = json::from_cbor(cbor, /*strict=*/true, /*allow_exceptions=*/false);
	return j.is_object() ? j : json::object();
}

json entitySummary(HorizonWorld& world, Entity e)
{
	// Held in a named object, deliberately: `items()` on a temporary json returns
	// a proxy into an object that dies at the end of the full expression, and the
	// loop then walks freed memory. It crashes, but only once the entity carries
	// enough components for the object to leave the small-buffer case.
	const json comps = componentsOf(world, e);
	json keys = json::array();
	for (const auto& [k, v] : comps.items())
		if (k != "__name") keys.push_back(k);

	return json{
		{ "uuid",       uuidOf(world, e) },
		{ "name",       nameOf(world, e) },
		{ "parent",     parentUuidOf(world, e) },
		{ "components", std::move(keys) },
		// Reported rather than hidden: a client that cannot see the sun does not
		// understand why the entity count is one higher than its own list, and
		// tries to work out what it is missing.
		{ "builtin",    world.isBuiltin(e) },
	};
}

// ── Resolving an address ─────────────────────────────────────────────────────
// Two failure shapes, and they are not the same: no world at all is a state the
// client can wait out, an unknown uuid in an open world is a mistake it has to
// correct. `execute` would answer both correctly, but the tools need the handle
// before they can build a command, so the same two answers are given here.
struct Resolved
{
	HorizonWorld* world  = nullptr;
	Entity        entity = entt::null;
	bool          ok     = false;
	ToolResult    failure = ToolResult::ok(json::object());
};

Resolved resolve(EditorCommands& cmds, const json& args, const char* key = "uuid")
{
	Resolved r;
	r.world = cmds.world();
	if (!r.world)
	{
		r.failure = failFor(CmdError::NoWorld, {});
		return r;
	}
	const std::string uuid = strArg(args, key);
	if (uuid.empty())
	{
		r.failure = failFor(CmdError::InvalidPayload,
		                    std::string("'") + key + "' is required and must be an "
		                    "entity uuid as returned by entity_list or entity_create.");
		return r;
	}
	r.entity = entityByUuid(*r.world, uuid);
	if (r.entity == entt::null)
	{
		r.failure = failFor(CmdError::NotFound, uuid);
		return r;
	}
	r.ok = true;
	return r;
}

// ── Schemas ──────────────────────────────────────────────────────────────────

json uuidProp(const char* what)
{
	return json{ { "type", "string" }, { "description", what } };
}

json vec3Prop(const char* what)
{
	return json{
		{ "type",        "array" },
		{ "items",       json{ { "type", "number" } } },
		{ "minItems",    3 },
		{ "maxItems",    3 },
		{ "description", what },
	};
}

json objectSchema(json properties, std::vector<std::string> required)
{
	json s{
		{ "type",       "object" },
		{ "properties", std::move(properties) },
		{ "additionalProperties", false },
	};
	if (!required.empty()) s["required"] = required;
	return s;
}

// The transform an entity has right now, as the nine floats a command carries.
// Read from the LOCAL TransformComponent, never from `worldMatrix`: that field
// is only as fresh as the last propagateTransforms, and an entity created in
// this very frame answers with the identity (this repository has been bitten by
// it four times, see docs and the header of TransformHierarchy.h).
bool currentTransform(HorizonWorld& world, Entity e, float out[9])
{
	const auto* tc = world.registry().try_get<TransformComponent>(e);
	if (!tc) return false;
	out[0] = tc->position.x; out[1] = tc->position.y; out[2] = tc->position.z;
	out[3] = tc->rotation.x; out[4] = tc->rotation.y; out[5] = tc->rotation.z;
	out[6] = tc->scale.x;    out[7] = tc->scale.y;    out[8] = tc->scale.z;
	return true;
}

json transformJson(const float v9[9])
{
	return json{
		{ "position", json::array({ v9[0], v9[1], v9[2] }) },
		{ "rotation", json::array({ v9[3], v9[4], v9[5] }) },
		{ "scale",    json::array({ v9[6], v9[7], v9[8] }) },
	};
}

// Every top-level key of a component object has to be one the scene loader
// restores. Without this an unknown key would be dropped in silence by
// applyComponents and the tool would report success for a write that never
// happened — the single most expensive kind of lie to a caller that cannot see
// the screen.
bool checkComponentKeys(const json& comps, std::string& badKey)
{
	for (const auto& [k, v] : comps.items())
	{
		if (!SceneSerializer::isKnownComponentKey(k)) { badKey = k; return false; }
		if (!v.is_object() && k != "__name") { badKey = k; return false; }
	}
	return true;
}

} // namespace

// ─── The tools ───────────────────────────────────────────────────────────────

void registerEntityTools(McpToolRegistry& registry, EditorCommands& cmds)
{
	EditorCommands* c = &cmds;

	// ── entity_list ──────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "entity_list";
		t.description =
			"List the entities of the open scene: uuid, name, parent uuid and which "
			"components each one carries. Every other entity tool addresses an entity "
			"by the uuid returned here, so this is normally the first call after "
			"scene_info. Optional filters narrow the answer instead of paging it.";
		t.inputSchema = objectSchema(json{
			{ "nameContains", json{ { "type", "string" },
			                        { "description", "Case-insensitive substring of the "
			                                         "entity name." } } },
			{ "component",    json{ { "type", "string" },
			                        { "description", "Only entities carrying this "
			                                         "component key, e.g. 'mesh', "
			                                         "'light', 'rigidbody'." } } },
			{ "limit",        json{ { "type", "integer" }, { "minimum", 1 },
			                        { "description", "At most this many entities "
			                                         "(default 200)." } } },
		}, {});
		t.handler = [c](const json& args) -> ToolResult {
			HorizonWorld* world = c->world();
			if (!world) return failFor(CmdError::NoWorld, {});

			std::string needle = strArg(args, "nameContains");
			std::transform(needle.begin(), needle.end(), needle.begin(),
			               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			const std::string wantComponent = strArg(args, "component");
			int limit = 200;
			if (args.is_object() && args.contains("limit") &&
			    args["limit"].is_number_integer())
				limit = std::max(1, args["limit"].get<int>());

			// Collected first, walked second. Reading an entity's components goes
			// through try_get for every component type the scene format knows, and
			// a try_get for a type nothing in this world has yet CREATES its
			// storage — inside the loop that means adding pools to the registry
			// while iterating one of them, which is a crash and not a slow path.
			std::vector<Entity> all;
			for (auto e : world->registry().view<entt::entity>())
				all.push_back(e);

			json out      = json::array();
			int  truncated = 0;
			for (const Entity e : all)
			{
				// The world root is scaffolding, not content: it has no uuid worth
				// handing out and every top-level entity already reports itself as
				// parentless.
				if (e == world->rootEntity()) continue;
				json summary = entitySummary(*world, e);
				if (summary["uuid"].get<std::string>().empty()) continue;

				if (!needle.empty())
				{
					std::string name = summary["name"].get<std::string>();
					std::transform(name.begin(), name.end(), name.begin(),
					               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
					if (name.find(needle) == std::string::npos) continue;
				}
				if (!wantComponent.empty())
				{
					const json& keys = summary["components"];
					if (std::find(keys.begin(), keys.end(), json(wantComponent)) == keys.end())
						continue;
				}

				if (static_cast<int>(out.size()) >= limit) { ++truncated; continue; }
				out.push_back(std::move(summary));
			}

			return ToolResult::ok(json{
				{ "entities",  std::move(out) },
				// Named rather than silently cut: a client that filters on a common
				// component and gets exactly `limit` rows has no way to tell a full
				// answer from a clipped one.
				{ "truncated", truncated },
			});
		};
		registry.add(std::move(t));
	}

	// ── entity_get ───────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "entity_get";
		t.description =
			"Everything the editor knows about one entity: name, parent, its full "
			"component JSON (the same shape the scene file stores) and its world "
			"position. Read this before entity_set_components — the patch that tool "
			"takes is a partial merge into exactly this object.";
		t.inputSchema = objectSchema(json{
			{ "uuid", uuidProp("Entity uuid, from entity_list or entity_create.") },
		}, { "uuid" });
		t.handler = [c](const json& args) -> ToolResult {
			const Resolved r = resolve(*c, args);
			if (!r.ok) return r.failure;

			json comps = componentsOf(*r.world, r.entity);
			return ToolResult::ok(json{
				{ "uuid",       uuidOf(*r.world, r.entity) },
				{ "name",       nameOf(*r.world, r.entity) },
				{ "parent",     parentUuidOf(*r.world, r.entity) },
				{ "builtin",    r.world->isBuiltin(r.entity) },
				{ "components", std::move(comps) },
				// Composed by walking the parent chain on the spot. The transform
				// inside `components` is LOCAL, and for a child of a moved parent the
				// two differ — which is exactly the question a client asks when it
				// wants to place something next to an object.
				{ "worldPosition", vec3Json(HE::worldPositionOf(*r.world, r.entity)) },
			});
		};
		registry.add(std::move(t));
	}

	// ── entity_create ────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "entity_create";
		t.description =
			"Create a new entity in the open scene and return its uuid. The entity is "
			"placed under `parent` (top level when omitted) and gets a transform from "
			"`position`/`rotation`/`scale`. `components` sets any further component in "
			"the scene file's own JSON shape — read one off an existing entity with "
			"entity_get to see it. Undoable in the editor like any other change.";
		t.inputSchema = objectSchema(json{
			{ "name",     json{ { "type", "string" },
			                    { "description", "Display name in the outliner "
			                                     "(default 'Entity')." } } },
			{ "parent",   uuidProp("Uuid of the parent entity. Omit for top level.") },
			{ "position", vec3Prop("Local position [x, y, z]. Default [0, 0, 0].") },
			{ "rotation", vec3Prop("Local rotation as Euler angles in DEGREES "
			                       "[x, y, z]. Default [0, 0, 0].") },
			{ "scale",    vec3Prop("Local scale [x, y, z]. Default [1, 1, 1].") },
			{ "components", json{ { "type", "object" },
			                      { "description", "Further components, keyed exactly as "
			                                       "the scene file keys them ('mesh', "
			                                       "'light', 'rigidbody', …). A key the "
			                                       "scene loader does not know is "
			                                       "refused rather than dropped." } } },
		}, {});
		t.mutates = true;
		t.handler = [c](const json& args) -> ToolResult {
			HorizonWorld* world = c->world();
			if (!world) return failFor(CmdError::NoWorld, {});

			Entity parent = entt::null;
			const std::string parentUuid = strArg(args, "parent");
			if (!parentUuid.empty())
			{
				parent = entityByUuid(*world, parentUuid);
				if (parent == entt::null) return failFor(CmdError::NotFound, parentUuid);
			}

			json comps = json::object();
			if (args.is_object() && args.contains("components"))
			{
				if (!args["components"].is_object())
					return failFor(CmdError::InvalidPayload,
					               "'components' must be an object keyed by component name.");
				comps = args["components"];
			}
			std::string bad;
			if (!checkComponentKeys(comps, bad))
				return failFor(CmdError::InvalidPayload,
				               "'" + bad + "' is not a component the scene loader "
				               "restores — it would be dropped in silence. Read the "
				               "component keys off an existing entity with entity_get.");

			// The transform arguments win over whatever `components.transform` said,
			// and are written into the same object rather than beside it: two places
			// that can both name a position is a bug waiting for the first client
			// that fills in both.
			float v9[9] = { 0, 0, 0, 0, 0, 0, 1, 1, 1 };
			if (comps.contains("transform"))
			{
				const json& tr = comps["transform"];
				float tmp[3];
				if (vec3Arg(tr, "position", tmp)) { v9[0] = tmp[0]; v9[1] = tmp[1]; v9[2] = tmp[2]; }
				if (vec3Arg(tr, "rotation", tmp)) { v9[3] = tmp[0]; v9[4] = tmp[1]; v9[5] = tmp[2]; }
				if (vec3Arg(tr, "scale",    tmp)) { v9[6] = tmp[0]; v9[7] = tmp[1]; v9[8] = tmp[2]; }
			}
			float tmp[3];
			if (vec3Arg(args, "position", tmp)) { v9[0] = tmp[0]; v9[1] = tmp[1]; v9[2] = tmp[2]; }
			if (vec3Arg(args, "rotation", tmp)) { v9[3] = tmp[0]; v9[4] = tmp[1]; v9[5] = tmp[2]; }
			if (vec3Arg(args, "scale",    tmp)) { v9[6] = tmp[0]; v9[7] = tmp[1]; v9[8] = tmp[2]; }
			comps["transform"] = transformJson(v9);

			std::string name = strArg(args, "name");
			if (name.empty()) name = "Entity";

			// A one-entity prefab blob, in the shape buildSubtreeJson writes and
			// applyPrefabJson reads. Built here rather than by creating a scratch
			// entity somewhere and serialising it: the gateway's CreateSubtree takes
			// a blob, and a blob of one record is a handful of fields. No `parent`
			// key — a record without one IS the subtree root, and naming an outside
			// parent would make applyPrefabJson find no root and refuse everything.
			json blob{
				{ "version",  "1.1" },
				{ "entities", json::array({ json{
					{ "id",         0 },
					{ "name",       name },
					{ "components", std::move(comps) },
				} }) },
			};

			const Command cmd =
				Command::create(parent, json::to_cbor(blob), /*preserveIds=*/false);
			const Result res = c->execute(cmd, Origin::External);
			if (!res.ok())
				return failFor(res.error, parentUuid.empty() ? name : parentUuid);

			return ToolResult::ok(json{
				{ "uuid",   uuidOf(*world, res.root) },
				{ "name",   nameOf(*world, res.root) },
				{ "parent", parentUuidOf(*world, res.root) },
				{ "transform", transformJson(v9) },
			});
		};
		registry.add(std::move(t));
	}

	// ── entity_destroy ───────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "entity_destroy";
		t.description =
			"Delete an entity and everything parented under it. The whole subtree goes, "
			"so check entity_list for children first if that is not what you mean. "
			"Undoable in the editor.";
		t.inputSchema = objectSchema(json{
			{ "uuid", uuidProp("Uuid of the entity to delete.") },
		}, { "uuid" });
		t.mutates = true;
		t.handler = [c](const json& args) -> ToolResult {
			const Resolved r = resolve(*c, args);
			if (!r.ok) return r.failure;

			const std::string uuid = uuidOf(*r.world, r.entity);
			const std::string name = nameOf(*r.world, r.entity);
			// Counted before the apply, for the same reason the gateway captures the
			// blob there: afterwards the subtree cannot be walked.
			int subtreeSize = 1;
			if (const auto* h = r.world->registry().try_get<HierarchyComponent>(r.entity))
				subtreeSize += static_cast<int>(h->children.size());

			const Result res = c->execute(Command::destroy(r.entity), Origin::External);
			if (!res.ok()) return failFor(res.error, uuid);

			return ToolResult::ok(json{
				{ "destroyed",       uuid },
				{ "name",            name },
				{ "directChildren",  subtreeSize - 1 },
			});
		};
		registry.add(std::move(t));
	}

	// ── entity_reparent ──────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "entity_reparent";
		t.description =
			"Move an entity to a different parent, keeping its LOCAL transform. Omit "
			"`parent` to move it to the top level. A parent that sits inside the "
			"entity's own subtree is refused ('failed') — that would be a cycle.";
		t.inputSchema = objectSchema(json{
			{ "uuid",   uuidProp("Uuid of the entity to move.") },
			{ "parent", uuidProp("Uuid of the new parent. Omit or pass \"\" for the "
			                     "top level.") },
		}, { "uuid" });
		t.mutates = true;
		t.handler = [c](const json& args) -> ToolResult {
			const Resolved r = resolve(*c, args);
			if (!r.ok) return r.failure;

			Entity parent = entt::null;
			const std::string parentUuid = strArg(args, "parent");
			if (!parentUuid.empty())
			{
				parent = entityByUuid(*r.world, parentUuid);
				if (parent == entt::null) return failFor(CmdError::NotFound, parentUuid);
			}

			const std::string uuid = uuidOf(*r.world, r.entity);
			const Result res =
				c->execute(Command::reparent(r.entity, parent), Origin::External);
			if (!res.ok()) return failFor(res.error, uuid);

			return ToolResult::ok(json{
				{ "uuid",   uuid },
				{ "parent", parentUuidOf(*r.world, r.entity) },
			});
		};
		registry.add(std::move(t));
	}

	// ── entity_set_transform ─────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "entity_set_transform";
		t.description =
			"Move, rotate or scale an entity. Values are LOCAL — relative to the "
			"parent, which is what the editor's own transform fields show — and "
			"rotation is Euler angles in degrees. Every field is optional: what you "
			"leave out keeps its current value, so moving something without changing "
			"its rotation needs only `position`. Use entity_get's worldPosition when "
			"you need to reason about where something actually is.";
		t.inputSchema = objectSchema(json{
			{ "uuid",     uuidProp("Uuid of the entity to move.") },
			{ "position", vec3Prop("New local position [x, y, z]. Unchanged if omitted.") },
			{ "rotation", vec3Prop("New local rotation, Euler DEGREES [x, y, z]. "
			                       "Unchanged if omitted.") },
			{ "scale",    vec3Prop("New local scale [x, y, z]. Unchanged if omitted.") },
		}, { "uuid" });
		t.mutates = true;
		t.handler = [c](const json& args) -> ToolResult {
			const Resolved r = resolve(*c, args);
			if (!r.ok) return r.failure;

			float v9[9];
			if (!currentTransform(*r.world, r.entity, v9))
				return failFor(CmdError::InvalidPayload,
				               "that entity has no transform component, so there is "
				               "nothing to move.");

			float tmp[3];
			bool any = false;
			if (vec3Arg(args, "position", tmp)) { v9[0] = tmp[0]; v9[1] = tmp[1]; v9[2] = tmp[2]; any = true; }
			if (vec3Arg(args, "rotation", tmp)) { v9[3] = tmp[0]; v9[4] = tmp[1]; v9[5] = tmp[2]; any = true; }
			if (vec3Arg(args, "scale",    tmp)) { v9[6] = tmp[0]; v9[7] = tmp[1]; v9[8] = tmp[2]; any = true; }
			if (!any)
				return failFor(CmdError::InvalidPayload,
				               "none of 'position', 'rotation' or 'scale' was given as a "
				               "three-number array, so this call would change nothing.");

			const std::string uuid = uuidOf(*r.world, r.entity);
			const Result res =
				c->execute(Command::setTransform(r.entity, v9), Origin::External);
			if (!res.ok()) return failFor(res.error, uuid);

			json out = transformJson(v9);
			out["uuid"] = uuid;
			out["worldPosition"] = vec3Json(HE::worldPositionOf(*r.world, r.entity));
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}

	// ── entity_set_components ────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "entity_set_components";
		t.description =
			"Set component fields on an entity. `patch` is merged into the entity's "
			"component JSON (the shape entity_get returns): a component you do not "
			"mention is untouched, and a FIELD you do not mention inside a component "
			"you do mention keeps its current value. Read the entity with entity_get "
			"first to see the field names — a component key the scene loader does not "
			"know is refused rather than silently dropped.";
		t.inputSchema = objectSchema(json{
			{ "uuid",  uuidProp("Uuid of the entity to edit.") },
			{ "patch", json{ { "type", "object" },
			                 { "description", "Partial component JSON, e.g. "
			                                  "{\"light\": {\"intensity\": 2.0}}." } } },
		}, { "uuid", "patch" });
		t.mutates = true;
		t.handler = [c](const json& args) -> ToolResult {
			const Resolved r = resolve(*c, args);
			if (!r.ok) return r.failure;

			if (!args.contains("patch") || !args["patch"].is_object() ||
			    args["patch"].empty())
				return failFor(CmdError::InvalidPayload,
				               "'patch' must be a non-empty object keyed by component "
				               "name, e.g. {\"transform\": {\"position\": [0, 1, 0]}}.");
			const json& patch = args["patch"];

			std::string bad;
			if (!checkComponentKeys(patch, bad))
				return failFor(CmdError::InvalidPayload,
				               "'" + bad + "' is not a component the scene loader "
				               "restores — applying it would report success for a write "
				               "that never happened. Use entity_get to see the keys this "
				               "entity actually has.");

			// The merge happens against the CURRENT state of each named component,
			// not against the bare patch. `applyComponents` overwrites a whole
			// component from the object it is given, so sending {"light":
			// {"intensity": 2}} on its own would reset the light's colour and range
			// to whatever the component's defaults are. Merging first is what makes
			// "set one field" mean that.
			const json current = componentsOf(*r.world, r.entity);
			json write = json::object();
			for (const auto& [key, value] : patch.items())
			{
				if (!value.is_object())
				{
					write[key] = value;   // "__name", the only non-object member
					continue;
				}
				json merged = current.contains(key) && current[key].is_object()
				                  ? current[key] : json::object();
				merged.update(value);
				write[key] = std::move(merged);
			}

			const std::string uuid = uuidOf(*r.world, r.entity);
			const Result res = c->execute(
				Command::setComponents(r.entity, json::to_cbor(write)), Origin::External);
			if (!res.ok()) return failFor(res.error, uuid);

			return ToolResult::ok(json{
				{ "uuid",       uuid },
				// The state afterwards, read back out of the world rather than echoed
				// from the patch: a field the component clamped or ignored is visible
				// here and nowhere else.
				{ "components", componentsOf(*r.world, r.entity) },
			});
		};
		registry.add(std::move(t));
	}
}

} // namespace HE::Ed
