#include "McpToolRegistry.h"

#include "AssetStubWriter.h"
#include "EditorAssetTypeCache.h"

#include <ContentManager/AssetRefRetarget.h>
#include <ContentManager/AssetRefScan.h>
#include <ContentManager/ContentManager.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// ─── The files a project is made of, from outside the editor ─────────────────
// The rationale for the shape of this file is in McpToolRegistry.h beside
// McpAssetHooks (why not EditorCommands, why every path is confined). What is
// worth stating HERE is what the handlers do and do not do:
//
//   • A READER NEVER LOADS. asset_resolve and asset_list answer from
//     `idForPath` (what is already resident), the file's own META chunk
//     (`assetUuidOfFile`) and the editor's header-sniff cache. Not one of them
//     calls loadAsset — that registers the asset as a side effect, which moves
//     the dense asset pool and invalidates every pointer the editor is holding
//     at that moment. A question must not change the answer.
//
//   • THE STUB WRITER IS SHARED. asset_create calls the same
//     `HE::Ed::writeAssetStub` the Content Browser's create menu calls, so an
//     asset made over MCP is byte-for-byte the asset a human would have made.
//
//   • DELETE ASKS FIRST. `AssetRefs::findReferrers` is the same scan the delete
//     dialog runs, and its `incomplete` flag is honoured: a walk that broke down
//     is a LOWER BOUND, never a confident "nothing references this". The scan is
//     synchronous here — MCP handlers run on the frame thread (McpBridge.h) —
//     so `maxReferrers` is small: the answer a client needs is "is it
//     referenced", not a complete bibliography, and a full walk of a large
//     project would stall the editor's frame.
//
//   • IN A SESSION, DELETE AND MOVE ARE REQUESTS. The host answers by
//     broadcasting, so nothing moves locally until it does. The tools report
//     `applied: false, requested: true` rather than claiming a change they did
//     not make — the one lie that would be most expensive to a caller who cannot
//     see the screen.

namespace HE::Ed
{

using nlohmann::json;

namespace
{

// ── Small readers ────────────────────────────────────────────────────────────

std::string strArg(const json& args, const char* key)
{
	if (!args.is_object()) return {};
	const auto it = args.find(key);
	return (it != args.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

bool boolArg(const json& args, const char* key, bool fallback = false)
{
	if (!args.is_object()) return fallback;
	const auto it = args.find(key);
	return (it != args.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

int intArg(const json& args, const char* key, int fallback)
{
	if (!args.is_object()) return fallback;
	const auto it = args.find(key);
	return (it != args.end() && it->is_number_integer()) ? it->get<int>() : fallback;
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

json stringProp(const char* what)
{
	return json{ { "type", "string" }, { "description", what } };
}

// ── Confinement ──────────────────────────────────────────────────────────────
// The registry header promises an external client no file access. These tools
// take paths, so that promise is only as good as this function.
//
// Three separate refusals, because they are three different mistakes: a shape
// that is not a content-relative path at all, a path that RESOLVES outside every
// known root (which is what `..` buys), and the reserved "Engine/" namespace,
// which the Content Browser also refuses to mutate.
struct PathCheck
{
	std::string rel;        // normalised, forward slashes
	std::string abs;        // lexically normal, inside a known root
	bool        engine = false;   // under the reserved "Engine/" namespace
	bool        ok = false;
	ToolResult  failure = ToolResult::ok(json::object());
};

ToolResult failPath(const std::string& why)
{
	return ToolResult::fail("invalid_path", why);
}

// Is `child` inside `root`? Both already lexically normal and absolute.
bool isUnder(const std::filesystem::path& child, const std::filesystem::path& root)
{
	if (root.empty()) return false;
	auto c = child.begin();
	auto r = root.begin();
	for (; r != root.end(); ++r, ++c)
	{
		if (c == child.end()) return false;
		// The trailing empty element a path ending in a separator carries.
		if (r->empty() && ++decltype(r)(r) == root.end()) break;
		if (*c != *r) return false;
	}
	return true;
}

PathCheck checkPath(ContentManager& content, const std::string& raw, bool mustExist,
                    const char* argName)
{
	PathCheck p;
	if (raw.empty())
	{
		p.failure = failPath(std::string("'") + argName + "' is required: a "
		                     "content-relative asset path such as "
		                     "'Materials/Rock.hasset'.");
		return p;
	}

	// Windows separators arrive from clients that assemble paths with the host
	// OS in mind; stored references only ever use '/'.
	std::string rel = raw;
	std::replace(rel.begin(), rel.end(), '\\', '/');

	// An absolute path is refused as ONE, before anything is stripped off it.
	// Quietly dropping the leading slash would be the tempting leniency and it is
	// the wrong one twice over: '/Materials/Rock.hasset' (a client that meant the
	// content root) and '/Users/someone/.ssh/id_rsa' are the same string after
	// stripping, and the second one would come back as a plain "not found" — a
	// refusal that reads like "try a different absolute path".
	if (!rel.empty() && (rel.front() == '/' || (rel.size() >= 2 && rel[1] == ':')))
	{
		p.failure = failPath("'" + raw + "' is an absolute path. These tools address "
		                     "assets content-relative to the open project, e.g. "
		                     "'Materials/Rock.hasset' — drop the leading '/' if that "
		                     "is what was meant.");
		return p;
	}

	while (!rel.empty() && rel.back() == '/') rel.pop_back();
	if (rel.empty())
	{
		p.failure = failPath(std::string("'") + argName + "' is the content root "
		                     "itself, which is not an asset.");
		return p;
	}
	for (const auto& part : std::filesystem::path(rel))
	{
		if (part == "..")
		{
			p.failure = failPath("'" + rel + "' walks out of the content root with "
			                     "'..'. These tools reach the open project's content "
			                     "and nothing else on this machine.");
			return p;
		}
	}

	if (content.contentRoot().empty())
		return { {}, {}, false, false,
		         ToolResult::fail("no_project", "No project is open in the editor, so "
		                          "there is no content root to address. Call scene_info "
		                          "first.") };

	const std::string absRaw = content.resolveAbsolutePath(rel);
	if (absRaw.empty())
	{
		p.failure = failPath("'" + rel + "' does not resolve to anything under the "
		                     "project's content root.");
		return p;
	}

	std::error_code ec;
	std::filesystem::path abs = std::filesystem::path(absRaw).lexically_normal();

	// The check that actually keeps the promise: resolution is symbolic, so a
	// path can come back looking fine and still point outside. Both roots count —
	// "Engine/…" legitimately resolves into the shared engine content.
	const std::filesystem::path contentRoot =
		std::filesystem::path(content.contentRoot()).lexically_normal();
	const std::filesystem::path engineRoot =
		content.engineContentRoot().empty()
			? std::filesystem::path{}
			: std::filesystem::path(content.engineContentRoot()).lexically_normal();
	if (!isUnder(abs, contentRoot) && !(engineRoot.empty() ? false : isUnder(abs, engineRoot)))
	{
		p.failure = failPath("'" + rel + "' resolves outside the project's content "
		                     "root. These tools reach the open project's content and "
		                     "nothing else on this machine.");
		return p;
	}

	if (mustExist && !std::filesystem::exists(abs, ec))
	{
		p.failure = ToolResult::fail("not_found",
			"No file at '" + rel + "'. Use asset_list to see what is there; a path "
			"from another project or an earlier session does not carry over.");
		return p;
	}

	p.rel    = rel;
	p.abs    = abs.string();
	p.engine = rel.rfind("Engine/", 0) == 0;
	p.ok     = true;
	return p;
}

// The reserved namespace is read-only from a project's perspective — the same
// gate the Content Browser applies (`engineLocked`), and for the same reason:
// those files are the shipped engine defaults, shared by every project on this
// machine.
ToolResult failEngineReadOnly(const std::string& rel)
{
	return ToolResult::fail("read_only",
		"'" + rel + "' is in the reserved 'Engine/' namespace — the engine's shipped "
		"default content, shared by every project on this machine. The editor does not "
		"let a human create, delete or move anything there either. Save a copy under "
		"the project's own content instead.");
}

// ── What a file IS, without loading it ───────────────────────────────────────

std::string typeNameOf(const std::string& abs)
{
	// The header sniff, cached editor-wide. Unknown for a .hescene (JSON, not an
	// HAsset) and for anything that is not an asset at all.
	return HE::assetTypeName(EditorAssetTypeCache::assetTypeOf(abs));
}

// The [hi, lo] pair, and NOT a prettier string: this is exactly the shape
// SceneSerializer::uuidToJson writes and entity_set_components reads back, so
// the answer of asset_resolve can be pasted straight into a component field. A
// second spelling here would be a second thing to convert, in the one place a
// client is least able to notice it got it wrong.
json uuidJson(const HE::UUID& id)
{
	return json::array({ id.hi, id.lo });
}

// One entry of asset_list / the answer of asset_resolve. Deliberately the same
// object in both, so a client that learned the shape from one can read the
// other.
json assetEntry(ContentManager& content, const std::string& rel, const std::string& abs,
                bool isFolder)
{
	json j{
		{ "path",     rel },
		{ "name",     std::filesystem::path(abs).filename().string() },
		{ "isFolder", isFolder },
	};
	if (isFolder) return j;

	j["type"] = typeNameOf(abs);

	// The resident id first — that is the one every other tool will accept
	// today. The file's own META is the fallback, and the two agree by
	// construction (the loader takes the id from the same chunk).
	HE::UUID id = content.idForPath(rel);
	bool     unreadable = false;
	if (id == HE::UUID{}) id = HE::AssetRefs::assetUuidOfFile(abs, &unreadable);
	j["uuid"]   = uuidJson(id);
	j["loaded"] = !(content.idForPath(rel) == HE::UUID{}) && content.isLoaded(rel);
	// Stated rather than folded into a null uuid: "this file has no id" and "this
	// file could not be read" are different answers, and the second one is a
	// reason to look at the file rather than at the reference.
	if (unreadable) j["unreadable"] = true;

	std::error_code ec;
	const auto size = std::filesystem::file_size(abs, ec);
	if (!ec) j["sizeBytes"] = static_cast<std::uint64_t>(size);
	return j;
}

// ── The collaboration key ────────────────────────────────────────────────────

std::string collabKeyOf(const McpAssetHooks& h, ContentManager& content,
                        const std::string& abs, const std::string& rel, bool folder)
{
	if (h.collabKey) return h.collabKey(abs, folder);
	(void)content;
	return rel;
}

// The lock gate, asked once per mutating tool. Deliberately only the
// foreign-lock half, for the reason McpHcHooks::lockedByOther gives: the
// editor's asset policy is optimistic by design.
ToolResult* checkLocked(const McpAssetHooks& h, const std::string& key,
                        const std::string& rel, ToolResult& scratch)
{
	if (!h.lockedByOther || key.empty() || !h.lockedByOther(key)) return nullptr;
	scratch = ToolResult::fail("locked_by_other",
		"Another participant in the collaboration session is editing '" + rel +
		"' right now. Wait until they let go, or work on something else.");
	return &scratch;
}

ToolResult* checkPlaying(const McpAssetHooks& h, ToolResult& scratch)
{
	if (!h.isPlaying || !h.isPlaying()) return nullptr;
	scratch = ToolResult::fail("play_mode",
		"Play-in-editor is running. Creating, deleting or moving assets now would "
		"fight the running session for the same files, so it is refused rather than "
		"half-done. Ask the user to stop play mode.");
	return &scratch;
}

// ── The delete safety question ───────────────────────────────────────────────

json referrersJson(const HE::AssetRefs::ScanResult& scan)
{
	json arr = json::array();
	for (const HE::AssetRefs::Referrer& r : scan.referrers)
		arr.push_back(json{
			{ "path", r.displayPath.empty() ? r.absolutePath : r.displayPath },
			{ "via",  r.kind == HE::AssetRefs::RefKind::Uuid ? "uuid" : "path" },
		});
	return arr;
}

HE::AssetRefs::ScanResult scanReferrers(const McpAssetHooks& h, ContentManager& content,
                                        const std::string& rel, const std::string& abs)
{
	HE::AssetRefs::ScanTargets targets;
	targets.paths.push_back(rel);
	bool unreadable = false;
	const HE::UUID id = HE::AssetRefs::assetUuidOfFile(abs, &unreadable);
	if (!(id == HE::UUID{})) targets.uuids.push_back(id);

	HE::AssetRefs::ScanRequest req;
	req.contentRoot    = content.contentRoot();
	req.projectRoot    = h.projectRoot    ? h.projectRoot()    : std::string();
	req.contentDirName = h.contentDirName ? h.contentDirName() : std::string();
	req.excludeFiles.push_back(abs);
	// Small on purpose: this runs on the frame thread, and "is it referenced" is
	// the question — see the header.
	req.maxReferrers = 25;

	HE::AssetRefs::ScanResult scan = HE::AssetRefs::findReferrers(targets, req);
	// A file whose header could not be read is a target the scan then could not
	// look for, and scenes reference meshes and materials by id alone: the answer
	// would come back empty and confident.
	if (unreadable) scan.incomplete = true;
	return scan;
}

} // namespace

// ─── The tools ───────────────────────────────────────────────────────────────

void registerAssetTools(McpToolRegistry& registry, ContentManager& content,
                        McpAssetHooks hooks)
{
	ContentManager* cm = &content;
	auto h = std::make_shared<McpAssetHooks>(std::move(hooks));

	// ── asset_resolve ────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "asset_resolve";
		t.description =
			"Look up one asset by its content-relative path: its uuid, its type, "
			"whether the editor has it loaded and how big the file is. This is how a "
			"path becomes the uuid that entity_set_components and the component "
			"formats address assets by — the uuid comes back as the [hi, lo] pair "
			"those fields take, so it can be pasted straight in. Reads only: it never "
			"loads the asset, so asking costs nothing and changes nothing.";
		t.inputSchema = objectSchema(json{
			{ "path", stringProp("Content-relative path, e.g. "
			                     "'Materials/Rock.hasset' or 'Levels/Main.hescene'. "
			                     "'Engine/...' addresses the shared engine content.") },
		}, { "path" });
		t.handler = [cm, h](const json& args) -> ToolResult {
			const PathCheck p = checkPath(*cm, strArg(args, "path"), /*mustExist=*/true, "path");
			if (!p.ok) return p.failure;
			std::error_code ec;
			const bool folder = std::filesystem::is_directory(p.abs, ec);
			return ToolResult::ok(assetEntry(*cm, p.rel, p.abs, folder));
		};
		registry.add(std::move(t));
	}

	// ── asset_list ───────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "asset_list";
		t.description =
			"List what is in a content folder: every asset with its path, type and "
			"uuid, plus the sub-folders. The usual first call when looking for "
			"something to reference — a mesh to put on an entity, a material to "
			"assign, a HorizonCode class to spawn. Reads only, and loads nothing.";
		t.inputSchema = objectSchema(json{
			{ "path",      stringProp("Content-relative folder, e.g. 'Materials'. "
			                          "Omit for the content root itself.") },
			{ "recursive", json{ { "type", "boolean" },
			                     { "description", "Walk sub-folders too (default "
			                                      "false)." } } },
			{ "type",      stringProp("Only assets of this type, spelled as "
			                          "asset_resolve reports it: 'Material', "
			                          "'StaticMesh', 'HorizonCodeClass', 'Widget', "
			                          "'Texture', …") },
			{ "limit",     json{ { "type", "integer" }, { "minimum", 1 },
			                     { "description", "At most this many entries "
			                                      "(default 200)." } } },
		}, {});
		t.handler = [cm, h](const json& args) -> ToolResult {
			if (cm->contentRoot().empty())
				return ToolResult::fail("no_project",
					"No project is open in the editor, so there is no content to list. "
					"Call scene_info first.");

			std::string dirRel = strArg(args, "path");
			std::string dirAbs;
			if (dirRel.empty())
			{
				dirAbs = cm->contentRoot();
			}
			else
			{
				const PathCheck p = checkPath(*cm, dirRel, /*mustExist=*/true, "path");
				if (!p.ok) return p.failure;
				std::error_code dirEc;
				if (!std::filesystem::is_directory(p.abs, dirEc))
					return ToolResult::fail("invalid_path",
						"'" + p.rel + "' is a file, not a folder. asset_resolve reads a "
						"single asset; asset_list walks a directory.");
				dirRel = p.rel;
				dirAbs = p.abs;
			}

			const std::string wantType = strArg(args, "type");
			if (!wantType.empty() && assetTypeFromName(wantType) == HE::AssetType::Unknown)
				return ToolResult::fail("invalid_payload",
					"'" + wantType + "' is not an asset type. The spellings are the ones "
					"asset_resolve reports: Material, StaticMesh, SkeletalMesh, Texture, "
					"Widget, Theme, HorizonCodeClass, Script, InputAction, "
					"InputMappingContext, ParticleSystem, AnimatorStateMachine, BoneMask, "
					"BlendSpace, StructType, EnumType, SaveGameTemplate, Prefab, Scene, "
					"Audio, Font, AnimationClip.");

			const bool recursive = boolArg(args, "recursive");
			const int  limit     = std::max(1, intArg(args, "limit", 200));

			json entries = json::array();
			bool truncated = false;

			// Collected as (abs, isFolder) first so the walk cannot be disturbed by
			// what the per-entry work does, and sorted so two calls on an unchanged
			// folder answer identically — a client diffing its own earlier answer
			// should see the changes, not the directory order.
			std::vector<std::pair<std::string, bool>> found;
			std::error_code ec;
			auto take = [&](const std::filesystem::directory_entry& de) {
				std::error_code e;
				const bool isDir = de.is_directory(e);
				if (!isDir && !de.is_regular_file(e)) return;
				// The same rule GlobalState's content scan uses: dotfiles are
				// VCS/OS bookkeeping, not content anyone put there.
				if (de.path().filename().string().rfind('.', 0) == 0) return;
				found.emplace_back(de.path().lexically_normal().string(), isDir);
			};
			if (recursive)
			{
				std::filesystem::recursive_directory_iterator it(
					dirAbs, std::filesystem::directory_options::skip_permission_denied, ec);
				const std::filesystem::recursive_directory_iterator end;
				for (; !ec && it != end; it.increment(ec))
				{
					if (it->path().filename().string().rfind('.', 0) == 0)
					{
						std::error_code dirEc;
						if (it->is_directory(dirEc)) it.disable_recursion_pending();
						continue;
					}
					take(*it);
				}
			}
			else
			{
				std::filesystem::directory_iterator it(
					dirAbs, std::filesystem::directory_options::skip_permission_denied, ec);
				const std::filesystem::directory_iterator end;
				for (; !ec && it != end; it.increment(ec)) take(*it);
			}
			std::sort(found.begin(), found.end());

			for (const auto& [abs, isDir] : found)
			{
				const std::string rel = cm->toContentRelativePath(abs);
				if (rel.empty()) continue;   // outside every root — not addressable
				if (!wantType.empty() && (isDir || typeNameOf(abs) != wantType)) continue;
				if (static_cast<int>(entries.size()) >= limit) { truncated = true; break; }
				entries.push_back(assetEntry(*cm, rel, abs, isDir));
			}

			json out{
				{ "path",    dirRel },
				{ "entries", std::move(entries) },
			};
			if (truncated) out["truncated"] = true;
			// Not folded into an empty list: a walk that broke down half way is a
			// lower bound, and a client must not read it as "the folder is empty".
			if (ec) out["incomplete"] = true;
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}

	// ── asset_create ─────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "asset_create";
		t.description =
			"Create a new, empty asset at a content-relative path — the same file the "
			"Content Browser's Create Asset menu writes, with the same starter "
			"contents (a widget gets an empty tree, a theme the shipped default, an "
			"input action valid JSON). Only AUTHORED types can be created: meshes, "
			"textures, audio and fonts are imported, not made, and a scene is created "
			"with the scene tools. Missing parent folders are created. Refuses rather "
			"than picking a free name when the path is taken.";
		t.mutates     = true;
		t.inputSchema = objectSchema(json{
			{ "path", stringProp("Content-relative path for the new asset, e.g. "
			                     "'Materials/Rock.hasset'. A missing '.hasset' "
			                     "suffix is appended.") },
			{ "type", stringProp("What to create: Material, MaterialFunction, Widget, "
			                     "Theme, HorizonCodeClass, Script, InputAction, "
			                     "InputMappingContext, ParticleSystem, "
			                     "AnimatorStateMachine, BoneMask, BlendSpace, "
			                     "StructType, EnumType, SaveGameTemplate.") },
			{ "baseClass", stringProp("HorizonCodeClass only: the engine class it "
			                          "derives from ('Entity', 'PlayerController', "
			                          "'PlayerCharacter'). Omit for a plain Object. "
			                          "It decides which events the class can handle, "
			                          "so it cannot be changed as easily later.") },
		}, { "path", "type" });
		t.handler = [cm, h](const json& args) -> ToolResult {
			ToolResult scratch = ToolResult::ok(json::object());
			if (ToolResult* r = checkPlaying(*h, scratch)) return *r;

			const std::string typeName = strArg(args, "type");
			const HE::AssetType type   = assetTypeFromName(typeName);
			if (type == HE::AssetType::Unknown)
				return ToolResult::fail("invalid_payload",
					"'" + typeName + "' is not an asset type. See the 'type' description "
					"for the list this tool accepts.");
			if (!isCreatableAssetType(type))
				return ToolResult::fail("invalid_payload",
					"A " + typeName + " cannot be created empty. Meshes, textures, audio "
					"and fonts are IMPORTED from a file; a shader is generated from a "
					"material graph; a prefab is made from an entity subtree; a scene is "
					"created with the scene tools. What this tool makes is an authored "
					"asset with valid starter contents.");
			// The project's own gate, mirrored: a project without Advanced Shader
			// Effects has no Material row in the create menu, and one authored in
			// Lua has no HorizonCode row. MCP must not create what the menu will
			// not offer — the same rule the HorizonCode tools follow for node types.
			if (h->creatableTypes)
			{
				const std::vector<HE::AssetType> allowed = h->creatableTypes();
				if (std::find(allowed.begin(), allowed.end(), type) == allowed.end())
					return ToolResult::fail("invalid_payload",
						"This project does not offer " + typeName + " assets — the "
						"Content Browser's create menu does not list them either. The "
						"gate is usually the project's scripting language or its "
						"Advanced Shader Effects setting.");
			}

			std::string rawPath = strArg(args, "path");
			if (!rawPath.empty() && std::filesystem::path(rawPath).extension().empty())
				rawPath += ".hasset";
			const PathCheck p = checkPath(*cm, rawPath, /*mustExist=*/false, "path");
			if (!p.ok) return p.failure;
			if (p.engine) return failEngineReadOnly(p.rel);

			std::error_code ec;
			if (std::filesystem::exists(p.abs, ec))
				return ToolResult::fail("already_exists",
					"'" + p.rel + "' already exists. The Content Browser would pick the "
					"next free name; a client that named a path explicitly is more likely "
					"to have meant a different one, so nothing was overwritten. Choose "
					"another path, or delete that asset first.");

			const std::string key = collabKeyOf(*h, *cm, p.abs, p.rel, /*folder=*/false);
			if (ToolResult* r = checkLocked(*h, key, p.rel, scratch)) return *r;

			std::filesystem::create_directories(std::filesystem::path(p.abs).parent_path(), ec);

			AssetStubSpec spec;
			spec.scriptLanguage = h->scriptLanguage ? h->scriptLanguage()
			                                        : HE::ScriptLanguage::Lua;
			if (type == HE::AssetType::HorizonCodeClass)
				spec.horizonCodeBaseClass = strArg(args, "baseClass");

			const std::string name = std::filesystem::path(p.abs).stem().string();
			if (!writeAssetStub(p.abs, p.rel, name, type, spec))
				return ToolResult::fail("failed",
					"The file at '" + p.rel + "' could not be written. The folder may be "
					"read-only, or the disk full.");

			// A path that was probed while it was still free has a stale negative
			// entry in the shared type cache — this asset decides its type now.
			EditorAssetTypeCache::invalidate(p.abs);
			if (h->onAssetAppeared) h->onAssetAppeared(p.abs);
			// Published, not requested: nothing refers to a brand-new asset yet, so
			// there is nothing for the host to arbitrate.
			if (h->publishCreate) h->publishCreate(p.rel, p.abs);

			json out = assetEntry(*cm, p.rel, p.abs, /*isFolder=*/false);
			out["created"] = true;
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}

	// ── asset_delete ─────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "asset_delete";
		t.description =
			"Delete one asset. There is NO UNDO for this — the editor does not offer "
			"one to a human either — so the tool first runs the same reference scan "
			"the delete dialog runs and refuses when something still points at the "
			"asset, listing what does. Pass force=true to delete anyway and accept the "
			"broken references. In a collaboration session this becomes a request to "
			"the host: the answer says so, and nothing has been deleted yet.";
		t.mutates     = true;
		t.inputSchema = objectSchema(json{
			{ "path",  stringProp("Content-relative path of the asset to delete.") },
			{ "force", json{ { "type", "boolean" },
			                 { "description", "Delete even though something still "
			                                  "references it, or the scan could not "
			                                  "finish. Default false." } } },
		}, { "path" });
		t.handler = [cm, h](const json& args) -> ToolResult {
			ToolResult scratch = ToolResult::ok(json::object());
			if (ToolResult* r = checkPlaying(*h, scratch)) return *r;

			const PathCheck p = checkPath(*cm, strArg(args, "path"), /*mustExist=*/true, "path");
			if (!p.ok) return p.failure;
			if (p.engine) return failEngineReadOnly(p.rel);

			std::error_code ec;
			if (std::filesystem::is_directory(p.abs, ec))
				return ToolResult::fail("invalid_path",
					"'" + p.rel + "' is a folder. Deleting one takes everything under it "
					"with it, which is not something to do without seeing the list — do "
					"it in the Content Browser, which shows that list first.");

			const std::string key = collabKeyOf(*h, *cm, p.abs, p.rel, /*folder=*/false);
			if (ToolResult* r = checkLocked(*h, key, p.rel, scratch)) return *r;

			const bool force = boolArg(args, "force");
			if (!force)
			{
				const HE::AssetRefs::ScanResult scan = scanReferrers(*h, *cm, p.rel, p.abs);
				if (!scan.referrers.empty())
				{
					ToolResult fail = ToolResult::fail("has_referrers",
						std::to_string(scan.referrers.size()) +
						(scan.truncated ? " or more files" : " file(s)") +
						" still reference '" + p.rel + "' and would keep a broken "
						"reference. Retarget them first, or pass force=true if the "
						"breakage is intended.");
					fail.content = json{ { "referrers", referrersJson(scan) },
					                     { "truncated", scan.truncated } };
					return fail;
				}
				if (scan.incomplete)
					return ToolResult::fail("scan_incomplete",
						"The reference scan for '" + p.rel + "' could not finish, so an "
						"empty result is a lower bound rather than an answer: something "
						"may reference this asset without the scan having seen it. Pass "
						"force=true to delete regardless.");
			}

			// In a session this is the host's call, not ours: a delete breaks every
			// reference to the asset, which is as much somebody else's problem as
			// ours. Nothing moves locally until the host broadcasts.
			if (h->requestDelete && !key.empty() && h->requestDelete(key, /*folder=*/false))
				return ToolResult::ok(json{
					{ "path",      p.rel },
					{ "applied",   false },
					{ "requested", true },
					{ "note",      "A collaboration session is live, so this went to the "
					               "host as a request. The asset is still there; it "
					               "disappears on every machine at once when the host "
					               "approves. Check with asset_resolve." },
				});

			std::filesystem::remove(p.abs, ec);
			if (ec)
				return ToolResult::fail("failed",
					"'" + p.rel + "' could not be removed: " + ec.message());

			// The path is free again, and a NEW asset of a different type may be
			// created there next — a stale header sniff would name the dead one.
			EditorAssetTypeCache::invalidate(p.abs);
			if (h->onAssetGone) h->onAssetGone(p.abs);

			return ToolResult::ok(json{
				{ "path",    p.rel },
				{ "applied", true },
				{ "deleted", true },
			});
		};
		registry.add(std::move(t));
	}

	// ── asset_move ───────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "asset_move";
		t.description =
			"Move or rename one asset, carrying every stored reference over with it — "
			"materials naming their textures, meshes naming their material, "
			"HorizonCode graphs naming classes, and the asset's own embedded path. "
			"Renaming is a move within the same folder; there is no separate rename "
			"tool, because two would eventually disagree. In a collaboration session "
			"this becomes a request to the host: the answer says so, and nothing has "
			"moved yet.";
		t.mutates     = true;
		t.inputSchema = objectSchema(json{
			{ "path",    stringProp("Content-relative path of the asset to move.") },
			{ "newPath", stringProp("Its new content-relative path. Same folder with "
			                        "a different filename = a rename. The extension "
			                        "is carried over when the new path has none.") },
		}, { "path", "newPath" });
		t.handler = [cm, h](const json& args) -> ToolResult {
			ToolResult scratch = ToolResult::ok(json::object());
			if (ToolResult* r = checkPlaying(*h, scratch)) return *r;

			const PathCheck from = checkPath(*cm, strArg(args, "path"), /*mustExist=*/true, "path");
			if (!from.ok) return from.failure;
			if (from.engine) return failEngineReadOnly(from.rel);

			std::error_code ec;
			if (std::filesystem::is_directory(from.abs, ec))
				return ToolResult::fail("invalid_path",
					"'" + from.rel + "' is a folder. Moving one re-roots every reference "
					"below it, which is a bigger change than this tool is scoped for — "
					"do it in the Content Browser.");

			std::string rawTo = strArg(args, "newPath");
			// A client that renamed 'Rock.hasset' to 'Stone' meant 'Stone.hasset':
			// dropping the extension would leave a file no panel can open, and the
			// loader routes on the header rather than the name, so nothing would say
			// so until much later.
			if (!rawTo.empty() && std::filesystem::path(rawTo).extension().empty())
				rawTo += std::filesystem::path(from.abs).extension().string();
			const PathCheck to = checkPath(*cm, rawTo, /*mustExist=*/false, "newPath");
			if (!to.ok) return to.failure;
			if (to.engine) return failEngineReadOnly(to.rel);

			if (to.rel == from.rel)
				return ToolResult::fail("invalid_payload",
					"'newPath' is the path the asset already has. Nothing to do.");
			if (std::filesystem::exists(to.abs, ec))
				return ToolResult::fail("already_exists",
					"'" + to.rel + "' already exists. Nothing was overwritten — moving "
					"an asset onto another one would destroy the second without asking.");

			const std::string keyFrom = collabKeyOf(*h, *cm, from.abs, from.rel, false);
			const std::string keyTo   = collabKeyOf(*h, *cm, to.abs,   to.rel,   false);
			if (ToolResult* r = checkLocked(*h, keyFrom, from.rel, scratch)) return *r;

			// Same reasoning as the delete above: a rename breaks every reference to
			// the old name, so in a session the host decides and every machine moves
			// at once. Renaming here first would leave us out of step until the
			// answer, and out of step for good if the answer is no.
			if (h->requestMove && !keyFrom.empty() && !keyTo.empty() &&
			    h->requestMove(keyFrom, keyTo, /*folder=*/false))
				return ToolResult::ok(json{
					{ "path",      from.rel },
					{ "newPath",   to.rel },
					{ "applied",   false },
					{ "requested", true },
					{ "note",      "A collaboration session is live, so this went to the "
					               "host as a request. The asset is still at its old "
					               "path; it moves on every machine at once when the host "
					               "approves. Check with asset_resolve." },
				});

			std::filesystem::create_directories(std::filesystem::path(to.abs).parent_path(), ec);
			ec.clear();
			std::filesystem::rename(from.abs, to.abs, ec);
			if (ec)
				return ToolResult::fail("failed",
					"'" + from.rel + "' could not be moved to '" + to.rel + "': " +
					ec.message());

			// The two halves, and the split is not ours to change — see
			// McpAssetHooks::enqueueRetarget. In memory first: until it runs the
			// content manager still believes the asset lives at the old path, and
			// the very next save would write it back there.
			cm->retargetAssetReferencesInMemory(from.rel, to.rel, /*folder=*/false);
			if (h->enqueueRetarget) h->enqueueRetarget(from.rel, to.rel, /*folder=*/false);
			else                    cm->retargetAssetReferencesOnDisk(from.rel, to.rel, false);

			EditorAssetTypeCache::invalidate(from.abs);
			EditorAssetTypeCache::invalidate(to.abs);
			if (h->onAssetMoved) h->onAssetMoved(from.abs, to.abs, /*folder=*/false);

			json out = assetEntry(*cm, to.rel, to.abs, /*isFolder=*/false);
			out["previousPath"] = from.rel;
			out["applied"]      = true;
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}
}

} // namespace HE::Ed
