#include "McpToolRegistry.h"

#include "EditorAssetTypeCache.h"     // what a path holds, without loading it
#include "EditorCommands.h"           // the one door into the scene
#include "StructuralSync.h"           // structParentOf — the hierarchy read the editor shares
#include "McpToolCommon.h"            // the argument readers, the confinement rule, the walk

#include <ContentManager/AssetRefScan.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>

#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/NameComponent.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// ─── Placing an authored subtree from outside the editor ─────────────────────
// Why prefabs need tools of their own, why the placement rides in the blob and
// why `prefab_save` is not a gateway command: McpToolRegistry.h, beside
// McpPrefabHooks. What is worth stating HERE is what the handlers promise.
//
//   • NOTHING IS LOADED TO ANSWER, AND NOTHING IS LOADED TO PLACE. The payload
//     is read straight out of the file's PFAB chunk (`HAsset::Reader`), which is
//     byte for byte what `ContentManager::loadAsset` would have put into
//     `PrefabAsset::data`. Two reasons, and the second is the load-bearing one:
//     a question must not change its own answer (the same rule the input and
//     material readers follow), and a `PrefabAsset*` taken from the content
//     manager is a pointer into a dense vector that the NEXT load invalidates
//     together with the strings it owns. Reading the file has neither problem.
//
//   • THE ROOT OF A BLOB IS THE RECORD WITHOUT A "parent" KEY. That is not a
//     convention this file invented — `buildSubtreeJson` omits the key for the
//     root on purpose ("naming an outside parent would make applyPrefabJson find
//     no root and refuse everything") and `applyPrefabJson` reads it back the
//     same way. A blob with TWO parentless records is refused here rather than
//     placed, because the loader would make one of them the root and leave the
//     other standing at the top level of the scene with nothing to say so.
//
//   • ONLY THE AXES THE CLIENT SENT ARE PATCHED. A prefab's authored rotation
//     and scale are part of what was saved; overwriting them with defaults
//     because the call did not mention them would silently un-author it. Same
//     rule the viewport's drag-drop follows.
//
//   • A REFUSAL IS A NO-OP. Nothing is created, nothing is written, no name is
//     claimed at the session host.

namespace HE::Ed
{

using nlohmann::json;

namespace
{

// ── Refusals ─────────────────────────────────────────────────────────────────
// The gateway's codes, in this family's words. Copied rather than shared with
// McpToolsEntity/McpToolsTerrain for the reason those two already show: the code
// is the machine-readable half and it comes from `errorName`, while the sentence
// is the entire explanation the model gets and is worth writing for the call it
// is actually answering.
ToolResult failFor(CmdError e, const std::string& what)
{
	const std::string code = errorName(e);
	switch (e)
	{
	case CmdError::NoWorld:
		return ToolResult::fail(code, "No scene is open in the editor, so there is "
		                              "nowhere to place a prefab. Call scene_info first.");
	case CmdError::NotFound:
		return ToolResult::fail(code, "No entity with uuid '" + what + "' in the open "
		                              "scene. Use entity_list to see what exists; a uuid "
		                              "from an earlier scene does not survive a reload.");
	case CmdError::Builtin:
		return ToolResult::fail(code, "'" + what + "' is a built-in entity (the "
		                              "environment sun and its kin). It is not something "
		                              "the editor lets a human save as a prefab either.");
	case CmdError::PlayMode:
		return ToolResult::fail(code, "Play-in-editor is running. Anything placed now "
		                              "would be thrown away when it stops, so it is "
		                              "refused rather than lost. Ask the user to stop "
		                              "play mode.");
	case CmdError::InvalidPayload:
		return ToolResult::fail(code, what);
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
		return ToolResult::fail(code, "The editor refused to instantiate '" + what +
		                              "' — the payload is not a subtree the scene loader "
		                              "can read.");
	case CmdError::None:
		break;
	}
	return ToolResult::fail(code, what);
}

// The [hi, lo] pair, the same shape asset_resolve answers with.
json uuidJson(const HE::UUID& id)
{
	return json::array({ id.hi, id.lo });
}

// ── The transform arguments ──────────────────────────────────────────────────
// Spelled the way McpToolsEntity spells them, deliberately down to the refusal
// of a two-element array: a client that thinks in 2D and sends [x, z] would
// otherwise leave one axis wherever the prefab left it and have no way to see
// that it did. Kept here rather than moved into McpToolCommon because the
// argument READERS there are the ones the confinement rule needs; this pair has
// exactly two callers, and neither of them can be given a wrong answer by the
// other one's copy — they are eight lines of shape check with no state.
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

// ── The addressed prefab ─────────────────────────────────────────────────────
// The file, its uuid and its payload decoded to JSON. `blob` is the CBOR the
// gateway takes; `tree` is the same thing readable, which is what both the
// reporting and the root patch work on.
struct Prefab
{
	std::string           rel;
	std::string           abs;
	HE::UUID              id;
	std::vector<uint8_t>  blob;
	json                  tree;
	bool                  ok = false;
	ToolResult            failure = ToolResult::ok(json::object());
};

// The record every other record hangs off. Null when the blob has none (which
// the loader answers to with a bare "refused"), and `count` is what separates
// that from the two-roots case the caller has to refuse for a different reason.
json* rootRecordOf(json& tree, int& count)
{
	count = 0;
	json* found = nullptr;
	const auto it = tree.find("entities");
	if (it == tree.end() || !it->is_array()) return nullptr;
	for (json& e : *it)
	{
		if (!e.is_object() || e.contains("parent")) continue;
		++count;
		found = &e;
	}
	return found;
}

int entityCountOf(const json& tree)
{
	const auto it = tree.find("entities");
	return (it != tree.end() && it->is_array()) ? static_cast<int>(it->size()) : 0;
}

Prefab openPrefab(ContentManager& content, const json& args, const char* argName,
                  bool needPayload)
{
	Prefab p;
	const PathCheck c = checkPath(content, strArg(args, argName), /*mustExist=*/true, argName);
	if (!c.ok) { p.failure = c.failure; return p; }
	p.rel = c.rel;
	p.abs = c.abs;

	if (EditorAssetTypeCache::assetTypeOf(p.abs) != HE::AssetType::Prefab)
	{
		p.failure = ToolResult::fail("invalid_path",
			"'" + p.rel + "' is not a Prefab asset. asset_resolve reports what a path "
			"holds, prefab_info without arguments lists every prefab in the project, and "
			"prefab_save turns a subtree of the open scene into a new one.");
		return p;
	}

	// The file's own META id, read from the header — the same one
	// `asset_resolve` reports and the one a scene stores when it links an
	// instance back to its source.
	p.id = HE::AssetRefs::assetUuidOfFile(p.abs);

	if (needPayload)
	{
		HAsset::Reader r;
		const HAsset::Reader::Chunk* chunk = nullptr;
		if (r.open(p.abs)) chunk = r.findChunk(HAsset::CHUNK_PFAB);
		// An empty prefab is a real state — "Save as Prefab" is the only thing
		// that writes the chunk, so a hand-made file has none — and it is a
		// refusal rather than an empty placement, because there is nothing to
		// place and a client that got `ok` would believe otherwise.
		if (!chunk || chunk->data.empty())
		{
			p.failure = ToolResult::fail("invalid_payload",
				"'" + p.rel + "' carries no subtree (no PFAB chunk). A prefab file is "
				"written by 'Save as Prefab' in the Outliner or by prefab_save; a file "
				"without the payload is an empty file, not an empty prefab.");
			return p;
		}
		p.blob = chunk->data;
		p.tree = json::from_cbor(p.blob, /*strict=*/true, /*allow_exceptions=*/false);
		if (!p.tree.is_object() || entityCountOf(p.tree) == 0)
		{
			p.failure = ToolResult::fail("invalid_payload",
				"The payload of '" + p.rel + "' is not a readable entity subtree. The "
				"file is damaged; the editor would refuse to instantiate it too.");
			return p;
		}
	}
	p.ok = true;
	return p;
}

// What one record of a blob says about itself. Component KEYS rather than their
// contents, exactly like entity_list: a client that wants the values of one
// entity reads them off the placed instance with entity_get, and dumping a whole
// vehicle's components into the answer would bury the shape of the tree.
json recordJson(const json& rec)
{
	json keys = json::array();
	const auto comps = rec.find("components");
	if (comps != rec.end() && comps->is_object())
		for (const auto& [k, v] : comps->items())
		{
			(void)v;
			keys.push_back(k);
		}

	json j{
		{ "id",         rec.value("id", 0) },
		{ "name",       rec.value("name", std::string("Entity")) },
		{ "components", std::move(keys) },
	};
	// The blob links by uuid, and those uuids belong to the entities the prefab
	// was CAPTURED from — never to anything in the open scene. Reported as the
	// blob's own index, which is the only number that means something here.
	if (rec.contains("parent")) j["hasParent"] = true;
	return j;
}

// ── prefab_info ──────────────────────────────────────────────────────────────

void addInfo(McpToolRegistry& registry, ContentManager& content)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "prefab_info";
	t.description =
		"List the project's prefabs, or read one. Without 'path': every Prefab asset, "
		"sorted, with its uuid — the catalogue prefab_instantiate places from. With "
		"'path': what that prefab contains — how many entities, the name of its root "
		"and the tree beneath it with each entity's component keys. Nothing is loaded "
		"into the editor to answer this.";
	t.inputSchema = objectSchema(json{
		{ "path",  stringProp("Content-relative path of one prefab, e.g. "
		                      "'Prefabs/Lamp.hasset'. Omit for the catalogue.") },
		{ "limit", numberProp("Catalogue only: how many prefabs at most (default 200).") },
	}, {});
	t.handler = [cm](const json& args) -> ToolResult {
		if (!strArg(args, "path").empty())
		{
			Prefab p = openPrefab(*cm, args, "path", /*needPayload=*/true);
			if (!p.ok) return p.failure;

			int rootCount = 0;
			const json* root = rootRecordOf(p.tree, rootCount);

			json entities = json::array();
			for (const json& rec : p.tree["entities"])
				entities.push_back(recordJson(rec));

			json out{
				{ "path",        p.rel },
				{ "uuid",        uuidJson(p.id) },
				{ "name",        std::filesystem::path(p.rel).stem().string() },
				{ "entityCount", entityCountOf(p.tree) },
				{ "entities",    std::move(entities) },
			};
			out["root"] = root ? root->value("name", std::string("Entity")) : std::string();
			// Both damaged shapes are REPORTED here and refused by
			// prefab_instantiate, rather than being invisible until a placement
			// goes wrong: a reader that says "fine" about a file the writer will
			// refuse is the worst of the two answers.
			if (rootCount != 1) out["rootCount"] = rootCount;
			return ToolResult::ok(std::move(out));
		}

		int limit = intArg(args, "limit", 200);
		if (limit <= 0) limit = 200;
		bool truncated = false;
		json list = json::array();
		for (const ContentAsset& a :
		     walkContentAssets(*cm, { HE::AssetType::Prefab }, limit, truncated))
		{
			list.push_back(json{
				{ "path", a.rel },
				{ "name", std::filesystem::path(a.rel).stem().string() },
				{ "uuid", uuidJson(HE::AssetRefs::assetUuidOfFile(a.abs)) },
			});
		}
		json out{ { "prefabs", std::move(list) } };
		if (truncated) out["truncated"] = true;
		return ToolResult::ok(std::move(out));
	};
	registry.add(std::move(t));
}

// ── prefab_instantiate ───────────────────────────────────────────────────────

void addInstantiate(McpToolRegistry& registry, ContentManager& content,
                    EditorCommands& cmds)
{
	ContentManager* cm = &content;
	EditorCommands* c  = &cmds;
	McpTool t;
	t.name        = "prefab_instantiate";
	t.description =
		"Place a prefab in the open scene and return the uuid of the entity it became. "
		"The whole authored subtree comes with it — a lamp post arrives with its light, "
		"a vehicle with its wheels — under fresh identities, so the same prefab can be "
		"placed as often as you like. 'position'/'rotation'/'scale' override the "
		"prefab's own on the ROOT only; whatever you omit stays as it was authored. "
		"Undoable in the editor like any other change. This is a COPY that knows where "
		"it came from: later edits to the prefab asset do not reach it.";
	t.inputSchema = objectSchema(json{
		{ "path",     stringProp("Content-relative path of the prefab, e.g. "
		                         "'Prefabs/Lamp.hasset'. prefab_info lists them.") },
		{ "parent",   stringProp("Uuid of the entity to place it under. Omit for the "
		                         "top level.") },
		{ "name",     stringProp("Display name for the placed root. Omit to keep the "
		                         "prefab's own.") },
		{ "position", vec3Prop("Local position [x, y, z] of the root. Omit to keep the "
		                       "prefab's authored one.") },
		{ "rotation", vec3Prop("Local rotation as Euler angles in DEGREES [x, y, z]. "
		                       "Omit to keep the prefab's authored one.") },
		{ "scale",    vec3Prop("Local scale [x, y, z]. Omit to keep the prefab's "
		                       "authored one.") },
	}, { "path" });
	t.mutates = true;
	t.handler = [cm, c](const json& args) -> ToolResult {
		HorizonWorld* world = c->world();
		if (!world) return failFor(CmdError::NoWorld, {});

		Entity parent = entt::null;
		const std::string parentUuid = strArg(args, "parent");
		if (!parentUuid.empty())
		{
			parent = entityByUuid(*world, parentUuid);
			if (parent == entt::null) return failFor(CmdError::NotFound, parentUuid);
		}

		Prefab p = openPrefab(*cm, args, "path", /*needPayload=*/true);
		if (!p.ok) return p.failure;

		int rootCount = 0;
		json* root = rootRecordOf(p.tree, rootCount);
		if (!root)
			return failFor(CmdError::InvalidPayload,
				"Every record in '" + p.rel + "' names a parent, so the payload has no "
				"root and the scene loader would refuse the whole thing. The file is "
				"damaged.");
		if (rootCount > 1)
			return failFor(CmdError::InvalidPayload,
				"'" + p.rel + "' has " + std::to_string(rootCount) + " records without a "
				"parent, so it is " + std::to_string(rootCount) + " subtrees rather than "
				"one prefab. The editor would make one of them the root and leave the "
				"rest standing at the top level of the scene with nothing to say so, so "
				"it is refused instead.");

		// The patch, into the blob and before the command — a second
		// Command::setTransform afterwards would be a second undo entry and a
		// second publish for one placement.
		const std::string newName = strArg(args, "name");
		if (!newName.empty()) (*root)["name"] = newName;

		float v[3];
		json& comps = (*root)["components"];
		if (!comps.is_object()) comps = json::object();
		json& xf = comps["transform"];
		if (!xf.is_object()) xf = json::object();
		if (vec3Arg(args, "position", v)) xf["position"] = json::array({ v[0], v[1], v[2] });
		if (vec3Arg(args, "rotation", v)) xf["rotation"] = json::array({ v[0], v[1], v[2] });
		if (vec3Arg(args, "scale",    v)) xf["scale"]    = json::array({ v[0], v[1], v[2] });
		// A root with no transform at all and no override either: the loader would
		// give the entity no TransformComponent, and an entity in a scene without
		// one cannot be moved by anything afterwards. Seeded with the identity,
		// which is what `entity_create` writes for a caller that names nothing.
		if (!xf.contains("position")) xf["position"] = json::array({ 0.0, 0.0, 0.0 });
		if (!xf.contains("rotation")) xf["rotation"] = json::array({ 0.0, 0.0, 0.0 });
		if (!xf.contains("scale"))    xf["scale"]    = json::array({ 1.0, 1.0, 1.0 });

		const Command cmd =
			Command::create(parent, json::to_cbor(p.tree), /*preserveIds=*/false);
		const Result res = c->execute(cmd, Origin::External);
		if (!res.ok()) return failFor(res.error, res.error == CmdError::Failed
		                                         ? p.rel : parentUuid);

		const auto* n = world->registry().try_get<NameComponent>(res.root);
		json out{
			{ "uuid",        uuidOf(*world, res.root) },
			{ "name",        n ? n->name : std::string() },
			{ "prefab",      p.rel },
			{ "prefabUuid",  uuidJson(p.id) },
			{ "entityCount", entityCountOf(p.tree) },
		};
		const Entity ep = structParentOf(world->registry(), res.root);
		out["parent"] = (ep == entt::null || ep == world->rootEntity())
		                ? std::string() : uuidOf(*world, ep);
		return ToolResult::ok(std::move(out));
	};
	registry.add(std::move(t));
}

// ── prefab_save ──────────────────────────────────────────────────────────────

void addSave(McpToolRegistry& registry, ContentManager& content, EditorCommands& cmds,
             const std::shared_ptr<McpPrefabHooks>& h)
{
	ContentManager* cm = &content;
	EditorCommands* c  = &cmds;
	McpTool t;
	t.name        = "prefab_save";
	t.description =
		"Save an entity and everything under it as a new prefab asset, the same thing "
		"'Save as Prefab' does in the Outliner. The scene is not changed. Omit 'path' "
		"for 'Prefabs/<Name>.hasset', uniquified. An existing path is refused rather "
		"than overwritten. This is the only way to create a Prefab asset — asset_create "
		"refuses the type, because a prefab file without a subtree in it is an empty "
		"file rather than an empty prefab.";
	t.inputSchema = objectSchema(json{
		{ "uuid", stringProp("Uuid of the entity whose subtree becomes the prefab. Its "
		                     "children come with it.") },
		{ "path", stringProp("Content-relative path for the new asset, e.g. "
		                     "'Prefabs/Lamp.hasset'. A missing '.hasset' suffix is "
		                     "appended. Omit for the Outliner's own default.") },
	}, { "uuid" });
	t.mutates = true;
	t.handler = [cm, c, h](const json& args) -> ToolResult {
		// Not the gateway's check: this tool never calls execute(). During play the
		// world holds the PLAY session's state, so the prefab would be a snapshot of
		// something the user is about to throw away.
		if (h->isPlaying && h->isPlaying())
			return ToolResult::fail("play_mode",
				"Play-in-editor is running, so the scene holds the running session's "
				"state rather than the authored one. A prefab captured now would be a "
				"snapshot of something that is about to be thrown away. Ask the user to "
				"stop play mode.");

		HorizonWorld* world = c->world();
		if (!world) return failFor(CmdError::NoWorld, {});
		const std::string uuid = strArg(args, "uuid");
		if (uuid.empty())
			return failFor(CmdError::InvalidPayload,
				"'uuid' is required and must be an entity uuid as returned by "
				"entity_list or prefab_instantiate.");
		const Entity e = entityByUuid(*world, uuid);
		if (e == entt::null) return failFor(CmdError::NotFound, uuid);
		if (world->isBuiltin(e)) return failFor(CmdError::Builtin, uuid);
		// The Outliner offers "Save as Prefab" on everything except the world root,
		// and for a reason worth repeating: the root's subtree is the whole scene,
		// and its record would carry no parent for every top-level entity — the
		// two-roots blob prefab_instantiate refuses above.
		if (e == world->rootEntity())
			return failFor(CmdError::InvalidPayload,
				"That is the scene root. Its subtree is the entire scene, which is a "
				"scene rather than a prefab — scene_save writes one of those.");

		std::string name = "Prefab";
		if (const auto* n = world->registry().try_get<NameComponent>(e);
		    n && !n->name.empty())
			name = n->name;
		// Entity names are free text, and a '/' in one would reach saveAsset's
		// create_directories: "Arm/Left" would silently land in Prefabs/Arm. The
		// Outliner sanitises the same way.
		for (char& ch : name)
			if (ch == '/' || ch == '\\') ch = '_';

		std::string rawPath = strArg(args, "path");
		if (rawPath.empty())
		{
			// The Outliner's default, uniquified the Outliner's way.
			const std::string base = "Prefabs/" + name;
			rawPath = base + ".hasset";
			for (int k = 1; k < 1000; ++k)
			{
				const std::string abs = cm->resolveAbsolutePath(rawPath);
				std::error_code existsEc;
				if (abs.empty() || !std::filesystem::exists(abs, existsEc)) break;
				rawPath = base + std::to_string(k) + ".hasset";
			}
		}
		else if (std::filesystem::path(rawPath).extension().empty())
		{
			rawPath += ".hasset";
		}
		else if (std::filesystem::path(rawPath).extension() != ".hasset")
		{
			return ToolResult::fail("invalid_path",
				"'" + rawPath + "' does not end in '.hasset'. A prefab is an asset like "
				"any other; the suffix is not negotiable, and a wrong one is refused "
				"rather than corrected.");
		}

		const PathCheck dst = checkPath(*cm, rawPath, /*mustExist=*/false, "path");
		if (!dst.ok) return dst.failure;
		if (dst.engine) return failEngineReadOnly(dst.rel);
		std::error_code ec;
		if (std::filesystem::exists(dst.abs, ec))
			return ToolResult::fail("already_exists",
				"'" + dst.rel + "' already exists. Overwriting it would change every "
				"future placement of that prefab while leaving the ones already in "
				"scenes untouched, which is not something to do by accident. Choose "
				"another path, or omit 'path' for the next free name.");
		if (h->lockedByOther && h->lockedByOther(dst.rel))
			return ToolResult::fail("locked_by_other",
				"Another participant in the collaboration session holds '" + dst.rel +
				"' right now. Wait until they let go, or choose another path.");

		SceneSerializer ser;
		std::vector<uint8_t> blob = ser.serializeSubtree(*world, e);
		json tree = json::from_cbor(blob, /*strict=*/true, /*allow_exceptions=*/false);
		if (!tree.is_object() || entityCountOf(tree) == 0)
			return ToolResult::fail("failed",
				"The subtree under '" + uuid + "' could not be captured. An entity whose "
				"only children are engine-generated (a landscape's chunks, the "
				"environment's lights) has nothing a prefab could carry.");
		const int entityCount = entityCountOf(tree);

		std::filesystem::create_directories(std::filesystem::path(dst.abs).parent_path(), ec);

		// Written and THEN registered, in the Outliner's order and for both of its
		// reasons. Writing first because `saveAsset` mints the identity of a fresh
		// asset, and `registerRuntimeAsset` only mints one when there is none — so
		// this way the path→uuid entry and the uuid in the file agree, and a later
		// placement resolves the prefab without re-reading it. Registering at all
		// because an asset that lives only on disk is invisible to the editor until
		// the next content refresh, and one that lives only in the SlotMap never
		// reaches disk and is gone at shutdown.
		PrefabAsset prefab;
		prefab.type = HE::AssetType::Prefab;
		prefab.name = std::filesystem::path(dst.abs).stem().string();
		prefab.path = dst.rel;
		prefab.data = std::move(blob);
		if (!cm->saveAsset(prefab))
			return ToolResult::fail("failed",
				"Could not write '" + dst.rel + "'. A read-only file or a full disk is "
				"the usual cause; the editor log carries the reason.");
		const HE::UUID id = prefab.id;
		cm->registerPrefab(std::move(prefab));

		EditorAssetTypeCache::invalidate(dst.abs);
		if (h->onAssetAppeared) h->onAssetAppeared(dst.abs);
		// Published as a CREATE, which is what it is: the name is claimed at the
		// host, so two people saving an "Arm" at the same moment do not silently
		// overwrite one another (the uniquifier above only ever consults this
		// machine's disk).
		if (h->publishCreate) h->publishCreate(dst.rel, dst.abs);

		return ToolResult::ok(json{
			{ "path",        dst.rel },
			{ "uuid",        uuidJson(id) },
			{ "name",        std::filesystem::path(dst.abs).stem().string() },
			{ "entityCount", entityCount },
			{ "from",        uuid },
		});
	};
	registry.add(std::move(t));
}

} // namespace

void registerPrefabTools(McpToolRegistry& registry, ContentManager& content,
                         EditorCommands& cmds, McpPrefabHooks hooks)
{
	// Shared rather than copied into each handler, like the material tools: the
	// hooks hold std::functions that capture the editor, and one copy per handler
	// would be one chance per handler to let one go stale.
	auto h = std::make_shared<McpPrefabHooks>(std::move(hooks));
	addInfo(registry, content);
	addInstantiate(registry, content, cmds);
	addSave(registry, content, cmds, h);
}

} // namespace HE::Ed
