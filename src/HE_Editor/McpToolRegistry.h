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
//
// The per-family hooks and register functions are in McpToolHooks.h; this
// header is what the bridge needs and nothing more (EDT-1209).

#include "McpClientCameras.h"     // McpClientId, and the per-client camera table

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

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

	// An optional picture beside the JSON. MCP has a content type for exactly
	// this (`{type:"image", data:<base64>, mimeType}`), and a model reads an
	// image block as an image — base64 pasted into the text block would be a
	// wall of characters it cannot look at. The bridge appends the block AFTER
	// the text one and only when `imageBytes` is non-empty, so every tool
	// without a picture answers exactly as before. The batch tool carries only
	// `content` per slot; a picture asked for through it is dropped there, and
	// the screenshot tool's schema says so.
	std::vector<std::uint8_t> imageBytes;
	std::string               imageMime;   // "image/png"

	static ToolResult ok(nlohmann::json j) { return ToolResult{ std::move(j), false, {}, {}, {}, {} }; }
	static ToolResult fail(std::string code, std::string message)
	{
		return ToolResult{ nlohmann::json::object(), true, std::move(code), std::move(message), {}, {} };
	}
};

// Who is calling. The bridge numbers its connections (HE::Net::ConnectionId,
// a uint32 starting at 1 per listener; McpBridge.cpp asserts that
// McpClientId — declared in McpClientCameras.h — is the same type, so this
// header stays free of the Net headers as its top comment promises). Zero is
// the anonymous caller: a test that invokes a handler directly, or a tool that
// has no bridge behind it. The context is a struct rather than a bare id so
// that the next thing a tool needs to know about its caller lands here rather
// than in every handler signature.
struct McpCallContext
{
	McpClientId client = 0;
};

struct McpTool
{
	std::string    name;
	std::string    description;
	nlohmann::json inputSchema;   // JSON Schema, object type

	// Mutating tools are refused while play-in-editor runs, and are the ones
	// written to the console log. Reading tools are neither.
	bool mutates = false;

	// Two ways to be called, one of which every tool has. Most tools do not
	// care who is asking and take only the arguments. A tool that keeps state
	// PER CLIENT (the screenshot camera) takes the context as well; the bridge
	// and the batch tool prefer that one when it is set. `add` fills in the
	// plain `handler` for a context-only tool, so `find(name)->handler(args)`
	// works for every registered tool — as the anonymous client.
	std::function<ToolResult(const nlohmann::json& args)>                       handler;
	std::function<ToolResult(const McpCallContext& ctx, const nlohmann::json& args)> handlerCtx;

	// The call the bridge makes: with the caller's identity when the tool wants
	// it, without when it does not.
	ToolResult invoke(const McpCallContext& ctx, const nlohmann::json& args) const
	{
		return handlerCtx ? handlerCtx(ctx, args) : handler(args);
	}
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
	void clear() { m_tools.clear(); m_clientGone.clear(); }

	// A client's connection is gone — closed by the client, dropped by the
	// bridge, or taken down with the listener. Tools that keep per-client state
	// register here and drop that client's share; the bridge calls `notify`
	// for every connection it forgets, so the state dies with the connection
	// and a later connection that happens to get the same number starts clean.
	// Registered on the registry rather than on the bridge because the tools
	// see only the registry, and because a test can fire it without a socket.
	void addClientGoneHook(std::function<void(McpClientId)> fn);
	void notifyClientGone(McpClientId client) const;

	// Who is calling RIGHT NOW, for code a tool reaches without being handed
	// the context: the gateway's lock hook above all. The entity, prefab and
	// terrain tools go through EditorCommands, which knows commands and
	// subjects but not callers, and whose lock has to be booked to the client
	// that caused it (McpLockBook). Threading McpCallContext through every
	// command would be the same answer in forty places.
	//
	// Sound because a call is synchronous and on the frame thread: the bridge
	// opens a CallerScope around `invoke`, the batch tool's inner calls run
	// inside that same scope, and nothing a tool does outlives its call. 0
	// outside any call — the anonymous caller, as everywhere else.
	McpClientId callingClient() const { return m_callingClient; }

	class CallerScope
	{
	public:
		CallerScope(const McpToolRegistry& r, McpClientId client)
			: m_registry(r), m_previous(r.m_callingClient)
		{
			m_registry.m_callingClient = client;
		}
		~CallerScope() { m_registry.m_callingClient = m_previous; }
		CallerScope(const CallerScope&)            = delete;
		CallerScope& operator=(const CallerScope&) = delete;

	private:
		const McpToolRegistry& m_registry;
		McpClientId            m_previous;
	};

	// The `tools/list` payload, in MCP's own shape:
	//   { "tools": [ { "name", "description", "inputSchema" }, … ] }
	// Shaped like the protocol rather than like us, because the shim forwards it
	// unchanged — anything invented here would have to be un-invented in the
	// shim later.
	nlohmann::json listPayload() const;

	// True when `name` may be handed to an MCP client as a tool name.
	static bool enforceNameRule(const std::string& name);

private:
	std::vector<McpTool>                          m_tools;
	std::vector<std::function<void(McpClientId)>> m_clientGone;
	// Mutable: the registry is const where tools are looked up and invoked,
	// and who is calling is not part of what the registry IS.
	mutable McpClientId                           m_callingClient = 0;
};

// Standard base64 (RFC 4648, with padding) — what an MCP image block's `data`
// has to be. Here rather than in a tool because the BRIDGE is what encodes a
// ToolResult's imageBytes for the wire; a tool never sees base64.
std::string mcpBase64Encode(const std::uint8_t* bytes, std::size_t count);

} // namespace HE::Ed
