#include "doctest.h"

#include "McpToolRegistry.h"
#include "../src/HE_Editor/CollabController.h"

#include <HorizonScene/HorizonWorld.h>

#include <chrono>
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
	std::uint64_t now = 0;
	while (std::chrono::steady_clock::now() < deadline)
	{
		a.collab.update(now);
		b.collab.update(now);
		if (done()) return true;
		now += 16;
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
	// Nothing ran, so nothing is left behind.
	CHECK(s.collab.status() == CollabController::Status::Idle);

	// Leaving without a session is a no-op, not a failure.
	CHECK_FALSE(s.call("collab_leave").isError);
}
