#include "doctest.h"
#include "FakeTeamServer.h"

#include "McpToolHooks.h"
#include "../src/HE_Editor/CollabController.h"
#include "../src/HE_Editor/TeamController.h"

#include <HorizonScene/HorizonWorld.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

// ─── The session as tools ────────────────────────────────────────────────────
// What scripts/he_collab_two_editors.py drives in two editor processes, here in
// one: two real CollabControllers over loopback TCP, each behind its own
// registry, hosted and joined THROUGH the tools. The questions a shortcut would
// hide:
//
//   • is hosting only there when the run asked for it (tools/list is what every
//     connected client sees, so a tool that opens a port must not be in it by
//     default),
//   • does collab_host hand back what collab_join needs — and does the join
//     code stay out of collab_status, which every client can call,
//   • does the roster the tool reports come from the session, i.e. does the
//     joiner's name arrive at the host,
//   • are the refusals the ones a client can act on.

using HE::Ed::McpCollabHooks;
using HE::Ed::McpToolRegistry;
using HE::Ed::ToolResult;
using nlohmann::json;

namespace {

// One fake clock for every pump in this file. TeamClient and CollabController
// age requests and heartbeats with unsigned `now - sentMs`: a pump that starts
// its own clock at 0 again would make a request sent at the end of the previous
// pump look 2^64 ms old, and TeamClient would drop the connection as "stopped
// answering" on the first tick (macOS CI, step 17 of topic 197).
std::uint64_t g_fakeNowMs = 1;

struct Side
{
	CollabController collab;
	HorizonWorld     world;
	McpToolRegistry  registry;
	bool             projectOpen = true;

	explicit Side(bool control)
	{
		collab.setWorld(&world);
		// Same project on both sides, or the host refuses the join as "a
		// different project" — which is correct and not what this test is about.
		collab.setProjectIdentity("mcp-collab-test-project", "MCP Collab Test");
		McpCollabHooks hooks;
		hooks.projectOpen    = [this] { return projectOpen; };
		hooks.displayName    = [] { return std::string("Stored Identity"); };
		hooks.controlAllowed = control;
		HE::Ed::registerCollabTools(registry, collab, std::move(hooks));
	}

	ToolResult call(const char* name, const json& args = json::object())
	{
		const auto* t = registry.find(name);
		REQUIRE(t != nullptr);
		return t->handler(args);
	}
};

template <typename Fn>
bool pumpUntil(Side& a, Side& b, Fn done)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < deadline)
	{
		a.collab.update(g_fakeNowMs);
		b.collab.update(g_fakeNowMs);
		if (done()) return true;
		g_fakeNowMs += 16;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return done();
}

bool rosterHas(const json& status, const std::string& name)
{
	for (const auto& p : status["participants"])
		if (p.value("name", std::string()) == name) return true;
	return false;
}

} // namespace

TEST_CASE("collab tools: without the run's permission only collab_status exists")
{
	Side s(false);
	CHECK(s.registry.find("collab_status") != nullptr);
	// Hosting reaches past this machine; a client must not find it in tools/list
	// unless the editor was started for exactly that.
	CHECK(s.registry.find("collab_host") == nullptr);
	CHECK(s.registry.find("collab_join") == nullptr);
	CHECK(s.registry.find("collab_leave") == nullptr);
	CHECK_FALSE(s.registry.find("collab_status")->mutates);

	const ToolResult r = s.call("collab_status");
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["status"] == "idle");
	CHECK(r.content["inSession"] == false);
	CHECK(r.content["participants"].empty());
}

TEST_CASE("collab tools: host and join through the tools, roster on both sides")
{
	Side host(true);
	Side guest(true);
	CHECK(host.registry.find("collab_host")->mutates);
	CHECK(host.registry.find("collab_join")->mutates);

	const ToolResult h = host.call("collab_host", json{ { "name", "Host A" } });
	REQUIRE_FALSE(h.isError);
	CHECK(h.content["status"] == "hosting");
	CHECK(h.content["isHost"] == true);
	const int port = h.content["port"].get<int>();
	const std::string code = h.content["joinCode"].get<std::string>();
	CHECK(port > 0);
	CHECK(code.size() >= 16);

	// The join code is the session's secret: the one that hosted has it, and a
	// status call — open to every client — does not repeat it.
	const ToolResult hs = host.call("collab_status");
	CHECK_FALSE(hs.content.contains("joinCode"));
	CHECK(hs.content.dump().find(code) == std::string::npos);

	// A second host on top of a running session would replace everybody's world.
	const ToolResult again = host.call("collab_host");
	REQUIRE(again.isError);
	CHECK(again.errorCode == "already_in_session");

	const ToolResult j = guest.call("collab_join", json{
		{ "host", "127.0.0.1" }, { "port", port }, { "joinCode", code }, { "name", "Guest B" } });
	REQUIRE_FALSE(j.isError);
	CHECK(j.content["status"] == "connecting");

	REQUIRE(pumpUntil(host, guest, [&] {
		return guest.collab.status() == CollabController::Status::Joined &&
		       host.collab.participants().size() == 2 &&
		       guest.collab.participants().size() == 2;
	}));

	const json hostView  = host.call("collab_status").content;
	const json guestView = guest.call("collab_status").content;
	CHECK(guestView["status"] == "joined");
	CHECK(guestView["inSession"] == true);
	CHECK(guestView["isHost"] == false);
	// Both names on both sides: the guest's travelled to the host in the join,
	// the host's came back in the roster.
	CHECK(rosterHas(hostView,  "Host A"));
	CHECK(rosterHas(hostView,  "Guest B"));
	CHECK(rosterHas(guestView, "Host A"));
	CHECK(rosterHas(guestView, "Guest B"));
	CHECK(hostView["you"] != guestView["you"]);

	const ToolResult l = guest.call("collab_leave");
	REQUIRE_FALSE(l.isError);
	CHECK(l.content["inSession"] == false);
	REQUIRE(pumpUntil(host, guest, [&] { return host.collab.participants().size() == 1; }));
	CHECK(host.call("collab_leave").content["inSession"] == false);
}

TEST_CASE("collab tools: refusals a client can act on")
{
	Side s(true);

	s.projectOpen = false;
	CHECK(s.call("collab_host").errorCode == "no_project");
	CHECK(s.call("collab_join", json{ { "host", "127.0.0.1" }, { "port", 1 },
	                                   { "joinCode", "x" } }).errorCode == "no_project");
	s.projectOpen = true;

	CHECK(s.call("collab_host", json{ { "port", 70000 } }).errorCode == "invalid_payload");
	CHECK(s.call("collab_join", json{ { "host", "127.0.0.1" }, { "port", 0 },
	                                   { "joinCode", "x" } }).errorCode == "invalid_payload");
	CHECK(s.call("collab_join", json{ { "host", "127.0.0.1" }, { "port", 5 } })
	          .errorCode == "invalid_payload");
	CHECK(s.call("collab_join", json{ { "sessionId", "abc" }, { "host", "127.0.0.1" },
	                                   { "port", 5 }, { "joinCode", "x" } })
	          .errorCode == "invalid_payload");
	CHECK(s.call("collab_join", json{ { "sessionId", "abc" } }).errorCode == "invalid_payload");
	// Nothing ran, so nothing is left behind.
	CHECK(s.collab.status() == CollabController::Status::Idle);

	// Leaving without a session is a no-op, not a failure.
	CHECK_FALSE(s.call("collab_leave").isError);
}

// With a relay set, a join by address is refused (the address does not say
// which session), so the tools need the panel's other path: the session id.
TEST_CASE("collab tools: with a relay set, collab_join goes by session id")
{
	Side s(true);
	// Nothing listens there: the relay is dialled, not trusted to exist.
	s.collab.setRelayAddress("127.0.0.1:1");

	const ToolResult byAddress = s.call("collab_join", json{
		{ "host", "127.0.0.1" }, { "port", 5 }, { "joinCode", "x" } });
	REQUIRE(byAddress.isError);
	CHECK(byAddress.errorCode == "join_failed");
	CHECK(byAddress.errorMessage.find("session ID") != std::string::npos);

	const ToolResult byId = s.call("collab_join", json{
		{ "sessionId", "some-session" }, { "joinCode", "x" } });
	// Either refused at once (connect to a closed port) or a link that fails
	// in the next updates — never a direct connection, never silence.
	if (!byId.isError)
	{
		Side other(true);
		pumpUntil(s, other, [&] { return s.collab.status() == CollabController::Status::Failed; });
	}
	CHECK(s.collab.status() == CollabController::Status::Failed);
	s.collab.leave();
}

// The real thing: host and join THROUGH the tools via a running collab_server.
TEST_CASE("collab tools: host and join by session id through a real relay (HE_TEST_RELAY)")
{
	const char* env = std::getenv("HE_TEST_RELAY");
	if (!env || !*env) {
		MESSAGE("HE_TEST_RELAY not set — skipped (start collab_server and point it there)");
		return;
	}
	Side host(true);
	Side guest(true);
	host.collab.setRelayAddress(env);
	guest.collab.setRelayAddress(env);

	const ToolResult h = host.call("collab_host", json{ { "name", "Host A" } });
	REQUIRE_FALSE(h.isError);
	const std::string sid  = h.content["sessionId"].get<std::string>();
	const std::string code = h.content["joinCode"].get<std::string>();
	CHECK_FALSE(sid.empty());
	REQUIRE(pumpUntil(host, guest, [&] {
		return host.collab.directoryStatus().find("Hosting through the relay") != std::string::npos;
	}));

	const ToolResult j = guest.call("collab_join", json{
		{ "sessionId", sid }, { "joinCode", code }, { "name", "Guest B" } });
	REQUIRE_FALSE(j.isError);
	REQUIRE(pumpUntil(host, guest, [&] {
		return guest.collab.status() == CollabController::Status::Joined &&
		       host.collab.participants().size() == 2;
	}));
	CHECK(rosterHas(guest.call("collab_status").content, "Host A"));
	CHECK(rosterHas(host.call("collab_status").content, "Guest B"));
	guest.call("collab_leave");
	host.call("collab_leave");
}

// ─── The team server as tools ────────────────────────────────────────────────
// What scripts/he_team_two_editors.py drives in two editor processes against
// the real team server, here against FakeTeamServer: two TeamControllers, each
// behind its own registry, signed in as different members.
namespace {

struct TeamSide
{
	CollabController collab;
	TeamController   team{ collab };
	HorizonWorld     world;
	McpToolRegistry  registry;

	explicit TeamSide(bool control)
	{
		collab.setWorld(&world);
		collab.setProjectIdentity("mcp-team-test-project", "MCP Team Test");
		McpCollabHooks hooks;
		hooks.projectOpen    = [] { return true; };
		hooks.displayName    = [] { return std::string("Stored Identity"); };
		hooks.controlAllowed = control;
		HE::Ed::registerTeamTools(registry, team, std::move(hooks));
	}

	ToolResult call(const char* name, const json& args = json::object())
	{
		const auto* t = registry.find(name);
		REQUIRE(t != nullptr);
		return t->handler(args);
	}
};

template <typename Fn>
bool pumpTeamUntil(FakeTeamServer& server, std::vector<TeamSide*> sides, Fn done)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (std::chrono::steady_clock::now() < deadline)
	{
		server.pump();
		for (TeamSide* s : sides)
		{
			s->collab.update(g_fakeNowMs);
			s->team.update(g_fakeNowMs);
		}
		if (done()) return true;
		g_fakeNowMs += 16;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return done();
}

const json* memberIn(const json& status, const std::string& name)
{
	for (const auto& m : status["members"])
		if (m.value("name", std::string()) == name) return &m;
	return nullptr;
}

} // namespace

TEST_CASE("team tools: without the run's permission only team_status exists")
{
	TeamSide s(false);
	REQUIRE(s.registry.find("team_status") != nullptr);
	CHECK_FALSE(s.registry.find("team_status")->mutates);
	for (const char* name : { "team_sign_in", "team_sign_out", "team_add_member", "team_host",
	                          "team_set_open", "team_invite", "team_join", "team_decline" })
		CHECK(s.registry.find(name) == nullptr);

	const ToolResult r = s.call("team_status");
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["status"] == "signed_out");
	CHECK(r.content["members"].empty());
	CHECK(r.content["sessions"].empty());
}

TEST_CASE("team tools: two members sign in, see each other, host and invite")
{
	FakeTeamServer server;
	REQUIRE(server.t);
	server.ignoreStrangers = true;   // team_host's relay HELLO goes to the same address
	const std::string addr = "127.0.0.1:" + std::to_string(server.t->boundPort());

	TeamSide anna(true);
	TeamSide bob(true);
	for (const char* name : { "team_sign_in", "team_add_member", "team_host", "team_invite",
	                          "team_join", "team_decline", "team_set_open", "team_sign_out" })
		CHECK(anna.registry.find(name)->mutates);

	// Refusals a client can act on, before anything is signed in.
	ToolResult r = anna.call("team_host");
	REQUIRE(r.isError);
	CHECK(r.errorCode == "host_failed");
	CHECK(r.errorMessage.find("Sign in") != std::string::npos);
	r = anna.call("team_sign_in", json{ { "server", addr }, { "team", "stu dio" },
	                                    { "member", "Anna" }, { "password", "x" } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "sign_in_failed");

	r = anna.call("team_sign_in", json{ { "server", addr }, { "team", "studio" },
	                                    { "member", "Anna" }, { "password", "correct-horse" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["status"] == "connecting");
	REQUIRE_FALSE(bob.call("team_sign_in", json{ { "server", addr }, { "team", "studio" },
	                                            { "member", "Bob" },
	                                            { "password", "correct-horse" } }).isError);

	// Presence both ways, read through the tool.
	REQUIRE(pumpTeamUntil(server, { &anna, &bob }, [&] {
		const json a = anna.call("team_status").content;
		const json b = bob.call("team_status").content;
		const json* bobInA = memberIn(a, "Bob");
		const json* annaInB = memberIn(b, "Anna");
		return a["status"] == "signed_in" && b["status"] == "signed_in" && bobInA &&
		       annaInB && (*bobInA)["online"] == true && (*annaInB)["online"] == true;
	}));
	json a = anna.call("team_status").content;
	CHECK(a["team"] == "studio");
	CHECK(a["member"] == "Anna");
	CHECK(a["isAdmin"] == true);
	CHECK((*memberIn(a, "Anna"))["you"] == true);
	CHECK(bob.call("team_status").content["isAdmin"] == false);

	// Invite before hosting is refused with the reason.
	r = anna.call("team_invite", json{ { "member", "Bob" } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "invite_failed");
	CHECK(r.errorMessage.find("Host a session first") != std::string::npos);

	// Add a member: the roster grows once the server has it.
	REQUIRE_FALSE(anna.call("team_add_member", json{ { "name", "Dana" },
	                                                { "password", "dana-pw" } }).isError);
	REQUIRE(pumpTeamUntil(server, { &anna, &bob }, [&] {
		return memberIn(anna.call("team_status").content, "Dana") != nullptr;
	}));

	// Host invitation-only, invite Bob: Bob's editor lists the session as invited.
	r = anna.call("team_host", json{ { "open", false } });
	REQUIRE_FALSE(r.isError);
	REQUIRE(pumpTeamUntil(server, { &anna, &bob }, [&] {
		return !anna.call("team_status").content["hostedSession"].get<std::string>().empty();
	}));
	a = anna.call("team_status").content;
	const std::string sid = a["hostedSession"].get<std::string>();
	CHECK(a["hostedSessionOpen"] == false);
	CHECK(a["sessionRunning"] == true);
	CHECK(a["status"] == "signed_in");
	r = anna.call("team_invite", json{ { "member", "Bob" } });
	INFO(r.errorCode, " ", r.errorMessage);
	REQUIRE_FALSE(r.isError);
	REQUIRE(pumpTeamUntil(server, { &anna, &bob }, [&] {
		// Held in a local: a range-for over call(...).content[...] would walk a
		// destroyed temporary (C++20 does not extend it through operator[]).
		const json b = bob.call("team_status").content;
		for (const auto& s : b["sessions"])
			if (s["session"] == sid && s["invited"] == true && s["host"] == "Anna") return true;
		return false;
	}));
	// Fake time stays far below the heartbeat interval, so a ping means the
	// clock went backwards somewhere and the clients aged their requests wrong.
	CHECK(server.pings == 0);

	CHECK(anna.call("team_sign_out").content["status"] == "signed_out");
	anna.collab.leave();
	bob.call("team_sign_out");
}
