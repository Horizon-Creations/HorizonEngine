#include "McpBridge.h"

#include <Diagnostics/Logger.h>
#include <Hpak/Aes256Gcm.h>   // Hpak::randomBytes for the auth token

#include <cstdio>
#include <fstream>
#include <random>

#ifdef _WIN32
#	include <process.h>
#else
#	include <fcntl.h>
#	include <sys/stat.h>
#	include <unistd.h>
#endif

namespace HE::Ed
{

using nlohmann::json;
using HE::Net::ConnectionId;
using HE::Net::NetEvent;
using HE::Net::NetEventType;
using HE::Net::SendMode;

namespace
{

// JSON-RPC 2.0 standard codes. Kept as named constants because a client tells
// "I sent nonsense" (-32700/-32600) apart from "you do not have that" (-32601)
// apart from "the tool refused" (a ToolResult error), and collapsing them would
// leave it nothing to act on.
constexpr int kParseError     = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams  = -32602;

// Read a string field, or "" when it is absent OR present with another type.
//
// NOT nlohmann's value(key, default): that one THROWS on a type mismatch, so
// `{"method": 123}` — valid JSON, past the parser, from a peer that has not
// authenticated yet — would come out of the accessor as an exception in the
// editor's frame loop. Every field a stranger can set is read through here.
std::string strField(const json& j, const char* key)
{
	if (!j.is_object()) return {};
	const auto it = j.find(key);
	if (it == j.end() || !it->is_string()) return {};
	return it->get<std::string>();
}

// Fill with CSPRNG bytes, falling back to std::random_device when no crypto
// backend is compiled in — the same shape SessionDirectory uses, and for the
// same reason: a guessable token would let any local process edit the scene.
void fillRandom(std::uint8_t* out, std::size_t n)
{
	if (Hpak::randomBytes(out, n)) return;
	std::random_device rd;
	for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::uint8_t>(rd() & 0xFFu);
}

std::string randomTokenHex(std::size_t bytes)
{
	std::vector<std::uint8_t> raw(bytes);
	fillRandom(raw.data(), raw.size());

	static const char* kHex = "0123456789abcdef";
	std::string out;
	out.reserve(bytes * 2);
	for (const std::uint8_t b : raw)
	{
		out.push_back(kHex[(b >> 4) & 0xF]);
		out.push_back(kHex[b & 0xF]);
	}
	return out;
}

// Constant-time-ish comparison. The token never leaves this machine and an
// attacker who can time it can read the file anyway, but comparing lengths first
// and never short-circuiting costs nothing and removes the question.
bool tokenEquals(const std::string& a, const std::string& b)
{
	if (a.size() != b.size()) return false;
	unsigned char diff = 0;
	for (std::size_t i = 0; i < a.size(); ++i)
		diff |= static_cast<unsigned char>(a[i] ^ b[i]);
	return diff == 0;
}

int currentPid()
{
#ifdef _WIN32
	return static_cast<int>(::_getpid());
#else
	return static_cast<int>(::getpid());
#endif
}

} // namespace

McpBridge::~McpBridge()
{
	// The endpoint file names a port and a pid. Outliving either of them turns
	// it into a lie that a shim will act on, so it dies with this object even on
	// an unclean editor shutdown path.
	stop();
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────

bool McpBridge::start()
{
	if (m_transport) return true;

	m_transport = HE::Net::TcpTransport::listenLoopback(m_requestedPort);
	if (!m_transport)
	{
		HE_LOG_ERROR(Editor, "MCP: could not open the local listener on port %u — "
		                     "external clients will not be able to connect",
		             static_cast<unsigned>(m_requestedPort));
		return false;
	}
	// Below the transport default, so an over-long prefix is refused before
	// anything is buffered for it.
	m_transport->setMaxFrameSize(kMaxFrameBytes);

	m_token = randomTokenHex(32);
	m_clients.clear();

	if (!writeEndpointFile())
	{
		// Without the file a client cannot learn the port or the token, so the
		// listener would be unreachable in practice. Refuse rather than run a
		// socket nobody can use.
		HE_LOG_ERROR(Editor, "MCP: could not write the endpoint file %s — "
		                     "shutting the listener down again",
		             m_endpointFile.string().c_str());
		m_transport.reset();
		m_token.clear();
		return false;
	}

	HE_LOG_INFO(Editor, "MCP: listening on 127.0.0.1:%u, endpoint file %s",
	            static_cast<unsigned>(port()), m_endpointFile.string().c_str());
	return true;
}

void McpBridge::stop()
{
	if (!m_transport && m_token.empty()) return;

	if (m_transport)
	{
		// Disconnect politely before the socket goes: a shim that sees a clean
		// close reconnects, one that sees a reset reports an error at the user.
		for (const auto& [id, c] : m_clients) m_transport->disconnect(id);
		m_transport->update();
		m_transport.reset();
		HE_LOG_INFO(Editor, "%s", "MCP: listener stopped");
	}
	m_clients.clear();
	m_token.clear();
	removeEndpointFile();
}

void McpBridge::setEnabled(bool on)
{
	if (on == m_enabled) return;
	m_enabled = on;
	if (on) start();
	else    stop();
}

std::uint16_t McpBridge::port() const
{
	return m_transport ? m_transport->boundPort() : 0;
}

std::size_t McpBridge::clientCount() const
{
	std::size_t n = 0;
	for (const auto& [id, c] : m_clients) if (c.authed) ++n;
	return n;
}

std::size_t McpBridge::connectionCount() const
{
	return m_clients.size();
}

// ─── The frame pump ──────────────────────────────────────────────────────────

void McpBridge::update(std::uint64_t nowMs)
{
	m_nowMs = nowMs;
	if (!m_transport) return;

	m_transport->update();

	NetEvent ev;
	while (m_transport->poll(ev))
	{
		switch (ev.type)
		{
		case NetEventType::Connected:
		{
			if (m_clients.size() >= kMaxClients)
			{
				// Refused rather than queued: a fifth connection is a mistake or
				// a probe, and letting it wait would look like the editor hanging.
				HE_LOG_WARN(Editor, "MCP: refusing connection %u — already at the "
				                    "limit of %zu",
				            static_cast<unsigned>(ev.conn), kMaxClients);
				m_transport->disconnect(ev.conn);
				break;
			}
			m_clients[ev.conn] = Client{ /*authed=*/false, nowMs };
			break;
		}
		case NetEventType::Disconnected:
			m_clients.erase(ev.conn);
			break;
		case NetEventType::Data:
			// The one catch site, and it is not belt-and-braces. nlohmann's
			// `value(key, default)` THROWS when the key exists with another type
			// — `{"method": 123}` is valid JSON, so it sails past the parser and
			// then blows up in the accessor. Without this, an unauthenticated
			// peer could kill the editor with one well-formed frame, which is
			// precisely the boundary this whole file exists to hold.
			try
			{
				handleFrame(ev.conn, ev.data);
			}
			catch (const nlohmann::json::exception& e)
			{
				auto it = m_clients.find(ev.conn);
				const bool authed = (it != m_clients.end()) && it->second.authed;
				HE_LOG_WARN(Editor, "MCP: malformed request from connection %u (%s)",
				            static_cast<unsigned>(ev.conn), e.what());
				if (authed)
				{
					// Same rule as any other bad request after the handshake:
					// answered, not punished.
					sendError(ev.conn, json(nullptr), kInvalidRequest,
					          "request field has the wrong type");
				}
				else if (it != m_clients.end())
				{
					m_transport->disconnect(ev.conn);
					m_clients.erase(it);
				}
			}
			break;
		}
	}

	// Answers queued by the handlers above leave in THIS frame rather than the
	// next one: a client that is waiting on a response would otherwise pay a
	// frame of latency per request, and a request made while the editor is
	// idle-throttled could sit for a long time.
	m_transport->update();
}

void McpBridge::handleFrame(ConnectionId id, const std::vector<std::uint8_t>& data)
{
	auto it = m_clients.find(id);
	if (it == m_clients.end()) return;   // already dropped this frame

	json msg;
	{
		// Nothing here is allowed to throw out into the frame loop: a malformed
		// frame is an ordinary event on a public-ish socket, not an editor crash.
		msg = json::parse(data.begin(), data.end(), nullptr, /*allow_exceptions=*/false);
	}

	const bool parsed = !msg.is_discarded();

	// ── The handshake ────────────────────────────────────────────────────────
	// Before `auth` there is no conversation. An unparseable frame, a wrong
	// method or a wrong token all end the connection without an answer: telling
	// an unauthenticated caller WHY it failed is how a scanner maps a service.
	if (!it->second.authed)
	{
		const bool isAuth = parsed && strField(msg, "method") == "auth";
		const std::string given = (parsed && msg.is_object() && msg.contains("params"))
		                              ? strField(msg["params"], "token")
		                              : std::string();

		if (!isAuth || m_token.empty() || !tokenEquals(given, m_token))
		{
			HE_LOG_WARN(Editor, "MCP: dropping connection %u — %s",
			            static_cast<unsigned>(id),
			            isAuth ? "wrong token" : "first message was not auth");
			// No answer, on purpose and by construction: TcpTransport::disconnect
			// discards whatever is still queued, so a reply written here would
			// never reach the wire anyway — and telling an unauthenticated caller
			// WHICH of the two things it got wrong is how a scanner maps a
			// service. A shim with a stale token sees the connection close, which
			// is the signal it needs (re-read the endpoint file) without being a
			// signal to anybody else.
			m_transport->disconnect(id);
			m_clients.erase(id);
			return;
		}

		it->second.authed = true;
		// Counted before anything else can go wrong with this connection: the
		// Tool Status check reads this number across a `claude mcp get`, and that
		// handshake is over before the next frame begins (see authCount()).
		++m_authCount;
		m_lastAuthClient = (msg.is_object() && msg.contains("params"))
		                       ? strField(msg["params"], "client")
		                       : std::string();
		const std::string named = m_lastAuthClient.empty() ? std::string()
		                                                   : " as " + m_lastAuthClient;
		HE_LOG_INFO(Editor, "MCP: client %u authenticated%s", static_cast<unsigned>(id),
		            named.c_str());
		if (msg.contains("id") && !msg["id"].is_null())
		{
			sendJson(id, json{
				{ "jsonrpc", "2.0" },
				{ "id",      msg["id"] },
				{ "result",  json{
					{ "ok",              true },
					{ "protocolVersion", kProtocolVersion },
					{ "server",          "HorizonEditor" },
					{ "toolCount",       static_cast<int>(m_registry.size()) },
				} },
			});
		}
		return;
	}

	// ── After the handshake ──────────────────────────────────────────────────
	// From here a bad frame is answered, not punished: the client is known, and
	// dropping the link over one malformed request would cost it every pending
	// one as well.
	if (!parsed)
	{
		sendError(id, json(nullptr), kParseError, "frame is not valid JSON");
		return;
	}
	if (!msg.is_object() || strField(msg, "jsonrpc") != "2.0" ||
	    !msg.contains("method") || !msg["method"].is_string())
	{
		sendError(id, msg.is_object() && msg.contains("id") ? msg["id"] : json(nullptr),
		          kInvalidRequest, "expected a JSON-RPC 2.0 request object");
		return;
	}

	const std::string method = msg["method"].get<std::string>();
	const json params = (msg.contains("params") && msg["params"].is_object())
	                        ? msg["params"] : json::object();
	// A request without an id is a notification: it is executed, and it gets no
	// answer. Sending one anyway would leave a reply the client never reads
	// sitting in its stream, misaligning every response after it.
	const bool isNotification = !msg.contains("id") || msg["id"].is_null();
	const json rpcId = isNotification ? json(nullptr) : msg["id"];

	json        result;
	int         errCode = 0;
	std::string errMsg;
	const bool  ok = dispatch(id, method, params, result, errCode, errMsg);

	if (isNotification) return;
	if (!ok) { sendError(id, rpcId, errCode, errMsg); return; }

	sendJson(id, json{ { "jsonrpc", "2.0" }, { "id", rpcId }, { "result", std::move(result) } });
}

bool McpBridge::dispatch(ConnectionId id, const std::string& method, const json& params,
                         json& outResult, int& outErrorCode, std::string& outErrorMessage)
{
	(void)id;

	if (method == "ping")
	{
		outResult = json{ { "pong", true }, { "nowMs", m_nowMs } };
		return true;
	}

	if (method == "auth")
	{
		// A second auth on an authenticated link. Harmless, and answering it is
		// friendlier than dropping a client that reconnected its own state.
		outResult = json{ { "ok", true }, { "protocolVersion", kProtocolVersion } };
		return true;
	}

	if (method == "tools/list")
	{
		outResult = m_registry.listPayload();
		return true;
	}

	if (method == "tools/call")
	{
		const std::string name = strField(params, "name");
		const json args = (params.contains("arguments") && params["arguments"].is_object())
		                      ? params["arguments"] : json::object();
		if (name.empty())
		{
			outErrorCode    = kInvalidParams;
			outErrorMessage = "tools/call needs a 'name'";
			return false;
		}
		const McpTool* tool = m_registry.find(name);
		if (!tool)
		{
			outErrorCode    = kMethodNotFound;
			outErrorMessage = "no such tool: " + name;
			return false;
		}

		const ToolResult r = tool->handler(args);

		// MCP's own result shape, not ours: the shim forwards this to the client
		// unchanged, so `content` has to be a content array and a refusal has to
		// be `isError` rather than a transport error — a tool that says "that
		// entity is locked" is a message for the model to read and act on, not a
		// protocol failure.
		json payload = r.isError
		                   ? json{ { "code", r.errorCode }, { "message", r.errorMessage } }
		                   : r.content;
		outResult = json{
			{ "content", json::array({ json{
				{ "type", "text" },
				{ "text", payload.dump(2) },
			} }) },
			{ "isError",       r.isError },
			// The same payload as structured JSON alongside the text block, so a
			// caller that is not a language model does not have to re-parse a
			// string this side just serialised.
			{ "structuredContent", std::move(payload) },
		};
		if (tool->mutates && !r.isError)
		{
			// "Who did that to my scene" has to stay answerable once a client can
			// move things, so every mutation leaves a line with a prefix a human
			// can search the console for.
			HE_LOG_INFO(Editor, "MCP: %s (client %u)", name.c_str(),
			            static_cast<unsigned>(id));
		}
		return true;
	}

	outErrorCode    = kMethodNotFound;
	outErrorMessage = "unknown method: " + method;
	return false;
}

void McpBridge::sendJson(ConnectionId id, const json& j)
{
	if (!m_transport) return;
	const std::string text = j.dump();
	m_transport->send(id, reinterpret_cast<const std::uint8_t*>(text.data()),
	                  text.size(), SendMode::ReliableOrdered);
}

void McpBridge::sendError(ConnectionId id, const json& rpcId, int code,
                          const std::string& message)
{
	sendJson(id, json{
		{ "jsonrpc", "2.0" },
		{ "id",      rpcId },
		{ "error",   json{ { "code", code }, { "message", message } } },
	});
}

// ─── The endpoint file ───────────────────────────────────────────────────────

bool McpBridge::writeEndpointFile()
{
	if (m_endpointFile.empty()) return false;

	std::error_code ec;
	std::filesystem::create_directories(m_endpointFile.parent_path(), ec);

	const json doc{
		{ "port",            static_cast<int>(port()) },
		{ "pid",             currentPid() },
		{ "token",           m_token },
		{ "protocolVersion", kProtocolVersion },
		// Spelled out rather than assumed by the reader: the listener is IPv4
		// loopback only, and a shim that resolved "localhost" would miss it on
		// macOS (::1 first).
		{ "host",            "127.0.0.1" },
	};
	const std::string text = doc.dump(2);

#ifndef _WIN32
	// open() with an explicit 0600, not ofstream: the token in here is the only
	// thing standing between another local account and the open scene, and an
	// ofstream would create it 0644 first and be corrected afterwards — a window
	// in which it is world-readable. O_TRUNC overwrites a file left behind by a
	// crashed editor.
	const int fd = ::open(m_endpointFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) return false;
	// A pre-existing file keeps its old mode through O_CREAT, so set it anyway.
	::fchmod(fd, 0600);
	const ssize_t written = ::write(fd, text.data(), text.size());
	::close(fd);
	return written == static_cast<ssize_t>(text.size());
#else
	// Windows has no mode bits to set here; the per-user profile directory is
	// what restricts the file, the same way the collaboration config is
	// protected.
	std::ofstream out(m_endpointFile, std::ios::binary | std::ios::trunc);
	if (!out) return false;
	out.write(text.data(), static_cast<std::streamsize>(text.size()));
	return out.good();
#endif
}

void McpBridge::removeEndpointFile()
{
	if (m_endpointFile.empty()) return;
	std::error_code ec;
	std::filesystem::remove(m_endpointFile, ec);
}

} // namespace HE::Ed
