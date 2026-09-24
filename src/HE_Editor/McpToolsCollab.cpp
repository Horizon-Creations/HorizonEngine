#include "McpToolCommon.h"
#include "McpToolRegistry.h"

#include "CollabController.h"

#include <string>

namespace HE::Ed
{
namespace
{
using json = nlohmann::json;

const char* statusName(CollabController::Status s)
{
	switch (s)
	{
	case CollabController::Status::Idle:       return "idle";
	case CollabController::Status::Hosting:    return "hosting";
	case CollabController::Status::Connecting: return "connecting";
	case CollabController::Status::Joined:     return "joined";
	case CollabController::Status::Failed:     return "failed";
	}
	return "unknown";
}

// What collab_status answers and what the three control tools answer with
// afterwards, so a client reads one shape whichever it called.
json statusJson(const CollabController& c)
{
	json roster = json::array();
	for (const HE::Net::Participant& p : c.participants())
		roster.push_back(json{
			{ "id",     p.id },
			{ "name",   p.name },
			{ "isHost", p.isHost },
		});

	json out{
		{ "status",      statusName(c.status()) },
		{ "inSession",   c.inSession() },
		{ "isHost",      c.isHost() },
		{ "you",         c.localParticipant() },
		{ "participants", std::move(roster) },
		{ "address",     c.localAddress() },
		{ "port",        c.port() },
		{ "sessionId",   c.sessionId() },
		// A snapshot in flight is the difference between "connecting and stuck"
		// and "connecting and 80 % there", which a client polling for `joined`
		// needs to decide whether to keep waiting.
		{ "snapshotProgress", c.snapshotProgress() },
	};
	if (!c.lastError().empty()) out["error"] = c.lastError();
	// The join code is deliberately NOT here. It is the session's only secret,
	// and collab_status is registered for every client; the one that hosted got
	// it from collab_host.
	return out;
}

ToolResult failNoProject()
{
	return ToolResult::fail("no_project",
		"no project is open — a session shares the open project's scene, and the "
		"peers are matched on the project's id, so there is nothing to share yet");
}

ToolResult failAlreadyActive(const CollabController& c)
{
	return ToolResult::fail("already_in_session",
		std::string("this editor is already ") + statusName(c.status()) +
		" — call collab_leave first; a second session would replace the first "
		"one's world under everybody in it");
}

std::string nameFrom(const json& args, const McpCollabHooks& hooks)
{
	std::string name = strArg(args, "name");
	if (name.empty() && hooks.displayName) name = hooks.displayName();
	return name;
}

bool portFrom(const json& args, int fallback, std::uint16_t& out)
{
	const int port = intArg(args, "port", fallback);
	if (port < 0 || port > 65535) return false;
	out = static_cast<std::uint16_t>(port);
	return true;
}

} // namespace

void registerCollabTools(McpToolRegistry& registry, ::CollabController& collab,
                         McpCollabHooks hooks)
{
	CollabController* c = &collab;

	// ── collab_status ────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "collab_status";
		t.description =
			"Whether this editor is in a collaboration session and with whom: status "
			"(idle, hosting, connecting, joined, failed), the participant roster with "
			"ids and names, and the host's address and port. Entity edits made through "
			"the entity_* tools while `inSession` is true reach every participant, and "
			"an entity another participant holds refuses them with locked_by_other.";
		t.inputSchema = objectSchema(json::object(), {});
		t.handler = [c](const json&) { return ToolResult::ok(statusJson(*c)); };
		registry.add(std::move(t));
	}

	if (!hooks.controlAllowed) return;

	// ── collab_host ──────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "collab_host";
		t.description =
			"Open a collaboration session on this editor, exactly as the "
			"Collaboration panel's \"Open session\" button does, and return the "
			"address, port and join code a second editor needs for collab_join. "
			"Unless the editor runs with HE_COLLAB_OFFLINE this also asks the router "
			"to forward the port and publishes the session on the public directory.";
		t.inputSchema = objectSchema(json{
			{ "port", json{ { "type", "integer" }, { "minimum", 0 }, { "maximum", 65535 },
			                { "description", "Port to listen on; 0 (the default) lets "
			                                 "the system pick a free one." } } },
			{ "name", stringProp("Display name for this session. Defaults to the "
			                     "editor's stored identity.") },
		}, {});
		t.mutates = true;
		t.handler = [c, hooks](const json& args) -> ToolResult {
			if (!hooks.projectOpen || !hooks.projectOpen()) return failNoProject();
			if (c->active()) return failAlreadyActive(*c);
			std::uint16_t port = 0;
			if (!portFrom(args, 0, port))
				return ToolResult::fail("invalid_payload", "'port' must be 0..65535");
			if (!c->startHosting(port, nameFrom(args, hooks)))
				return ToolResult::fail("host_failed", c->lastError());
			json out = statusJson(*c);
			out["joinCode"] = c->joinCode();
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}

	// ── collab_join ──────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "collab_join";
		t.description =
			"Join a collaboration session by address, port and join code — the "
			"panel's direct-connect path. Returns while the link is still being set "
			"up (status `connecting`); poll collab_status until it says `joined`, at "
			"which point the host's scene has REPLACED this editor's world, or "
			"`failed` with the reason in `error`.";
		t.inputSchema = objectSchema(json{
			{ "host",     stringProp("Address of the hosting editor, e.g. 127.0.0.1.") },
			{ "port",     json{ { "type", "integer" }, { "minimum", 1 }, { "maximum", 65535 },
			                    { "description", "The host's port, from its collab_host." } } },
			{ "joinCode", stringProp("The join code the host's collab_host returned.") },
			{ "name",     stringProp("Display name for this session. Defaults to the "
			                         "editor's stored identity.") },
		}, { "host", "port", "joinCode" });
		t.mutates = true;
		t.handler = [c, hooks](const json& args) -> ToolResult {
			if (!hooks.projectOpen || !hooks.projectOpen()) return failNoProject();
			if (c->active()) return failAlreadyActive(*c);
			const std::string host = strArg(args, "host");
			const std::string code = strArg(args, "joinCode");
			std::uint16_t port = 0;
			if (host.empty() || code.empty() || !portFrom(args, 0, port) || port == 0)
				return ToolResult::fail("invalid_payload",
				                        "'host', 'port' (1..65535) and 'joinCode' are all required");
			if (!c->joinSession(host, port, code, nameFrom(args, hooks)))
				return ToolResult::fail("join_failed", c->lastError());
			return ToolResult::ok(statusJson(*c));
		};
		registry.add(std::move(t));
	}

	// ── collab_leave ─────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "collab_leave";
		t.description =
			"Leave the collaboration session (or close it, on the host), as the "
			"panel's Leave button does. Answers with the status afterwards; leaving "
			"when there is no session is not an error.";
		t.inputSchema = objectSchema(json::object(), {});
		t.mutates = true;
		t.handler = [c](const json&) {
			c->leave();
			return ToolResult::ok(statusJson(*c));
		};
		registry.add(std::move(t));
	}
}

} // namespace HE::Ed
