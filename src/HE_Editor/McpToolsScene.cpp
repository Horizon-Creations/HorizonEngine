#include "McpToolRegistry.h"

#include "AssetStubWriter.h"          // writeEmptySceneFile — the shared newborn-scene writer
#include "EditorAssetTypeCache.h"
#include "McpToolCommon.h"            // the argument readers and the ONE confinement rule

#include <ContentManager/ContentManager.h>

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

// ─── The scene as a file, from outside the editor ────────────────────────────
// The rationale for the shape of this file is in McpToolRegistry.h beside
// McpSceneHooks (why not EditorCommands, which guard is replicated by hand, what
// is deliberately absent). What is worth stating HERE is what the handlers
// actually promise:
//
//   • NOTHING IS SAVED THAT THE CLIENT DID NOT ASK FOR. scene_open does not
//     write the current scene first. It refuses instead, and says so, because a
//     client that wanted both can call scene_save and then scene_open — whereas
//     an automatic save the caller did not ask for cannot be undone.
//
//   • PLAY MODE IS REFUSED FOR ALL THREE. Not caution: play-in-editor runs IN
//     m_editorWorld after snapshotting it to a temp file (setPlayMode), so the
//     world a save would write during play is the world the player has been
//     moving around in. A "save" that persists three minutes of play-time
//     physics over the authored level is a data-loss bug that looks like a
//     success.
//
//   • THE EXTENSION IS NOT NEGOTIABLE. A scene is JSON at a .hescene path.
//     Missing it is helpful to fix (a client that said "Levels/Main" meant
//     "Levels/Main.hescene"), any OTHER extension is not — writing scene JSON
//     into a .hasset would make a file every panel in the editor misreads.
//
//   • A NEW SCENE FILE IS WRITTEN BY THE SAME FUNCTION THE CREATE MENU USES.
//     `HE::Ed::writeEmptySceneFile` serialises an empty world rather than
//     spelling out a literal, so an MCP scene and a human's are one file format
//     with one implementation.

namespace HE::Ed
{

using nlohmann::json;

namespace
{

constexpr const char* kSceneExt = ".hescene";

// ── The refusals shared by all three ─────────────────────────────────────────

ToolResult* checkPlaying(const McpSceneHooks& h, ToolResult& scratch)
{
	if (!h.isPlaying || !h.isPlaying()) return nullptr;
	scratch = ToolResult::fail("play_mode",
		"Play-in-editor is running. The editor plays IN the scene world after "
		"snapshotting it, so saving now would write the running session's state over "
		"the authored level, and opening now would throw that session away. Ask the "
		"user to stop play mode first.");
	return &scratch;
}

// A path argument that has to name a scene FILE. `.hescene` is appended when the
// argument carries no extension at all; anything else is refused rather than
// corrected, because a client that said '.hasset' meant a different kind of file
// and silently renaming its intent is worse than telling it.
struct ScenePath
{
	PathCheck  p;
	ToolResult failure = ToolResult::ok(json::object());
	bool       ok = false;
};

ScenePath scenePathArg(ContentManager& content, const json& args, bool mustExist)
{
	ScenePath s;
	std::string raw = strArg(args, "path");

	// Appended BEFORE the confinement check, so `mustExist` is asked about the
	// file that will actually be used rather than about the extensionless form.
	if (!raw.empty())
	{
		const std::filesystem::path asPath(raw);
		if (asPath.extension().empty()) raw += kSceneExt;
	}

	s.p = checkPath(content, raw, mustExist, "path");
	if (!s.p.ok)
	{
		s.failure = s.p.failure;
		return s;
	}
	if (std::filesystem::path(s.p.rel).extension() != kSceneExt)
	{
		s.failure = ToolResult::fail("invalid_path",
			"'" + s.p.rel + "' is not a scene. A scene is JSON at a '.hescene' path; "
			"'.hasset' files are assets and are addressed with the asset_* tools.");
		return s;
	}
	if (s.p.engine)
	{
		s.failure = failEngineReadOnly(s.p.rel);
		return s;
	}
	s.ok = true;
	return s;
}

// What every one of the three reports about where the editor now stands, so a
// client never has to call scene_info just to learn what its own call did.
json sceneState(const McpSceneHooks& h, ContentManager& content)
{
	json j = json::object();
	const std::string abs = h.currentScenePath ? h.currentScenePath() : std::string();
	// Content-relative, never the absolute path: the tools take content-relative
	// paths, so answering with one the caller cannot pass back would be an
	// address in a second vocabulary — and it would hand out where this machine
	// keeps its files.
	j["scenePath"]   = abs.empty() ? std::string() : content.toContentRelativePath(abs);
	j["dirty"]       = h.sceneDirty ? h.sceneDirty() : false;
	j["entityCount"] = h.entityCount ? h.entityCount() : -1;
	if (h.rootUuid)
	{
		const std::string root = h.rootUuid();
		if (!root.empty()) j["rootUuid"] = root;
	}
	if (h.inSession && h.inSession()) j["inSession"] = true;
	return j;
}

// The file was just written where nothing was: tell the session and the editor.
void announceNewFile(const McpSceneHooks& h, const std::string& rel, const std::string& abs)
{
	// A path that was probed while it was still free has a stale "unknown" entry
	// in the shared type cache — this file is the one that decides its type now.
	EditorAssetTypeCache::invalidate(abs);
	if (h.publishCreate)   h.publishCreate(rel, abs);
	if (h.onAssetAppeared) h.onAssetAppeared(abs);
}

} // namespace

// ─── The tools ───────────────────────────────────────────────────────────────

void registerSceneTools(McpToolRegistry& registry, ContentManager& content,
                        McpSceneHooks hooks)
{
	ContentManager* cm = &content;
	auto h = std::make_shared<McpSceneHooks>(std::move(hooks));

	// ── scene_save ───────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "scene_save";
		t.description =
			"Write the open scene to disk. Without 'path' this saves over the file the "
			"scene came from, which is what File > Save does; with one it is Save As, "
			"and the editor's open scene becomes that new file. This is the only way "
			"anything placed with the entity tools outlasts the session — they change "
			"the world in memory and nothing else.";
		t.mutates     = true;
		t.inputSchema = objectSchema(json{
			{ "path", stringProp("Content-relative destination, e.g. "
			                     "'Levels/Main.hescene'. Omit to save over the file "
			                     "the scene was loaded from. A missing '.hescene' is "
			                     "added.") },
		}, {});
		t.handler = [cm, h](const json& args) -> ToolResult {
			ToolResult scratch;
			if (ToolResult* r = checkPlaying(*h, scratch)) return *r;
			if (!h->saveScene)
				return ToolResult::fail("no_project",
					"No project is open in the editor, so there is no scene to save. "
					"Call scene_info first.");

			std::string rel, abs;
			bool isNewFile = false;

			if (args.is_object() && args.contains("path") && !strArg(args, "path").empty())
			{
				const ScenePath s = scenePathArg(*cm, args, /*mustExist=*/false);
				if (!s.ok) return s.failure;
				rel = s.p.rel;
				abs = s.p.abs;
				std::error_code ec;
				isNewFile = !std::filesystem::exists(abs, ec);
				// The containing folder has to be there. Created rather than
				// refused: 'Levels/Main.hescene' in a project with no Levels folder
				// is the ordinary first save, and the Save As dialog lets a human
				// make the folder in the same gesture.
				const std::filesystem::path parent = std::filesystem::path(abs).parent_path();
				if (!parent.empty() && !std::filesystem::exists(parent, ec))
				{
					std::error_code mkEc;
					std::filesystem::create_directories(parent, mkEc);
					if (mkEc)
						return ToolResult::fail("failed",
							"Could not create the folder for '" + rel + "': " + mkEc.message());
				}
			}
			else
			{
				abs = h->currentScenePath ? h->currentScenePath() : std::string();
				if (abs.empty())
					return ToolResult::fail("no_scene",
						"This scene has never been saved, so there is no file to save over. "
						"Pass 'path' — 'Levels/Main.hescene' — and this call becomes the "
						"Save As that gives it one.");
				rel = cm->toContentRelativePath(abs);
			}

			if (!h->saveScene(abs))
				return ToolResult::fail("failed",
					"The editor could not write '" + rel + "'. A read-only file or a full "
					"disk is the usual cause; the editor log carries the reason.");

			if (isNewFile) announceNewFile(*h, rel, abs);

			json out = sceneState(*h, *cm);
			out["path"]    = rel;
			out["created"] = isNewFile;
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}

	// ── scene_create ─────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "scene_create";
		t.description =
			"Create a new, empty scene file. It holds one entity, the World root, "
			"exactly like File > New Scene — no sky, no light, no camera; those are "
			"entities to add. By default this only writes the file and leaves the open "
			"scene alone; pass open=true to switch the editor to it, which needs the "
			"current scene to be saved first.";
		t.mutates     = true;
		t.inputSchema = objectSchema(json{
			{ "path", stringProp("Content-relative path for the new scene, e.g. "
			                     "'Levels/Arena.hescene'. A missing '.hescene' is "
			                     "added. Missing folders are created.") },
			{ "open", json{ { "type", "boolean" },
			                { "description", "Switch the editor to the new scene "
			                                 "afterwards (default false). Refused while "
			                                 "the open scene has unsaved changes." } } },
		}, { "path" });
		t.handler = [cm, h](const json& args) -> ToolResult {
			ToolResult scratch;
			if (ToolResult* r = checkPlaying(*h, scratch)) return *r;

			const ScenePath s = scenePathArg(*cm, args, /*mustExist=*/false);
			if (!s.ok) return s.failure;

			std::error_code ec;
			if (std::filesystem::exists(s.p.abs, ec))
				return ToolResult::fail("already_exists",
					"'" + s.p.rel + "' is already there. Pick another name, or open it "
					"with scene_open — this tool never writes over an existing scene.");

			const bool wantOpen = boolArg(args, "open");
			// Asked BEFORE the file is written. A create that succeeded and an open
			// that was then refused leaves the client holding a half-done request it
			// has to reason about; refusing the whole call leaves nothing to clean up.
			if (wantOpen && h->sceneDirty && h->sceneDirty())
				return ToolResult::fail("dirty",
					"The open scene has unsaved changes, and switching away from it would "
					"throw them out. Call scene_save first, or create without open=true "
					"and switch later with scene_open (which can be told to discard).");

			const std::filesystem::path parent = std::filesystem::path(s.p.abs).parent_path();
			if (!parent.empty() && !std::filesystem::exists(parent, ec))
			{
				std::error_code mkEc;
				std::filesystem::create_directories(parent, mkEc);
				if (mkEc)
					return ToolResult::fail("failed",
						"Could not create the folder for '" + s.p.rel + "': " + mkEc.message());
			}

			if (!writeEmptySceneFile(s.p.abs))
				return ToolResult::fail("failed",
					"Could not write '" + s.p.rel + "'. A read-only folder or a full disk "
					"is the usual cause.");

			announceNewFile(*h, s.p.rel, s.p.abs);

			bool opened = false;
			if (wantOpen)
			{
				if (!h->openScene)
					return ToolResult::fail("failed",
						"'" + s.p.rel + "' was created, but this editor cannot open scenes "
						"(no project). The file is on disk.");
				opened = h->openScene(s.p.abs);
				if (!opened)
					return ToolResult::fail("failed",
						"'" + s.p.rel + "' was created but could not be opened. The file is "
						"on disk; the editor log carries the reason.");
			}

			json out = sceneState(*h, *cm);
			out["path"]   = s.p.rel;
			out["opened"] = opened;
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}

	// ── scene_open ───────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "scene_open";
		t.description =
			"Switch the editor to another scene, replacing everything in the current "
			"one. Every entity uuid a previous call learned belongs to the old world "
			"and means nothing after this — list the new one with entity_list. Undo "
			"history is cleared, exactly as it is when a human opens a scene. Refused "
			"while the open scene has unsaved changes unless discard_changes is true.";
		t.mutates     = true;
		t.inputSchema = objectSchema(json{
			{ "path", stringProp("Content-relative scene to open, e.g. "
			                     "'Levels/Main.hescene'. asset_list with type 'Scene' "
			                     "finds them.") },
			{ "discard_changes",
			  json{ { "type", "boolean" },
			        { "description", "Throw away the open scene's unsaved changes "
			                         "(default false). A human gets a save prompt here; "
			                         "this is the answer to it, and it cannot be undone." } } },
		}, { "path" });
		t.handler = [cm, h](const json& args) -> ToolResult {
			ToolResult scratch;
			if (ToolResult* r = checkPlaying(*h, scratch)) return *r;
			if (!h->openScene)
				return ToolResult::fail("no_project",
					"No project is open in the editor, so there is no scene to switch. "
					"Call scene_info first.");

			const ScenePath s = scenePathArg(*cm, args, /*mustExist=*/true);
			if (!s.ok) return s.failure;

			if (!boolArg(args, "discard_changes") && h->sceneDirty && h->sceneDirty())
				return ToolResult::fail("dirty",
					"The open scene has unsaved changes and opening another one throws "
					"them out. Call scene_save first, or repeat this call with "
					"discard_changes=true if losing them is what was meant.");

			if (!h->openScene(s.p.abs))
				return ToolResult::fail("failed",
					"'" + s.p.rel + "' could not be loaded as a scene. A scene is JSON; a "
					"file with the right extension and something else inside reads as "
					"empty. The editor log carries the parse error, and the world is now "
					"empty rather than half-loaded.");

			json out = sceneState(*h, *cm);
			out["path"] = s.p.rel;
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}
}

} // namespace HE::Ed
