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

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace HorizonCode { struct Graph; }

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
};

void registerHcTools(McpToolRegistry& registry, McpHcHooks hooks);

} // namespace HE::Ed
