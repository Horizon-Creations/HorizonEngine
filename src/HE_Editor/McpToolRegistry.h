#pragma once

// ─── What an external MCP client is allowed to ask for ───────────────────────
// The bridge (McpBridge.h) owns the socket, the handshake and the JSON-RPC
// envelope. This file owns the other half: the closed list of things that may be
// asked at all. A client can call exactly what is registered here and nothing
// else — no file access, no shell, no arbitrary engine call.
//
// Every tool carries its own JSON Schema, and that is not decoration: the shim
// is a pure passthrough (it lists what the editor lists and forwards what the
// client sends), so the schema IS the documentation the model reads before
// deciding what to send. A tool without one would be a tool nobody can call
// correctly. Registration refuses it.
//
// Names use underscores, never dots. The shim hands the name to the client
// verbatim, and the Anthropic Messages API only accepts ^[a-zA-Z0-9_-]{1,64}$ —
// `scene.info` would arrive at the model as an unusable tool. `enforceNameRule`
// is what keeps a later step from re-introducing the plan's dotted spelling.
//
// Deliberately free of ImGui, of SDL and of EditorApplication: the whole point
// of a registry is that a test can walk it, and this one can be walked without a
// window.

#include <Scripting/ScriptTypes.h>
#include <Types/Enums.h>

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class ContentManager;
namespace HorizonCode { struct Graph; }
namespace HE { struct UIWidgetTree; }

namespace HE::Ed
{

// What a handler gives back. Two shapes, because MCP has two: a result the model
// reads, and a failure the model is told to correct.
struct ToolResult
{
	nlohmann::json content = nlohmann::json::object();   // arbitrary JSON payload
	bool           isError = false;
	std::string    errorCode;      // machine-readable: not_found, play_mode, …
	std::string    errorMessage;

	static ToolResult ok(nlohmann::json j) { return ToolResult{ std::move(j), false, {}, {} }; }
	static ToolResult fail(std::string code, std::string message)
	{
		return ToolResult{ nlohmann::json::object(), true, std::move(code), std::move(message) };
	}
};

struct McpTool
{
	std::string    name;
	std::string    description;
	nlohmann::json inputSchema;   // JSON Schema, object type

	// Mutating tools are refused while play-in-editor runs, and are the ones
	// written to the console log. Reading tools are neither.
	bool mutates = false;

	std::function<ToolResult(const nlohmann::json& args)> handler;
};

class McpToolRegistry
{
public:
	// Returns false (and registers nothing) for a name that breaks the rule
	// above, a duplicate, a missing handler or a schema that is not an object.
	bool add(McpTool tool);

	const McpTool* find(const std::string& name) const;
	const std::vector<McpTool>& tools() const { return m_tools; }
	std::size_t size() const { return m_tools.size(); }
	void clear() { m_tools.clear(); }

	// The `tools/list` payload, in MCP's own shape:
	//   { "tools": [ { "name", "description", "inputSchema" }, … ] }
	// Shaped like the protocol rather than like us, because the shim forwards it
	// unchanged — anything invented here would have to be un-invented in the
	// shim later.
	nlohmann::json listPayload() const;

	// True when `name` may be handed to an MCP client as a tool name.
	static bool enforceNameRule(const std::string& name);

private:
	std::vector<McpTool> m_tools;
};

// ─── The scene the tools report on ───────────────────────────────────────────
// Everything the stub tools need from the editor, as functions rather than as an
// EditorApplication pointer — that class is not in the test binary, and the
// questions asked of it here are five predicates. Any hook may be empty; an
// absent one reads as "no project open", which is a real state (the listener
// runs before a scene is loaded).
struct McpEditorHooks
{
	std::function<std::string()> projectName;
	std::function<std::string()> scenePath;      // content-relative, empty = none
	std::function<bool()>        sceneDirty;
	std::function<bool()>        isPlaying;
	std::function<bool()>        inSession;
	std::function<int()>         entityCount;    // -1 = no world
};

// Register the two tools this step exists to prove the pipe with: `ping` (does
// the editor answer at all) and `scene_info` (does an answer carry real editor
// state, or just an echo). Entity and HorizonCode tools arrive in later steps
// through the same door.
void registerCoreTools(McpToolRegistry& registry, McpEditorHooks hooks);

class EditorCommands;

// ─── Placing and moving objects ──────────────────────────────────────────────
// The five mutating tools (create, destroy, reparent, set_transform,
// set_components) and the two reading ones without which they cannot be used at
// all: every address on this interface is a uuid, and a client that has no way
// to learn a uuid can only ever address what it made itself.
//
// Every mutation goes through `cmds.execute(…, Origin::External)` — there is no
// second path into the world from here, which is the whole reason the gateway
// exists. Refusals keep the gateway's own wire names (`not_found`, `play_mode`,
// `builtin`, `locked_by_other`, `lock_pending`, …), so the client reads the same
// vocabulary the editor uses internally.
//
// The reference is captured, so `cmds` has to outlive the registry. In the
// editor both are members of EditorApplication; in a test both are locals of the
// same fixture.
void registerEntityTools(McpToolRegistry& registry, EditorCommands& cmds);

// ─── Authoring HorizonCode ───────────────────────────────────────────────────
// The second half of what an external client can do to a project: read a visual
// script, add and remove nodes, wire and unwire pins, set parameters, save.
//
// Two rules shape this interface, and both come from what already exists:
//
//   • Every mutation is expressed in the ITEM-LEVEL JSON HorizonCode already
//     writes (`nodeToJson` / `variableToJson`, the four-int link array) and
//     applied through `CollabDocSync::forHorizonCodeGraph`'s adapter. That is
//     the same door a collaboration peer's edit comes through, so an MCP edit
//     cannot diverge from what a peer's edit does — and in a live session the
//     panel's own DocMirror diff picks the change up in the next frame and
//     publishes it, without this file knowing that collaboration exists.
//
//   • Wiring is the exception, and deliberately so: the adapter's link upsert
//     only checks that both endpoints exist, because a peer's link was already
//     validated on the peer. A client's is not, so `hc_connect` goes through
//     `HorizonCode::Graph::connect` (types, direction, occupancy) and falls back
//     to `connectWithConversion` — the same two steps the canvas takes when a
//     human drags a wire.
//
// Deliberately free of ImGui and of EditorApplication, like the two files above:
// a graph is a value, and "did that node land where the client asked" is a
// question a test can put to a `HorizonCode::Graph` on the stack.

// The two documents the editor owns itself rather than holding as an asset.
// Reserved words on this interface: no content-relative path can collide with
// them, because every asset path carries a '/' and a '.hasset' suffix.
inline constexpr const char* kMcpDocLevelScript  = "levelscript";
inline constexpr const char* kMcpDocGameInstance = "gameinstance";

// One addressable document. `key` is what every tool takes: the two words above
// for the graphs the editor owns, otherwise the content-relative path of a
// HorizonCode class asset.
struct McpHcDoc
{
	std::string key;
	std::string label;
	std::string kind;          // "level", "gameinstance", "class"
	bool        dirty = false;

	// The frontend's own restrictions, mirrored from HcGraphHost::MenuOpts so
	// that MCP cannot insert what the add menu refuses to offer. Both empty =
	// no restriction.
	//
	// Only the part of `addExcluded` that is a real restriction belongs here.
	// That list also holds types the palette merely offers through a DIFFERENT
	// route (Event through its own section, Get/Set Variable through the
	// sidebar), and mirroring those would refuse a client the very thing it
	// came for. See kLevelScriptExcluded in EditorApplication::setupMcpTools.
	std::vector<std::string> apiGroups;           // HE::api groups, e.g. "math"
	std::vector<std::string> excludedNodeTypes;   // stored node-type names
};

struct McpHcHooks
{
	// Every document a client may address right now. A class asset the editor
	// has never opened is deliberately absent rather than loaded on demand:
	// the panel owns that state, and a second copy loaded behind its back would
	// be the one edit the human's Save then throws away.
	std::function<std::vector<McpHcDoc>()> documents;

	// The live graph behind `key`, or null for an unknown one. The pointer is
	// used within the one call and never stored.
	std::function<HorizonCode::Graph*(const std::string& key)> resolve;

	// Around a mutation. `beginEdit` is where the editor takes whatever undo
	// entry fits the document (the level script gets a scene snapshot, exactly
	// as it does when a human edits it); `endEdit` marks the tab dirty and
	// re-registers the graph where something is running on it.
	std::function<void(const std::string& key)> beginEdit;
	std::function<void(const std::string& key)> endEdit;

	// Persist the document. False = the editor could not write it.
	std::function<bool(const std::string& key)> save;

	// Play-in-editor. Unlike the entity tools there is no gateway underneath
	// this file to refuse for us, so the check lives here.
	std::function<bool()> isPlaying;

	// Does a PEER of the collaboration session hold this document right now?
	// Same reason as the line above: there is no EditorCommands underneath these
	// tools to answer it, so the question is asked here — once, in `openDoc`,
	// for every mutating tool.
	//
	// Deliberately only the foreign-lock half of EditorCommands::checkLock. The
	// entity gateway also refuses `lock_pending` while its own claim is in
	// flight; documents do not, because the editor's asset policy is optimistic
	// by design (CollabController::beginAssetEdit — an asset edit that loses the
	// race is reloaded from disk, unlike a destroyed entity) and because nothing
	// here holds a document lock of its own to wait for.
	//
	// Absent hook, or no session, reads as "nobody else has it", which is the
	// truth outside a session: CollabController::assetLockedByOther answers
	// false when there is no session and excludes our own claim.
	std::function<bool(const std::string& key)> lockedByOther;
};

void registerHcTools(McpToolRegistry& registry, McpHcHooks hooks);

// ─── The files a project is made of ──────────────────────────────────────────
// Five tools — resolve, list, create, delete, move — and they are what turns the
// entity and HorizonCode tools from a demo into something usable: every one of
// those addresses assets (a mesh reference, a material, a class to spawn) by a
// path or a uuid the client has no other way to learn.
//
// ── Why these do NOT go through EditorCommands ───────────────────────────────
// The gateway knows five entity commands and records an undo entry for each.
// Assets have none: "deleting an asset is the one Content Browser operation with
// no undo" (AssetRefScan.h), and a rename is a filesystem move plus a rewrite of
// every referrer on disk. Teaching the gateway to invert that would be inventing
// an undo the editor itself does not offer — a behaviour change dressed as
// plumbing. So this file follows the McpHcHooks precedent instead: the checks a
// human gets from the Content Browser for free are asked here, once, through
// hooks, and the refusals keep the same wire names the entity tools use.
//
// ── The one rule ─────────────────────────────────────────────────────────────
// Everything below is confined to the content root. Every path argument is
// content-relative, is resolved through ContentManager::resolveAbsolutePath and
// is then checked to actually LAND inside a known root — the registry header
// promises an external client no file access, and `../../.ssh/id_rsa` is a path
// the content manager resolves quite happily.
struct McpAssetHooks
{
	// Play-in-editor. Like the HorizonCode tools and unlike the entity ones,
	// there is no gateway underneath to refuse for us.
	std::function<bool()> isPlaying;

	// Does a PEER hold this asset right now? Same question, same answer shape and
	// same optimistic asset policy as McpHcHooks::lockedByOther. The argument is
	// the COLLAB KEY, not the content-relative path — see collabKey below.
	std::function<bool(const std::string& collabKey)> lockedByOther;

	// The key a collaboration session addresses a file by. Not always the
	// content-relative path: a C++ class lives under <project>/Source, a sibling
	// of Content, and reading its empty content-relative path as "no session"
	// is how the Content Browser once turned a create and a delete into
	// local-only operations (ContentBrowserPanel::collabKeyFor). Absent hook =
	// the content-relative form, which is right for everything these tools touch.
	std::function<std::string(const std::string& absPath, bool isFolder)> collabKey;

	// ── Asking the host instead of doing it ──────────────────────────────────
	// Inside a session a delete or a rename is a REQUEST, not an act: it breaks
	// every reference to the old name, which is as much somebody else's problem
	// as ours, and the host answers by broadcasting so every machine moves at
	// once. Both return true when the session TOOK the request — in which case
	// nothing happens locally yet, and the tool says exactly that rather than
	// claiming a change it did not make.
	//
	// Absent hooks, or no session, answer false: just do it.
	std::function<bool(const std::string& collabKey, bool folder)> requestDelete;
	std::function<bool(const std::string& oldKey, const std::string& newKey,
	                   bool folder)>                               requestMove;

	// A create IS published, not requested — nothing refers to a brand-new asset
	// yet, so there is nothing for the host to arbitrate.
	std::function<void(const std::string& contentRel, const std::string& absPath)> publishCreate;

	// Carry every stored reference over to the new path. Two halves, and the
	// split is not ours to change: in-memory first (until it runs, the content
	// manager still believes the asset lives at the old path and the very next
	// save would write it back there), on-disk on the editor's single retarget
	// queue (two walks over one file lose one of the two rewrites). Absent hook =
	// the on-disk walk runs inline, which is what a test wants.
	std::function<void(const std::string& oldRel, const std::string& newRel,
	                   bool folder)> enqueueRetarget;

	// Which asset types may be created here. The Content Browser's create menu
	// gates on the project (no materials without Advanced Shader Effects, one
	// scripting language per project, a short flat list for an app project), and
	// the same principle as kLevelScriptExcluded applies: MCP must not create
	// what the menu will not offer. Absent hook = everything
	// `isCreatableAssetType` allows, which is what a test wants.
	std::function<std::vector<HE::AssetType>()> creatableTypes;

	// The project's scripting language, for a Script asset's CHUNK_SLNG. Absent
	// hook = Lua.
	std::function<HE::ScriptLanguage()> scriptLanguage;

	// The editor's own bookkeeping after a file appeared, moved or went away:
	// the type/thumbnail caches, tabs open on a path that is gone, the content
	// refresh. Absent outside the editor, where none of that exists.
	std::function<void(const std::string& absPath)> onAssetGone;      // deleted
	std::function<void(const std::string& absPath)> onAssetAppeared;  // created
	std::function<void(const std::string& oldAbs, const std::string& newAbs,
	                   bool folder)>                onAssetMoved;

	// Where the reference scan looks besides the content root: the .heproj
	// manifest and a project-root GameInstance.hcode. Empty = skip that half.
	std::function<std::string()> projectRoot;
	// The content root's directory NAME ("Content"), which adds the
	// project-relative rule form scene references use. Empty = skip that form.
	std::function<std::string()> contentDirName;
};

// The reference is captured, so `content` has to outlive the registry — in the
// editor both are members of EditorApplication.
void registerAssetTools(McpToolRegistry& registry, ContentManager& content,
                        McpAssetHooks hooks);

// ─── The scene as a file ─────────────────────────────────────────────────────
// Three tools — save, create, open — and they close the one gap that made every
// entity tool provisional: a client could place a hundred objects and had no way
// to make any of it outlast the session, no way to start a second level, and no
// way to move to one that already existed. `scene_info` told it the scene was
// dirty and offered nothing to do about that.
//
// ── Why these do NOT go through EditorCommands ───────────────────────────────
// The same answer as McpAssetHooks, and for a sharper reason. The gateway knows
// five entity commands and records an undo entry that inverts each. Opening a
// scene REPLACES the world and then clears the undo history outright
// (EditorApplication::openScene) — there is no inverse to record, and inventing
// one would be inventing an undo the editor itself does not offer. So the
// checks a human gets from the UI for free are asked here, once, through hooks,
// and the refusals keep the wire names the entity tools use.
//
// ── The guard that has to be replicated by hand ──────────────────────────────
// A human never reaches `openScene` directly: File > Open Scene goes through
// `requestGuarded` (EditorUI.cpp), which raises the save prompt when the scene
// is dirty. A client cannot see that prompt, so `scene_open` refuses a dirty
// scene with `dirty` unless it says `discard_changes: true`. Silently throwing
// away an hour of somebody's placement is the one lie that would be most
// expensive here, and it is invisible to the caller by construction.
//
// ── What is deliberately NOT here ────────────────────────────────────────────
//   • Additive load. It is a merge into the running world with its own physics
//     and undo rules (openSceneAdditive), which is a different question from
//     scene persistence.
//   • A "new empty scene" that discards the current world without writing it
//     anywhere. `scene_create` writes a file and can open it; there is no tool
//     whose only effect is to throw the open scene away.
//   • Refusing inside a collaboration session. A human is not refused either,
//     and a gate MCP has but the File menu does not would be a behaviour change
//     dressed as plumbing. The session state is reported in the result instead.
struct McpSceneHooks
{
	// Absolute path of the scene the editor world was last saved to or loaded
	// from. Empty = a new, never-saved scene, which is a real state.
	std::function<std::string()> currentScenePath;
	std::function<bool()>        sceneDirty;
	std::function<bool()>        isPlaying;
	std::function<bool()>        inSession;
	std::function<int()>         entityCount;   // -1 = no world
	std::function<std::string()> rootUuid;      // world root, "" = no world

	// Write the editor world to this absolute path / replace it with what is at
	// this absolute path. False = the editor could not. Both are the editor's
	// own members, so an MCP save takes the scene thumbnail and an MCP open
	// preloads the asset references exactly as the File menu's do.
	std::function<bool(const std::string& absPath)> saveScene;
	std::function<bool(const std::string& absPath)> openScene;

	// A file that was not there before. Same pair the asset tools use, and for
	// the same reason: a create IS published (nothing refers to a brand-new file
	// yet, so there is nothing to arbitrate), and the editor's own bookkeeping
	// has to hear that something appeared.
	std::function<void(const std::string& contentRel, const std::string& absPath)> publishCreate;
	std::function<void(const std::string& absPath)> onAssetAppeared;
};

// The reference is captured, so `content` has to outlive the registry.
void registerSceneTools(McpToolRegistry& registry, ContentManager& content,
                        McpSceneHooks hooks);

// ─── Shaping the ground ──────────────────────────────────────────────────────
// Four tools — info, heightmap, sculpt, paint — for the one component that
// `entity_get` and `entity_set_components` cannot usefully address.
//
// ── Why terrain needs tools of its own ───────────────────────────────────────
// A TerrainComponent's payload is two blobs: `sculptHeightsB64` (res² floats,
// 263k of them at the resolution the chunk builder snaps to) and
// `layerWeightsB64` (weightRes² RGBA texels). The generic component tools hand
// those to a client as base64 and take them back the same way, which is not an
// interface — it is the absence of one. A client cannot read a height out of it,
// cannot change one without re-encoding the whole field, and has no way at all
// to express "raise the ground here", which is what a landscape is edited by.
//
// So these four speak the vocabulary the Landscape mode speaks: a WORLD position,
// a brush radius and falloff, an operation. The maths is TerrainSculpt (heights)
// and TerrainPaint (layer weights) — the same functions the editor's own brushes
// are built from, so a client cannot produce a landscape the editor could not
// have produced by hand.
//
// ── Everything still goes through the gateway ────────────────────────────────
// Unlike the asset and scene tools, this file DOES go through EditorCommands: a
// terrain edit is a component change on an entity, which is exactly what
// `Command::setComponents` is, and routing it there is what buys undo, the
// publish to a collaboration session, the play-mode refusal and the lock gate
// without a second copy of any of them. The brush runs on a COPY of the
// component and the result is handed to the gateway as the component patch.
//
// ── The one thing done outside the gateway, and why ──────────────────────────
// TerrainComponent carries fields that are explicitly never serialised: the
// weightmap texture's uuid, the chunk grid the chunks were last built for, and
// the region-dirty rect. The gateway rebuilds the component from the scene JSON,
// which cannot carry them, so it lands with a fresh default: no weightmap texture
// (the next tick REGISTERS A SECOND ONE and the first leaks), no known chunk grid
// and `dirty`, i.e. a rebuild of all 64+ chunks for a brush dab of ten metres.
// So after the command lands those runtime fields are carried over onto the new
// component, and the dirty rect is set to the brush extent. That is a write to
// the world outside the gateway and it is deliberately the smallest one possible:
// it touches nothing a scene file, an undo entry or a peer would ever see.
struct McpTerrainHooks
{
	// Rebuild what the edit invalidated — in the editor,
	// TerrainSystem::updateTerrains with the content manager and the renderer,
	// the same call the Landscape brush makes at the end of a stroke. Absent in
	// a test, where there is neither and the component itself is the answer.
	std::function<void()> regenerate;
};

// The reference is captured, so `cmds` has to outlive the registry.
void registerTerrainTools(McpToolRegistry& registry, EditorCommands& cmds,
                          McpTerrainHooks hooks);

// ─── The widget tree of a UI Widget asset ────────────────────────────────────
// Eight tools — two readers (widget_tree, widget_types) and six that change a
// widget (add, remove, move, set_properties, set_anchor, save).
//
// ── Why a widget needs tools of its own ──────────────────────────────────────
// The same reason terrain does, one layer up. A UI Widget asset's whole content
// is one string, `UIWidgetAsset::treeJson`, and the asset tools can create that
// file and move it and delete it and cannot put a single button in it. Handing a
// client the JSON to rewrite would be the base64 mistake again with a nicer
// encoding: the ids are a numbering it would have to keep itself, sibling order
// is vector order, and a reparent that only writes `parentId` leaves the element
// wherever it happened to stand (see UIWidgetTree::moveElement).
//
// So these speak the vocabulary the Designer speaks: an element id, a parent, a
// place among its siblings, an anchor preset out of the sixteen UMG names, and
// properties BY NAME out of the very table the details panel draws
// (UIElement::allProperties). A client cannot author a widget the editor could
// not have authored by hand, and it cannot invent a property that does not
// exist — an unknown name is refused with the list rather than written nowhere,
// which is what `setPropAny` does with one.
//
// ── Two places a widget can live, and only one of them at a time ─────────────
// When a Designer tab HOLDS the asset, that tab's `UIWidgetTree` is the truth:
// the loaded asset is only a copy the tab refreshes on every edit, and the
// human's next Save writes the tab. So a mutation goes into the LIVE tree
// (hooks.liveTree) and is finished with `markEdited` — an undo snapshot and the
// dirty mark, exactly what the panel does after a human's edit. Nothing is
// written to disk: the tab now has unsaved changes, which is the truthful state,
// and `widget_save` is what writes them.
//
// When no tab holds it, there is no second copy to race with, so the tool edits
// `UIWidgetAsset::treeJson` through the ContentManager and writes the file at
// once. This is a deliberate DEVIATION from the McpHcHooks precedent, which
// refuses a class asset the panel has not opened. The refusal there guards
// against a second copy of a graph that the human's Save would overwrite; with
// no tab open that race cannot happen, and refusing would mean a client could
// create a widget with asset_create and then never put anything in it. The price
// is that the disk path has no undo — the same price the asset tools already pay
// (see McpAssetHooks) — and every mutating result says which path it took in
// `target`, so a client never has to guess whether its change is on disk.
//
// What the disk path does NOT do is publish to a collaboration session: an
// item-level widget edit is CollabDocSync's business and that adapter is bound
// to an open tab's mirror. Inside a session, work on an open tab.
struct McpWidgetHooks
{
	// Play-in-editor. Like the asset, scene and HorizonCode tools and unlike the
	// entity ones, there is no gateway underneath to refuse for us.
	std::function<bool()> isPlaying;

	// Does a PEER hold this asset right now? Same question, same shape and same
	// optimistic asset policy as McpHcHooks::lockedByOther. The argument is the
	// content-relative path, which is the collab key for anything under Content.
	std::function<bool(const std::string& contentRel)> lockedByOther;

	// The tree behind an open Designer tab, or null when this panel does not
	// hold the asset — which is the whole branch above. The pointer is used
	// within the one call and never stored.
	std::function<UIWidgetTree*(const std::string& contentRel)> liveTree;

	// After a mutation of that live tree: push the panel's undo snapshot, mark
	// the tab dirty and refresh the loaded asset so play-in-editor sees the
	// edit — UIEditorPanel::commitEdit, in one call, so an MCP edit and a
	// human's leave the tab in the same state.
	std::function<void(const std::string& contentRel)> markEdited;

	// Write an open tab to disk (UIEditorPanel::saveByContentPath). False = the
	// editor could not. Absent = there are no tabs, which is a test.
	std::function<bool(const std::string& contentRel)> save;

	// Does that tab have edits the file does not? Reported by every tool that
	// touched a live tree, so a client can see that its change is in the editor
	// and not yet on disk — and that widget_save is what finishes it.
	std::function<bool(const std::string& contentRel)> isDirty;
};

// The reference is captured, so `content` has to outlive the registry.
void registerWidgetTools(McpToolRegistry& registry, ContentManager& content,
                         McpWidgetHooks hooks);

// ─── What the game is played with ────────────────────────────────────────────
// Six tools — three readers (input_actions, input_mappings, input_bindable) and
// three writers (input_action_set, input_mapping_bind, input_mapping_unbind) —
// for the two asset types that decide whether a key does anything at all.
//
// ── Why input needs tools of its own ─────────────────────────────────────────
// Both assets are one JSON string (`InputActionAsset::json`,
// `InputMappingContextAsset::json`), so the asset tools can make the files and
// move them and delete them and cannot put a single key in them. And handing a
// client the JSON to write would be worse here than anywhere else, because of
// what the loader does with a name it does not know: `applyInputMappingContext`
// SKIPS it. Silently. A context whose every key name is misspelled loads
// without a complaint and produces a game that ignores the keyboard. A client
// cannot see that, so the vocabulary is checked HERE and a bad name is a
// refusal — `SDL_GetScancodeFromName`, `SDL_GetGamepadButtonFromString`,
// `HE::mouseButtonFromName`, `HE::axisSourceFromName` — and `input_bindable` is
// what a client reads the legal names out of in the first place.
//
// Three more traps the handlers exist to cover:
//
//   • THE ACTION'S VALUE TYPE DECIDES THE SHAPE OF ITS BINDINGS. A Button action
//     takes keys, pad buttons and mouse buttons; an Axis action takes axis rows;
//     an Axis 2D action takes them per component. Mixing them writes bindings
//     the runtime never reads (`mapAction` vs `mapAxis` vs `mapAxis2D`), so the
//     action is resolved first and the wrong shape is refused with the right one
//     spelled out.
//
//   • A 2D ENTRY WHOSE Y LIST IS STILL EMPTY MUST STILL ENCODE AS axesX. That is
//     not cosmetic: "axes" registers a ONE-dimensional mapping and
//     `axis2DValue()` then answers 0,0 forever (InputMappingModel.cpp says so at
//     length). `decodeMapping` leaves `MapEntry::valueType` at -1, so EVERY
//     entry's value type is re-resolved from its action before anything is
//     encoded — not just the entry a call touched, or the other entries of a
//     context would be quietly downgraded by a save that was about something
//     else.
//
//   • A MOUSE OR STICK ROW MUST NOT CARRY KEYS. The runtime reads a Key row's
//     pairs whatever the source says, so keys left on a row that became a stick
//     keep binding invisibly — the same rule the source combo enforces by
//     clearing them (InputMappingModel's `applyDetected`).
//
// ── Why an open tab is REFUSED here rather than edited ───────────────────────
// This is a deliberate deviation from the widget tools, and the reason is what
// the two panels keep. `UIEditorPanel` holds an undo stack per tab, so an MCP
// edit can land in the tab as an undoable snapshot and be indistinguishable from
// a human's. `InputAssetPanel::PanelState` holds no undo at all — just `dirty` —
// so there is no such thing as landing in that tab safely: writing into it would
// be an edit the human cannot take back, and writing the file behind it would be
// an edit the human's next Save silently reverts.
//
// So: with unsaved edits in an open Input Asset tab, a mutation is REFUSED with
// `dirty` and the message says to save or close the tab. With a CLEAN tab (or no
// tab) the file is written and the tab is told to re-read it — the same
// `reloadFromDisk` a collaboration peer's change goes through, which is the
// editor's own answer to "the file changed under an open tab". Nothing can be
// lost either way, and there is no `input_save`: these tools never leave
// something unsaved behind them.
struct McpInputHooks
{
	// Play-in-editor. Like the asset, scene, HorizonCode and widget tools and
	// unlike the entity ones, there is no gateway underneath to refuse for us.
	std::function<bool()> isPlaying;

	// Does a PEER hold this asset right now? Same question, same shape and same
	// optimistic asset policy as McpHcHooks::lockedByOther. The argument is the
	// content-relative path, which is the collab key for anything under Content.
	std::function<bool(const std::string& contentRel)> lockedByOther;

	// Does an open (or closed-but-remembered) Input Asset tab have edits the file
	// does not? That is the refusal above. Absent = there are no tabs, which is a
	// test.
	std::function<bool(const std::string& contentRel)> isDirty;

	// Tell that tab to re-read the file. TRUE when a tab was actually holding the
	// asset — which is how these tools report `reloadedInEditor` without needing a
	// second "is it open" question. Absent = no tabs.
	std::function<bool(const std::string& contentRel)> reloadFromDisk;
};

// The reference is captured, so `content` has to outlive the registry.
void registerInputTools(McpToolRegistry& registry, ContentManager& content,
                        McpInputHooks hooks);

// ─── What a surface looks like ───────────────────────────────────────────────
// Three tools — `material_info` (the list and the one material, the same split
// `terrain_info` uses), `material_set_param` and `material_create_instance`.
//
// ── Why a material needs tools of its own ────────────────────────────────────
// `asset_create` already makes a material FILE, and that file is a stub: no
// graph, no shader, no parameters. Everything that makes a material a material
// lives in `MaterialAsset::nodeGraphJson` and in the param block generated from
// it, and neither is reachable through a path-level tool. A client could assign
// a material to a mesh and had no way to learn what could be tuned on it, let
// alone tune it.
//
// ── The trap this file exists for: WHERE a parameter's value lives ───────────
// There are two answers and picking the wrong one writes a change that is
// reverted later, with nothing to see at the time:
//
//   • On a MASTER material the truth is the GRAPH. Param node values are the
//     defaults the codegen emits, and `MaterialEditorPanel::applyToMaterial`
//     rebuilds `shaderParamData` from them on EVERY edit, keeping nothing. So a
//     value written only into the param block survives until the human moves a
//     node, and then silently goes back. This tool writes BOTH: the Param
//     node's `p[]` and the slot.
//   • Writing only the node is the same mistake mirrored.
//     `ContentManager::regenerateMaterialFromGraph` snapshots the param block by
//     NAME before the codegen and restores it afterwards (values are edited
//     without recompiling, so the baked block may legitimately differ from the
//     node defaults) — so the slot write has to come FIRST, or the regenerate
//     puts the old value back over the new default.
//   • On an INSTANCE there is no graph at all. The truth is the param block plus
//     `instanceOverriddenParams`: a slot listed there keeps the instance's value,
//     every other one follows the parent on the next `syncMaterialInstance`. So
//     setting a value and NOT marking the override is a value the next sync eats.
//
// A Param node declared inside a material FUNCTION this graph calls is a third
// case, and the honest one: its slot can be set, but no node of this graph
// carries it, so the next edit in the Material Editor resets it to the
// function's default. The result says so (`graphDefaultUpdated: false`) instead
// of leaving the client to find out.
//
// ── Why an open tab is REFUSED rather than edited ────────────────────────────
// The same answer as the input tools, for a different reason than theirs. That
// panel has no undo; this one does (JSON graph snapshots). What it does not have
// is a reachable edit path: the tab's truth is its own `State::graph`, and
// landing an edit in it means `applyToMaterial` — regenerate, the inline
// cross-compile check, the preview invalidation, the instance propagation, the
// collaboration mirror — all of it behind an `AppContext` this file cannot have.
// A hook that exposed it would be a SECOND regenerate path to keep in step with
// the first. So: unsaved edits in an open Material Editor tab are refused with
// `dirty`; a clean tab is told to re-read the file, which is the same
// `reloadFromDisk` a collaboration peer's change goes through. There is no
// `material_save` — these tools never leave something unsaved behind them.
struct McpMaterialHooks
{
	// Play-in-editor. Like the asset, scene, HorizonCode, widget and input tools
	// and unlike the entity ones, there is no gateway underneath to refuse for us.
	std::function<bool()> isPlaying;

	// Does a PEER hold this asset right now? Same question, same shape and same
	// optimistic asset policy as McpHcHooks::lockedByOther. The argument is the
	// content-relative path, which is the collab key for anything under Content.
	std::function<bool(const std::string& contentRel)> lockedByOther;

	// Does an open (or closed-but-remembered) Material Editor tab have edits the
	// file does not? That is the refusal above. Absent = there are no tabs, which
	// is a test.
	std::function<bool(const std::string& contentRel)> isDirty;

	// Tell that tab to re-read the file. TRUE when a tab was actually holding the
	// asset — which is how these tools report `reloadedInEditor` without needing a
	// second "is it open" question. Absent = no tabs.
	std::function<bool(const std::string& contentRel)> reloadFromDisk;

	// Does THIS project author materials at all? The Content Browser's create menu
	// and its "Create Material Instance" row are both gated on the project's
	// Advanced Shader Effects setting, and the same rule as everywhere else
	// applies: MCP must not create what the menu will not offer. Absent = yes,
	// which is what a test wants.
	std::function<bool()> materialsAllowed;

	// A file that was not there before — the same pair the asset and scene tools
	// carry, for the same two reasons: a create IS published (nothing refers to a
	// brand-new file yet, so there is nothing for the host to arbitrate), and the
	// editor's own bookkeeping has to hear that something appeared.
	std::function<void(const std::string& contentRel, const std::string& absPath)> publishCreate;
	std::function<void(const std::string& absPath)> onAssetAppeared;
};

// The reference is captured, so `content` has to outlive the registry.
void registerMaterialTools(McpToolRegistry& registry, ContentManager& content,
                           McpMaterialHooks hooks);

} // namespace HE::Ed
