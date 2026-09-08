#include "McpToolRegistry.h"

#include <Diagnostics/Logger.h>

#include <algorithm>

namespace HE::Ed
{

using nlohmann::json;

bool McpToolRegistry::enforceNameRule(const std::string& name)
{
	if (name.empty() || name.size() > 64) return false;
	return std::all_of(name.begin(), name.end(), [](unsigned char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		       (c >= '0' && c <= '9') || c == '_' || c == '-';
	});
}

bool McpToolRegistry::add(McpTool tool)
{
	// Refusing loudly rather than registering something broken: a tool that
	// reaches a client without a schema, or under a name the client's API
	// rejects, fails at the far end where nothing in this repository is looking.
	if (!enforceNameRule(tool.name))
	{
		HE_LOG_ERROR(Editor,
		             "MCP tool '%s' rejected: a tool name may only contain letters, "
		             "digits, '_' and '-' (max 64) — a dot makes the tool unusable "
		             "for the client, which is where it would have failed",
		             tool.name.c_str());
		return false;
	}
	if (!tool.handler)
	{
		HE_LOG_ERROR(Editor, "MCP tool '%s' rejected: no handler", tool.name.c_str());
		return false;
	}
	if (!tool.inputSchema.is_object())
	{
		HE_LOG_ERROR(Editor, "MCP tool '%s' rejected: inputSchema must be a JSON "
		                     "Schema object — it is what the model reads before "
		                     "calling it",
		             tool.name.c_str());
		return false;
	}
	if (find(tool.name))
	{
		HE_LOG_ERROR(Editor, "MCP tool '%s' rejected: already registered", tool.name.c_str());
		return false;
	}

	m_tools.push_back(std::move(tool));
	return true;
}

const McpTool* McpToolRegistry::find(const std::string& name) const
{
	for (const auto& t : m_tools)
		if (t.name == name) return &t;
	return nullptr;
}

json McpToolRegistry::listPayload() const
{
	json tools = json::array();
	for (const auto& t : m_tools)
	{
		tools.push_back(json{
			{ "name",        t.name },
			{ "description", t.description },
			{ "inputSchema", t.inputSchema },
		});
	}
	return json{ { "tools", std::move(tools) } };
}

// ─── The two stub tools ──────────────────────────────────────────────────────

namespace
{

// A schema for a tool that takes nothing. Spelled out rather than omitted:
// clients validate against it, and "no properties" has to be stated to be
// enforced.
json emptySchema()
{
	return json{
		{ "type",       "object" },
		{ "properties", json::object() },
		{ "additionalProperties", false },
	};
}

} // namespace

void registerCoreTools(McpToolRegistry& registry, McpEditorHooks hooks)
{
	McpTool ping;
	ping.name        = "ping";
	ping.description = "Check that the Horizon editor is reachable and answering. "
	                   "Returns the editor's monotonic frame time in milliseconds.";
	ping.inputSchema = json{
		{ "type", "object" },
		{ "properties", json{
			{ "echo", json{ { "type", "string" },
			                { "description", "Optional text returned unchanged." } } },
		} },
		{ "additionalProperties", false },
	};
	ping.handler = [](const json& args) {
		json out{ { "pong", true } };
		// Echoed back verbatim so a client can correlate its own request without
		// relying on the JSON-RPC id surviving the shim.
		if (args.is_object() && args.contains("echo") && args["echo"].is_string())
			out["echo"] = args["echo"];
		return ToolResult::ok(std::move(out));
	};
	registry.add(std::move(ping));

	McpTool info;
	info.name        = "scene_info";
	info.description = "What the Horizon editor currently has open: project, scene "
	                   "path, whether it has unsaved changes, whether play-in-editor "
	                   "is running, whether a collaboration session is live, and how "
	                   "many entities the scene holds. Call this first — every other "
	                   "tool's answer depends on which scene is open.";
	info.inputSchema = emptySchema();
	info.handler = [hooks = std::move(hooks)](const json&) {
		// An empty hook is not an error: the listener starts with the editor and
		// the editor starts without a project. Reporting that honestly is what
		// keeps a client from addressing entities in a scene that is not there.
		const std::string scene = hooks.scenePath ? hooks.scenePath() : std::string();
		const int         count = hooks.entityCount ? hooks.entityCount() : -1;
		return ToolResult::ok(json{
			{ "project",     hooks.projectName ? hooks.projectName() : std::string() },
			{ "scene",       scene },
			{ "sceneOpen",   !scene.empty() },
			{ "dirty",       hooks.sceneDirty ? hooks.sceneDirty() : false },
			{ "playing",     hooks.isPlaying  ? hooks.isPlaying()  : false },
			{ "inSession",   hooks.inSession  ? hooks.inSession()  : false },
			{ "entityCount", count },
			{ "worldOpen",   count >= 0 },
		});
	};
	registry.add(std::move(info));
}

} // namespace HE::Ed
