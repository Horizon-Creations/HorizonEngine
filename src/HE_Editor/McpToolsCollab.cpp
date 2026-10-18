#include "McpToolCommon.h"
#include "McpToolHooks.h"

#include "CollabController.h"
#include "TeamController.h"

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

const char* teamStatusName(TeamController::Status s)
{
	switch (s)
	{
	case TeamController::Status::SignedOut:    return "signed_out";
	case TeamController::Status::Connecting:   return "connecting";
	case TeamController::Status::SignedIn:     return "signed_in";
	case TeamController::Status::Reconnecting: return "reconnecting";
	case TeamController::Status::Failed:       return "failed";
	}
	return "unknown";
}

// What team_status answers and every team tool answers with afterwards: the
// Team panel's content. Requests to the team server are asynchronous, so a
// client watches `revision` (moves with every roster or session change) and
// polls until the list says what it asked for.
json teamStatusJson(const TeamController& t)
{
	json members = json::array();
	for (const HE::Net::TeamMember& m : t.members())
		members.push_back(json{
			{ "name",   m.name },
			{ "admin",  m.admin },
			{ "you",    m.you },
			{ "online", m.online },
		});
	json sessions = json::array();
	for (const HE::Net::TeamSessionInfo& s : t.sessions())
		sessions.push_back(json{
			{ "session", s.session },
			{ "host",    s.host },
			{ "open",    s.open },
			{ "invited", s.invited },
			{ "hosting", s.hosting },
		});
	json out{
		{ "status",            teamStatusName(t.status()) },
		{ "server",            t.serverText() },
		{ "team",              t.team() },
		{ "member",            t.member() },
		{ "isAdmin",           t.isAdmin() },
		{ "members",           std::move(members) },
		{ "sessions",          std::move(sessions) },
		{ "hostedSession",     t.hostedSession() },
		{ "hostedSessionOpen", t.hostedSessionOpen() },
		{ "sessionRunning",    t.sessionRunning() },
		{ "revision",          t.revision() },
	};
	if (!t.lastError().empty()) out["error"] = t.lastError();
	return out;
}

// The controller's own refusal ("not signed in", "a collab session is already
// running") is in lastError() when one of its calls answers false.
ToolResult teamRefused(const TeamController& t, const char* code)
{
	return ToolResult::fail(code, t.lastError().empty() ? std::string("refused") : t.lastError());
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
			"panel's direct-connect path — or by session ID and join code, the "
			"panel's \"Join by session ID\" path (the only one when a relay server is "
			"set). Returns while the link is still being set up (status "
			"`connecting`); poll collab_status until it says `joined`, at which point "
			"the host's scene has REPLACED this editor's world, or `failed` with the "
			"reason in `error`.";
		t.inputSchema = objectSchema(json{
			{ "host",      stringProp("Address of the hosting editor, e.g. 127.0.0.1.") },
			{ "port",      json{ { "type", "integer" }, { "minimum", 1 }, { "maximum", 65535 },
			                     { "description", "The host's port, from its collab_host." } } },
			{ "sessionId", stringProp("Instead of host and port: the session ID from the "
			                          "host's collab_host or collab_status.") },
			{ "joinCode",  stringProp("The join code the host's collab_host returned.") },
			{ "name",      stringProp("Display name for this session. Defaults to the "
			                          "editor's stored identity.") },
		}, { "joinCode" });
		t.mutates = true;
		t.handler = [c, hooks](const json& args) -> ToolResult {
			if (!hooks.projectOpen || !hooks.projectOpen()) return failNoProject();
			if (c->active()) return failAlreadyActive(*c);
			const std::string sessionId = strArg(args, "sessionId");
			const std::string host = strArg(args, "host");
			const std::string code = strArg(args, "joinCode");
			if (!sessionId.empty())
			{
				if (!host.empty() || args.contains("port"))
					return ToolResult::fail("invalid_payload",
					                        "give either 'sessionId' or 'host' and 'port', not both");
				if (code.empty())
					return ToolResult::fail("invalid_payload", "'joinCode' is required");
				if (!c->joinBySessionId(sessionId, code, nameFrom(args, hooks)))
					return ToolResult::fail("join_failed", c->lastError());
				return ToolResult::ok(statusJson(*c));
			}
			std::uint16_t port = 0;
			if (host.empty() || code.empty() || !portFrom(args, 0, port) || port == 0)
				return ToolResult::fail("invalid_payload",
				                        "'host', 'port' (1..65535) and 'joinCode' are all required, "
				                        "or 'sessionId' and 'joinCode'");
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

void registerTeamTools(McpToolRegistry& registry, ::TeamController& team, McpCollabHooks hooks)
{
	TeamController* tc = &team;

	// ── team_status ──────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "team_status";
		t.description =
			"The Team panel's content: whether this editor is signed in to a team "
			"server (signed_out, connecting, signed_in, reconnecting, failed), as "
			"which member of which team, every member with `online`, and the team's "
			"announced collaboration sessions (open, invited, hosting). `revision` "
			"moves whenever the lists do.";
		t.inputSchema = objectSchema(json::object(), {});
		t.handler = [tc](const json&) { return ToolResult::ok(teamStatusJson(*tc)); };
		registry.add(std::move(t));
	}

	if (!hooks.controlAllowed) return;

	// Everything below talks to a server past this machine, with a password,
	// and team_host / team_join put the open scene into a session — the same
	// reasons collab_host is gated (see McpCollabHooks).

	// ── team_sign_in ─────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "team_sign_in";
		t.description =
			"Sign in to a team server, as Preferences > Team's Sign In does, or with "
			"`create` true create the team with this member as its first admin. "
			"Answers while the login is under way; poll team_status for signed_in "
			"or failed (reason in `error`).";
		t.inputSchema = objectSchema(json{
			{ "server",   stringProp("Team server address, host:port (default port 7777).") },
			{ "team",     stringProp("Team name.") },
			{ "member",   stringProp("Member name.") },
			{ "password", stringProp("The member's password. Kept in memory only.") },
			{ "create",   json{ { "type", "boolean" },
			                    { "description", "Create the team instead of signing in." } } },
		}, { "server", "team", "member", "password" });
		t.mutates = true;
		t.handler = [tc](const json& args) -> ToolResult {
			const std::string server = strArg(args, "server");
			const std::string teamName = strArg(args, "team");
			const std::string member = strArg(args, "member");
			const std::string password = strArg(args, "password");
			const bool ok = boolArg(args, "create")
			                    ? tc->createTeam(server, teamName, member, password)
			                    : tc->signIn(server, teamName, member, password);
			if (!ok) return teamRefused(*tc, "sign_in_failed");
			return ToolResult::ok(teamStatusJson(*tc));
		};
		registry.add(std::move(t));
	}

	// ── team_sign_out ────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "team_sign_out";
		t.description =
			"Sign out of the team server. Sessions this editor announced end with the "
			"connection; a running collaboration session is left alone (collab_leave).";
		t.inputSchema = objectSchema(json::object(), {});
		t.mutates = true;
		t.handler = [tc](const json&) {
			tc->signOut();
			return ToolResult::ok(teamStatusJson(*tc));
		};
		registry.add(std::move(t));
	}

	// ── team_add_member ──────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "team_add_member";
		t.description =
			"Add a member to the signed-in team (admins only), as the Team panel's "
			"Add Member does. The server's answer is asynchronous: the member shows "
			"up in team_status's `members` once it is in.";
		t.inputSchema = objectSchema(json{
			{ "name",     stringProp("New member's name.") },
			{ "password", stringProp("New member's password.") },
			{ "admin",    json{ { "type", "boolean" },
			                    { "description", "Make the new member an admin." } } },
		}, { "name", "password" });
		t.mutates = true;
		t.handler = [tc](const json& args) -> ToolResult {
			if (!tc->addMember(strArg(args, "name"), strArg(args, "password"),
			                   boolArg(args, "admin")))
				return teamRefused(*tc, "add_member_failed");
			return ToolResult::ok(teamStatusJson(*tc));
		};
		registry.add(std::move(t));
	}

	// ── team_host ────────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "team_host";
		t.description =
			"Host a collaboration session through the team server and announce it to "
			"the team, as the Team panel's Host button does. `open` lets members join "
			"without an invitation. collab_status shows the session itself.";
		t.inputSchema = objectSchema(json{
			{ "open", json{ { "type", "boolean" },
			                { "description", "Members may join without an invitation." } } },
		}, {});
		t.mutates = true;
		t.handler = [tc, hooks](const json& args) -> ToolResult {
			if (!hooks.projectOpen || !hooks.projectOpen()) return failNoProject();
			if (!tc->hostSession(boolArg(args, "open"))) return teamRefused(*tc, "host_failed");
			return ToolResult::ok(teamStatusJson(*tc));
		};
		registry.add(std::move(t));
	}

	// ── team_set_open ────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "team_set_open";
		t.description = "Allow or stop members joining the hosted team session without an "
		                "invitation.";
		t.inputSchema = objectSchema(json{
			{ "open", json{ { "type", "boolean" },
			                { "description", "Members may join without an invitation." } } },
		}, { "open" });
		t.mutates = true;
		t.handler = [tc](const json& args) -> ToolResult {
			if (!tc->setSessionOpen(boolArg(args, "open"))) return teamRefused(*tc, "set_open_failed");
			return ToolResult::ok(teamStatusJson(*tc));
		};
		registry.add(std::move(t));
	}

	// ── team_invite ──────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "team_invite";
		t.description =
			"Invite a member to the session this editor hosts, as the Team panel's "
			"Invite button does. The member's editors list the session as `invited`.";
		t.inputSchema = objectSchema(json{
			{ "member", stringProp("Member to invite.") },
		}, { "member" });
		t.mutates = true;
		t.handler = [tc](const json& args) -> ToolResult {
			if (!tc->invite(strArg(args, "member"))) return teamRefused(*tc, "invite_failed");
			return ToolResult::ok(teamStatusJson(*tc));
		};
		registry.add(std::move(t));
	}

	// ── team_join / team_decline ─────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "team_join";
		t.description =
			"Accept an invitation or join an open team session, as the Team panel's "
			"Join button does: JOIN on the team server, then the relay connect. Poll "
			"collab_status for `joined`; the host's scene then REPLACES this "
			"editor's world.";
		t.inputSchema = objectSchema(json{
			{ "session", stringProp("Session id from team_status's `sessions`.") },
		}, { "session" });
		t.mutates = true;
		t.handler = [tc, hooks](const json& args) -> ToolResult {
			if (!hooks.projectOpen || !hooks.projectOpen()) return failNoProject();
			if (!tc->join(strArg(args, "session"))) return teamRefused(*tc, "join_failed");
			return ToolResult::ok(teamStatusJson(*tc));
		};
		registry.add(std::move(t));
	}
	{
		McpTool t;
		t.name        = "team_decline";
		t.description = "Decline an invitation to a team session; the host is told.";
		t.inputSchema = objectSchema(json{
			{ "session", stringProp("Session id from team_status's `sessions`.") },
		}, { "session" });
		t.mutates = true;
		t.handler = [tc](const json& args) -> ToolResult {
			if (!tc->decline(strArg(args, "session"))) return teamRefused(*tc, "decline_failed");
			return ToolResult::ok(teamStatusJson(*tc));
		};
		registry.add(std::move(t));
	}
}

} // namespace HE::Ed
