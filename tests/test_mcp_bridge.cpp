#include "doctest.h"

#include "McpBridge.h"
#include "McpToolRegistry.h"

#include <Net/TcpTransport.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// ─── The door an external client knocks on ───────────────────────────────────
// Everything here runs over a REAL loopback socket rather than against a mocked
// transport, because the questions this file exists to answer are about what a
// stranger on that socket can get away with: can it skip the handshake, can it
// guess the token, can a fifth of them arrive, can one of them make the editor
// allocate whatever it claims. A mock would answer all of those the way the code
// intends rather than the way it behaves.
//
// EditorApplication is not in this binary — which is the point of the bridge
// taking its editor state as hooks. The "editor" below is five lambdas.

using HE::Ed::McpBridge;
using HE::Ed::McpEditorHooks;
using HE::Ed::McpTool;
using HE::Ed::McpToolRegistry;
using HE::Ed::ToolResult;
using HE::Net::ConnectionId;
using HE::Net::NetEvent;
using HE::Net::NetEventType;
using HE::Net::SendMode;
using HE::Net::TcpTransport;

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// A client that speaks the bridge's protocol: connect, frame a JSON-RPC object,
// pump both ends until the matching id comes back.
struct TestClient
{
	std::unique_ptr<TcpTransport> t;
	bool                          disconnected = false;
	std::vector<json>             inbox;

	bool connect(std::uint16_t port)
	{
		t = TcpTransport::connect("127.0.0.1", port);
		return t != nullptr;
	}

	void send(const json& j)
	{
		const std::string text = j.dump();
		t->send(ConnectionId{ 1 }, reinterpret_cast<const std::uint8_t*>(text.data()),
		        text.size(), SendMode::ReliableOrdered);
	}

	// Drain whatever has arrived, recording replies and noticing a drop.
	void drain()
	{
		NetEvent ev;
		while (t->poll(ev))
		{
			if (ev.type == NetEventType::Disconnected) disconnected = true;
			if (ev.type != NetEventType::Data) continue;
			json j = json::parse(ev.data.begin(), ev.data.end(), nullptr, false);
			if (!j.is_discarded()) inbox.push_back(std::move(j));
		}
	}
};

// Drive bridge and client until `done` or the deadline. Nothing on a socket may
// be assumed to have happened without pumping, and a bounded wait keeps a broken
// build from hanging the suite.
template <typename Fn>
bool pumpUntil(McpBridge& bridge, std::vector<TestClient*> clients, Fn done,
               std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
	const auto    deadline = std::chrono::steady_clock::now() + timeout;
	std::uint64_t now      = 0;
	while (std::chrono::steady_clock::now() < deadline)
	{
		bridge.update(now);
		for (auto* c : clients) { c->t->update(); c->drain(); }
		if (done()) return true;
		now += 16;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	bridge.update(now);
	for (auto* c : clients) { c->t->update(); c->drain(); }
	return done();
}

// Find a reply by JSON-RPC id.
const json* replyWithId(const TestClient& c, int id)
{
	for (const auto& j : c.inbox)
		if (j.is_object() && j.contains("id") && j["id"].is_number_integer() &&
		    j["id"].get<int>() == id)
			return &j;
	return nullptr;
}

// A bridge in a temp directory, already started, with the two stub tools on it.
// TMPDIR is per-test (tests/CMakeLists.txt sets it), so nothing here collides
// with a parallel run.
struct Fixture
{
	McpBridge bridge;
	fs::path  dir;

	explicit Fixture(McpEditorHooks hooks = {})
	{
		dir = fs::temp_directory_path() /
		      ("he_mcp_" + std::to_string(
		                       std::chrono::steady_clock::now().time_since_epoch().count()));
		fs::create_directories(dir);
		bridge.setEndpointFile(dir / "mcp-endpoint.json");
		HE::Ed::registerCoreTools(bridge.registry(), std::move(hooks));
		REQUIRE(bridge.start());
		REQUIRE(bridge.port() != 0);
	}

	~Fixture()
	{
		bridge.stop();
		std::error_code ec;
		fs::remove_all(dir, ec);
	}

	// Connect, authenticate, and leave the client ready to call tools.
	bool authenticate(TestClient& c, int id = 1)
	{
		// Relative to whatever is already connected, so the same helper serves
		// the single-client cases and the four-client limit case.
		const std::size_t before = bridge.connectionCount();
		if (!c.connect(bridge.port())) return false;
		if (!pumpUntil(bridge, { &c },
		               [&] { return bridge.connectionCount() > before; }))
			return false;
		c.send(json{ { "jsonrpc", "2.0" },
		             { "id", id },
		             { "method", "auth" },
		             { "params", json{ { "token", bridge.token() } } } });
		return pumpUntil(bridge, { &c }, [&] { return replyWithId(c, id) != nullptr; });
	}
};

// The structured half of a tools/call result, which is what a non-model caller
// reads (the text block is the same payload, serialised for the model).
json structured(const json& reply)
{
	REQUIRE(reply.contains("result"));
	REQUIRE(reply["result"].contains("structuredContent"));
	return reply["result"]["structuredContent"];
}

} // namespace

// ─── The registry ────────────────────────────────────────────────────────────

TEST_CASE("McpToolRegistry: a dotted name is refused, because the client would reject it")
{
	// The plan from step 1 wrote `scene.info`. It cannot ship: the shim forwards
	// the name verbatim and the Messages API only takes [a-zA-Z0-9_-]. Refusing
	// here is what keeps the failure in this repository rather than at the far
	// end of somebody's client.
	CHECK(McpToolRegistry::enforceNameRule("scene_info"));
	CHECK(McpToolRegistry::enforceNameRule("entity-create"));
	CHECK_FALSE(McpToolRegistry::enforceNameRule("scene.info"));
	CHECK_FALSE(McpToolRegistry::enforceNameRule("hc.add_node"));
	CHECK_FALSE(McpToolRegistry::enforceNameRule(""));
	CHECK_FALSE(McpToolRegistry::enforceNameRule(std::string(65, 'a')));

	McpToolRegistry reg;
	McpTool bad;
	bad.name        = "scene.info";
	bad.inputSchema = json::object();
	bad.handler     = [](const json&) { return ToolResult::ok(json::object()); };
	CHECK_FALSE(reg.add(bad));
	CHECK(reg.size() == 0);
}

TEST_CASE("McpToolRegistry: a tool without a schema or a handler is not a tool")
{
	McpToolRegistry reg;

	McpTool noHandler;
	noHandler.name        = "a";
	noHandler.inputSchema = json::object();
	CHECK_FALSE(reg.add(noHandler));

	McpTool noSchema;
	noSchema.name        = "b";
	noSchema.inputSchema = json("not an object");
	noSchema.handler     = [](const json&) { return ToolResult::ok(json::object()); };
	CHECK_FALSE(reg.add(noSchema));

	McpTool good;
	good.name        = "c";
	good.inputSchema = json::object();
	good.handler     = [](const json&) { return ToolResult::ok(json::object()); };
	CHECK(reg.add(good));
	// The same name twice would leave find() picking one of two silently.
	CHECK_FALSE(reg.add(good));
	CHECK(reg.size() == 1);
}

// ─── The handshake ───────────────────────────────────────────────────────────

TEST_CASE("McpBridge: a connection that does not authenticate first is dropped")
{
	Fixture   f;
	TestClient c;
	REQUIRE(c.connect(f.bridge.port()));
	REQUIRE(pumpUntil(f.bridge, { &c }, [&] { return f.bridge.connectionCount() == 1; }));

	// A perfectly well-formed request — for a method that exists — sent before
	// auth. It gets no answer at all: telling an unauthenticated caller what went
	// wrong is how a scanner maps a service.
	c.send(json{ { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "tools/list" } });

	REQUIRE(pumpUntil(f.bridge, { &c }, [&] { return f.bridge.connectionCount() == 0; }));
	CHECK(c.inbox.empty());
	CHECK(f.bridge.clientCount() == 0);
}

TEST_CASE("McpBridge: a wrong token is dropped, a right one is let in")
{
	Fixture f;

	{
		TestClient wrong;
		REQUIRE(wrong.connect(f.bridge.port()));
		REQUIRE(pumpUntil(f.bridge, { &wrong },
		                  [&] { return f.bridge.connectionCount() == 1; }));
		wrong.send(json{ { "jsonrpc", "2.0" },
		                 { "id", 1 },
		                 { "method", "auth" },
		                 { "params", json{ { "token", "0000000000000000" } } } });
		REQUIRE(pumpUntil(f.bridge, { &wrong },
		                  [&] { return f.bridge.connectionCount() == 0; }));
		CHECK(f.bridge.clientCount() == 0);
	}

	TestClient ok;
	REQUIRE(f.authenticate(ok));
	const json* reply = replyWithId(ok, 1);
	REQUIRE(reply != nullptr);
	REQUIRE(reply->contains("result"));
	CHECK((*reply)["result"]["ok"] == true);
	CHECK((*reply)["result"]["protocolVersion"] == McpBridge::kProtocolVersion);
	CHECK(f.bridge.clientCount() == 1);
}

TEST_CASE("McpBridge: every listed tool carries a schema")
{
	Fixture    f;
	TestClient c;
	REQUIRE(f.authenticate(c));

	c.send(json{ { "jsonrpc", "2.0" }, { "id", 2 }, { "method", "tools/list" } });
	REQUIRE(pumpUntil(f.bridge, { &c }, [&] { return replyWithId(c, 2) != nullptr; }));

	const json* reply = replyWithId(c, 2);
	REQUIRE(reply->contains("result"));
	const json& tools = (*reply)["result"]["tools"];
	REQUIRE(tools.is_array());
	CHECK(tools.size() == 2);

	bool sawPing = false, sawInfo = false;
	for (const auto& t : tools)
	{
		// Not decoration: the schema is what the model reads before deciding
		// what to send, so a tool without one is a tool nobody can call right.
		REQUIRE(t.contains("inputSchema"));
		CHECK(t["inputSchema"].is_object());
		CHECK(t["description"].is_string());
		CHECK_FALSE(t["description"].get<std::string>().empty());
		CHECK(McpToolRegistry::enforceNameRule(t["name"].get<std::string>()));
		if (t["name"] == "ping")       sawPing = true;
		if (t["name"] == "scene_info") sawInfo = true;
	}
	CHECK(sawPing);
	CHECK(sawInfo);
}

// ─── The stub tools, end to end ──────────────────────────────────────────────

TEST_CASE("McpBridge: ping goes out over the socket and comes back")
{
	Fixture    f;
	TestClient c;
	REQUIRE(f.authenticate(c));

	c.send(json{ { "jsonrpc", "2.0" },
	             { "id", 7 },
	             { "method", "tools/call" },
	             { "params", json{ { "name", "ping" },
	                               { "arguments", json{ { "echo", "hallo" } } } } } });
	REQUIRE(pumpUntil(f.bridge, { &c }, [&] { return replyWithId(c, 7) != nullptr; }));

	const json* reply = replyWithId(c, 7);
	CHECK((*reply)["result"]["isError"] == false);
	// MCP's own shape, because the shim forwards this unchanged.
	REQUIRE((*reply)["result"]["content"].is_array());
	CHECK((*reply)["result"]["content"][0]["type"] == "text");

	const json s = structured(*reply);
	CHECK(s["pong"] == true);
	CHECK(s["echo"] == "hallo");
}

TEST_CASE("McpBridge: scene_info reports the editor's real state, not an echo")
{
	// The whole point of the stub: prove the pipe carries live editor state.
	// Every field is driven from a variable the test then changes.
	std::string scene   = "Content/Scenes/Test.hescene";
	bool        dirty   = false;
	bool        playing = false;
	int         count   = 3;

	McpEditorHooks hooks;
	hooks.projectName = [] { return std::string("TestProject"); };
	hooks.scenePath   = [&] { return scene; };
	hooks.sceneDirty  = [&] { return dirty; };
	hooks.isPlaying   = [&] { return playing; };
	hooks.inSession   = [] { return false; };
	hooks.entityCount = [&] { return count; };

	Fixture    f(std::move(hooks));
	TestClient c;
	REQUIRE(f.authenticate(c));

	auto callInfo = [&](int id) {
		c.send(json{ { "jsonrpc", "2.0" },
		             { "id", id },
		             { "method", "tools/call" },
		             { "params", json{ { "name", "scene_info" } } } });
		REQUIRE(pumpUntil(f.bridge, { &c }, [&] { return replyWithId(c, id) != nullptr; }));
		return structured(*replyWithId(c, id));
	};

	json s = callInfo(10);
	CHECK(s["project"] == "TestProject");
	CHECK(s["scene"] == scene);
	CHECK(s["sceneOpen"] == true);
	CHECK(s["dirty"] == false);
	CHECK(s["playing"] == false);
	CHECK(s["inSession"] == false);
	CHECK(s["entityCount"] == 3);
	CHECK(s["worldOpen"] == true);

	// Move the editor underneath it. A stub that returned a constant would pass
	// the first call and fail here.
	dirty   = true;
	playing = true;
	count   = 11;
	s       = callInfo(11);
	CHECK(s["dirty"] == true);
	CHECK(s["playing"] == true);
	CHECK(s["entityCount"] == 11);

	// No project at all is a real state — the listener starts before a scene is
	// loaded — and it has to be reported as such rather than as an empty scene.
	scene = "";
	count = -1;
	s     = callInfo(12);
	CHECK(s["sceneOpen"] == false);
	CHECK(s["worldOpen"] == false);
	CHECK(s["entityCount"] == -1);
}

TEST_CASE("McpBridge: a bad request after auth is answered, not punished")
{
	Fixture    f;
	TestClient c;
	REQUIRE(f.authenticate(c));

	// Unknown method.
	c.send(json{ { "jsonrpc", "2.0" }, { "id", 20 }, { "method", "does/not/exist" } });
	REQUIRE(pumpUntil(f.bridge, { &c }, [&] { return replyWithId(c, 20) != nullptr; }));
	CHECK((*replyWithId(c, 20))["error"]["code"] == -32601);

	// Unknown tool.
	c.send(json{ { "jsonrpc", "2.0" },
	             { "id", 21 },
	             { "method", "tools/call" },
	             { "params", json{ { "name", "entity_teleport_to_mars" } } } });
	REQUIRE(pumpUntil(f.bridge, { &c }, [&] { return replyWithId(c, 21) != nullptr; }));
	CHECK((*replyWithId(c, 21))["error"]["code"] == -32601);

	// tools/call without a name.
	c.send(json{ { "jsonrpc", "2.0" }, { "id", 22 }, { "method", "tools/call" } });
	REQUIRE(pumpUntil(f.bridge, { &c }, [&] { return replyWithId(c, 22) != nullptr; }));
	CHECK((*replyWithId(c, 22))["error"]["code"] == -32602);

	// Not JSON at all. Answered with a parse error — and the connection survives,
	// which is what the next call proves.
	const std::string junk = "{not json";
	c.t->send(ConnectionId{ 1 }, reinterpret_cast<const std::uint8_t*>(junk.data()),
	          junk.size(), SendMode::ReliableOrdered);

	c.send(json{ { "jsonrpc", "2.0" }, { "id", 23 }, { "method", "ping" } });
	REQUIRE(pumpUntil(f.bridge, { &c }, [&] { return replyWithId(c, 23) != nullptr; }));
	CHECK((*replyWithId(c, 23))["result"]["pong"] == true);
	CHECK(f.bridge.clientCount() == 1);
}

TEST_CASE("McpBridge: a notification is executed and left unanswered")
{
	Fixture    f;
	TestClient c;
	REQUIRE(f.authenticate(c));
	const std::size_t before = c.inbox.size();

	// No id — JSON-RPC says no reply. Sending one anyway would leave a response
	// the client never reads in its stream, misaligning every one after it.
	c.send(json{ { "jsonrpc", "2.0" }, { "method", "ping" } });
	c.send(json{ { "jsonrpc", "2.0" }, { "id", 30 }, { "method", "ping" } });
	REQUIRE(pumpUntil(f.bridge, { &c }, [&] { return replyWithId(c, 30) != nullptr; }));

	// Exactly one new message: the answer to the second call.
	CHECK(c.inbox.size() == before + 1);
}

// ─── The limits ──────────────────────────────────────────────────────────────

TEST_CASE("McpBridge: the fifth connection is refused and the first four keep working")
{
	Fixture f;

	std::vector<std::unique_ptr<TestClient>> clients;
	std::vector<TestClient*>                 all;
	for (int i = 0; i < 4; ++i)
	{
		clients.push_back(std::make_unique<TestClient>());
		all.push_back(clients.back().get());
		REQUIRE(f.authenticate(*clients.back(), 100 + i));
	}
	CHECK(f.bridge.clientCount() == McpBridge::kMaxClients);

	TestClient fifth;
	REQUIRE(fifth.connect(f.bridge.port()));
	std::vector<TestClient*> withFifth = all;
	withFifth.push_back(&fifth);
	REQUIRE(pumpUntil(f.bridge, withFifth, [&] { return fifth.disconnected; }));
	CHECK(f.bridge.connectionCount() == McpBridge::kMaxClients);

	// And the four that were already in are unharmed — a limit that took the
	// service down with it would be worse than no limit.
	for (int i = 0; i < 4; ++i)
	{
		clients[i]->send(json{ { "jsonrpc", "2.0" }, { "id", 200 + i }, { "method", "ping" } });
	}
	REQUIRE(pumpUntil(f.bridge, all, [&] {
		for (int i = 0; i < 4; ++i)
			if (!replyWithId(*clients[i], 200 + i)) return false;
		return true;
	}));
	for (int i = 0; i < 4; ++i)
		CHECK((*replyWithId(*clients[i], 200 + i))["result"]["pong"] == true);
}

TEST_CASE("McpBridge: an oversized frame kills its own connection and no other")
{
	Fixture f;

	TestClient hostile, bystander;
	REQUIRE(f.authenticate(hostile, 1));
	REQUIRE(f.authenticate(bystander, 2));
	std::vector<TestClient*> both{ &hostile, &bystander };

	// 5 MiB, over the bridge's 4 MiB ceiling and far under the transport's own —
	// so this tests the bridge's limit, not the transport's default. The client's
	// own limit is untouched, which is why it can send what the bridge refuses.
	std::string payload(5u * 1024u * 1024u, 'x');
	json big{ { "jsonrpc", "2.0" }, { "id", 40 }, { "method", "ping" },
	          { "params", json{ { "junk", payload } } } };
	hostile.send(big);

	REQUIRE(pumpUntil(f.bridge, both, [&] { return f.bridge.connectionCount() == 1; },
	                  std::chrono::seconds(10)));

	// The bystander never noticed.
	bystander.send(json{ { "jsonrpc", "2.0" }, { "id", 41 }, { "method", "ping" } });
	REQUIRE(pumpUntil(f.bridge, both,
	                  [&] { return replyWithId(bystander, 41) != nullptr; }));
	CHECK((*replyWithId(bystander, 41))["result"]["pong"] == true);
	CHECK_FALSE(bystander.disconnected);
}

// ─── The endpoint file ───────────────────────────────────────────────────────

TEST_CASE("McpBridge: the endpoint file appears with the listener and dies with it")
{
	const fs::path dir = fs::temp_directory_path() / "he_mcp_endpoint";
	fs::create_directories(dir);
	const fs::path file = dir / "mcp-endpoint.json";
	std::error_code ec;
	fs::remove(file, ec);

	{
		McpBridge bridge;
		bridge.setEndpointFile(file);
		REQUIRE(bridge.start());
		REQUIRE(fs::exists(file));

		std::ifstream in(file);
		json          doc;
		in >> doc;
		// Everything the shim needs to reach this editor, and nothing it has to
		// be told by hand.
		CHECK(doc["port"].get<int>() == static_cast<int>(bridge.port()));
		CHECK(doc["pid"].get<int>() != 0);
		CHECK(doc["token"].get<std::string>() == bridge.token());
		CHECK(doc["token"].get<std::string>().size() == 64);   // 32 bytes, hex
		// Spelled out rather than left to the reader: "localhost" resolves to ::1
		// first on macOS, and this listener is IPv4 loopback only.
		CHECK(doc["host"] == "127.0.0.1");

#ifndef _WIN32
		// The token in this file is the only thing between another local account
		// and the open scene. 0600, and asserted rather than trusted: an ofstream
		// would have created it 0644.
		const auto perms = fs::status(file).permissions();
		CHECK((perms & fs::perms::owner_read) != fs::perms::none);
		CHECK((perms & fs::perms::owner_write) != fs::perms::none);
		CHECK((perms & fs::perms::group_all) == fs::perms::none);
		CHECK((perms & fs::perms::others_all) == fs::perms::none);
#endif

		bridge.stop();
		// A file that outlives its listener points a shim at whatever the OS
		// handed that port to next.
		CHECK_FALSE(fs::exists(file));
	}

	// And the destructor does it too, for the shutdown path that never reaches
	// stop() by hand.
	{
		McpBridge bridge;
		bridge.setEndpointFile(file);
		REQUIRE(bridge.start());
		REQUIRE(fs::exists(file));
	}
	CHECK_FALSE(fs::exists(file));

	fs::remove_all(dir, ec);
}

TEST_CASE("McpBridge: setEnabled is the whole lifecycle, and it is off to begin with")
{
	const fs::path dir = fs::temp_directory_path() / "he_mcp_enable";
	fs::create_directories(dir);
	const fs::path file = dir / "mcp-endpoint.json";

	McpBridge bridge;
	bridge.setEndpointFile(file);

	// Nothing listens until it is switched on. This is the first line of the
	// security model, so it is asserted rather than assumed from the default.
	CHECK_FALSE(bridge.isEnabled());
	CHECK_FALSE(bridge.isRunning());
	CHECK(bridge.port() == 0);
	CHECK_FALSE(fs::exists(file));

	bridge.setEnabled(true);
	CHECK(bridge.isRunning());
	const std::uint16_t first = bridge.port();
	CHECK(first != 0);
	CHECK(fs::exists(file));
	const std::string firstToken = bridge.token();

	// Idempotent: pushed every frame from the frame loop, so a repeated value
	// must not tear the listener down and build it again.
	bridge.setEnabled(true);
	CHECK(bridge.port() == first);
	CHECK(bridge.token() == firstToken);

	bridge.setEnabled(false);
	CHECK_FALSE(bridge.isRunning());
	CHECK(bridge.port() == 0);
	CHECK_FALSE(fs::exists(file));

	// Back on: a NEW token, because the old one was published in a file that is
	// now gone and any client still holding it is no longer trusted.
	bridge.setEnabled(true);
	CHECK(bridge.isRunning());
	CHECK(bridge.token() != firstToken);
	bridge.setEnabled(false);

	std::error_code ec;
	fs::remove_all(dir, ec);
}

TEST_CASE("McpBridge: update on a stopped bridge is a no-op, not a crash")
{
	// The frame loop pumps unconditionally, so this is the ordinary case for
	// every editor run that never turns the bridge on.
	McpBridge bridge;
	bridge.update(0);
	bridge.update(16);
	CHECK_FALSE(bridge.isRunning());
	CHECK(bridge.clientCount() == 0);
}
