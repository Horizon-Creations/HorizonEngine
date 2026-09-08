#pragma once

// ─── The door an external MCP client knocks on ───────────────────────────────
// A local TCP listener inside the running editor that speaks JSON-RPC 2.0, so a
// Claude instance (or any other MCP client, through the stdio shim) can ask the
// editor what it has open and — from the next step on — place and move objects
// in it.
//
// Why a listener in the editor rather than the usual arrangement (the client
// starts the server as a child process): the editor is a GUI application a human
// started and is still using. It cannot be a child of a Claude process, and a
// second editor process would be looking at a different world. So the process
// that owns the scene owns the socket, and the shim is the child.
//
// Everything about the lifecycle is deliberately boring: the listener opens when
// the editor turns it on, closes when the editor turns it off, and dies with the
// object. `update(nowMs)` is pumped from the frame loop — one call, on the main
// thread, between the collaboration pump and the UI. Every request that arrived
// in a frame is answered in that same frame. No thread, no lock, no half-applied
// state between frames.
//
// The security model, in the order it bites (docs/mcp-editor-integration-plan.md
// §2.3):
//
//   1. Off by default. Nothing listens unless the editor is told to.
//   2. Loopback only. The bind address is 127.0.0.1, so a machine on the network
//      cannot reach the port at all — that is a kernel fact, not a check.
//   3. A token. On start a 32-byte random token goes into an endpoint file
//      (mode 0600) together with port and pid; on stop the file is deleted. The
//      first message on a connection MUST be `auth` with that token; anything
//      else, and the connection is dropped without an answer. Without it every
//      local process could edit the scene; with it, only one that may read the
//      user's own file.
//   4. A closed tool list (McpToolRegistry). No file access, no shell, no
//      arbitrary engine call.
//   5. Four connections, and a frame ceiling far below the transport's own.
//
// Free of ImGui, of SDL and of EditorApplication, so the handshake and the
// refusals are testable over a real socket without a window.

#include "McpToolRegistry.h"

#include <Net/TcpTransport.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace HE::Ed
{

class McpBridge
{
public:
	// Four is not a scaling limit, it is a blast-radius limit: one human runs
	// one or two clients, and anything beyond that is a mistake or a probe.
	static constexpr std::size_t   kMaxClients    = 4;
	// A component patch is the largest legitimate payload. Well below the
	// transport's 64 MiB, so an over-long prefix is refused before a byte of it
	// is buffered.
	static constexpr std::uint32_t kMaxFrameBytes = 4u * 1024u * 1024u;

	// The protocol version this editor speaks. Sent back on `auth` so a shim
	// built against an older editor can say so instead of failing oddly later.
	static constexpr int kProtocolVersion = 1;

	McpBridge() = default;
	~McpBridge();

	McpBridge(const McpBridge&)            = delete;
	McpBridge& operator=(const McpBridge&) = delete;

	// Where the endpoint file goes. Injected rather than read from
	// GlobalState::userDataDir() inside this class, so a test can point it at a
	// temporary directory — and so it is impossible to accidentally write it
	// into a project directory, which is what ends up in git.
	void setEndpointFile(std::filesystem::path p) { m_endpointFile = std::move(p); }
	const std::filesystem::path& endpointFile() const { return m_endpointFile; }

	// 0 = let the OS pick, which is the sensible default: the port is published
	// in the endpoint file anyway, so nobody has to know it in advance.
	void setPort(std::uint16_t port) { m_requestedPort = port; }

	McpToolRegistry&       registry()       { return m_registry; }
	const McpToolRegistry& registry() const { return m_registry; }

	// Open the listener, mint a token, write the endpoint file. Idempotent.
	bool start();
	// Close every connection, close the listener, delete the endpoint file.
	// Idempotent, and called by the destructor — the endpoint file must never
	// outlive the process that it points at.
	void stop();

	// The one call the frame loop makes: pushes the on/off decision through and
	// then pumps. Config-driven, exactly like m_collab.setLanDiscoveryEnabled —
	// so Preferences only ever writes the config and there is one direction of
	// travel rather than two places that can disagree about whether the bridge
	// is up.
	void setEnabled(bool on);
	bool isEnabled() const { return m_enabled; }

	void update(std::uint64_t nowMs);

	bool          isRunning() const { return m_transport != nullptr; }
	std::uint16_t port() const;
	// Connections that have authenticated. A half-open one that has not sent
	// `auth` yet is not a client.
	std::size_t   clientCount() const;
	// Every accepted connection, authenticated or not — what the kMaxClients
	// limit actually counts.
	std::size_t   connectionCount() const;
	const std::string& token() const { return m_token; }

	// How many handshakes have EVER succeeded, and who said it was them.
	//
	// clientCount() cannot answer "did somebody reach this editor just now": one
	// update() drains the whole event queue, so a client that connects,
	// authenticates and hangs up between two frames is born and buried inside a
	// single pump and the count never leaves zero. That is exactly what `claude
	// mcp get` does — start the shim, run the handshake, close it — so the Tool
	// Status check needs a number that only goes up. Compared before and after,
	// it is proof that the connection landed HERE and not in a second editor
	// holding the same endpoint file (plan §6.7).
	//
	// `lastAuthClient` is the `params.client` the shim sends ("he_mcp/1"); empty
	// for a client that did not name itself. Both are written on the frame thread
	// in update() and are only safe to read there.
	std::uint64_t      authCount() const { return m_authCount; }
	const std::string& lastAuthClient() const { return m_lastAuthClient; }

private:
	struct Client
	{
		bool          authed      = false;
		std::uint64_t connectedMs = 0;
	};

	void handleFrame(HE::Net::ConnectionId id, const std::vector<std::uint8_t>& data);
	void sendJson(HE::Net::ConnectionId id, const nlohmann::json& j);
	void sendError(HE::Net::ConnectionId id, const nlohmann::json& id_, int code,
	               const std::string& message);

	// The dispatcher. Returns the `result` object; sets `outError` instead when
	// the method failed in a way JSON-RPC has a code for.
	bool dispatch(HE::Net::ConnectionId id, const std::string& method,
	              const nlohmann::json& params, nlohmann::json& outResult,
	              int& outErrorCode, std::string& outErrorMessage);

	bool writeEndpointFile();
	void removeEndpointFile();

	std::unique_ptr<HE::Net::TcpTransport>                m_transport;
	std::unordered_map<HE::Net::ConnectionId, Client>     m_clients;
	McpToolRegistry                                       m_registry;

	std::filesystem::path m_endpointFile;
	std::string           m_token;
	std::uint16_t         m_requestedPort = 0;
	bool                  m_enabled       = false;
	std::uint64_t         m_nowMs         = 0;
	std::uint64_t         m_authCount     = 0;   // monotonic; never reset by stop()
	std::string           m_lastAuthClient;
};

} // namespace HE::Ed
