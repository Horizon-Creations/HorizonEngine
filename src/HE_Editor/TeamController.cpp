#include "TeamController.h"

#include "CollabController.h"

#include <Diagnostics/Logger.h>
#include <Net/RelayTransport.h>

#include <algorithm>
#include <utility>

using HE::Net::RelayTransport;
using HE::Net::TeamClient;

namespace
{
// Bounded so a server that pushes the same refusal forever cannot grow it;
// the panel drains it every frame anyway.
constexpr std::size_t kMaxNotices = 64;

void wipe(std::string& secret)
{
	std::fill(secret.begin(), secret.end(), '\0');
	secret.clear();
}
} // namespace

TeamController::TeamController(CollabController& collab) : m_collab(collab) {}

TeamController::~TeamController() { signOut(); }

// ─── Signing in ──────────────────────────────────────────────────────────────

bool TeamController::signIn(const std::string& server, const std::string& team,
                            const std::string& member, const std::string& password)
{
	return begin(server, team, member, password, TeamClient::LoginOp::Login);
}

bool TeamController::createTeam(const std::string& server, const std::string& team,
                                const std::string& member, const std::string& password)
{
	return begin(server, team, member, password, TeamClient::LoginOp::CreateTeam);
}

bool TeamController::begin(const std::string& server, const std::string& team,
                           const std::string& member, const std::string& password,
                           TeamClient::LoginOp op)
{
	signOut();

	std::string why;
	HE::Net::RelayAddress address;
	if (!HE::Net::parseRelayAddress(server, address, &why))
	{
		m_error  = server.empty() ? std::string("Enter the team server's address first.")
		                          : "The team server address \"" + server + "\" cannot be used (" + why + ").";
		m_status = Status::Failed;
		return false;
	}
	if (!RelayTransport::isValidId(team) || !RelayTransport::isValidId(member))
	{
		m_error  = "Team and member names are 1-64 letters, digits, - or _.";
		m_status = Status::Failed;
		return false;
	}
	if (password.empty() || password.size() > 255)
	{
		m_error  = password.empty() ? "Enter a password." : "That password is too long (255 bytes at most).";
		m_status = Status::Failed;
		return false;
	}

	m_serverText = server;
	m_server     = address;
	m_team       = team;
	m_member     = member;
	m_password   = password;
	m_op         = op;
	m_backoffMs  = kRetryMinMs;
	if (!connectNow())
	{
		m_status = Status::Failed;
		wipe(m_password);
		return false;
	}
	m_status = Status::Connecting;
	return true;
}

bool TeamController::connectNow()
{
	m_client = TeamClient::connect(m_server, m_team, m_member, m_password, m_op);
	if (!m_client)
	{
		m_error = "Could not open a connection to the team server at " + m_server.host + ":" +
		          std::to_string(m_server.port) + ".";
		return false;
	}
	return true;
}

void TeamController::signOut()
{
	if (m_client) m_client->close();
	m_client.reset();
	wipe(m_password);
	m_status      = Status::SignedOut;
	m_error.clear();
	m_wasLoggedIn = false;
	m_hosted.clear();
	m_joining.clear();
}

const std::string& TeamController::member() const
{
	return m_client ? m_client->member() : m_member;
}

const std::vector<HE::Net::TeamMember>& TeamController::members() const
{
	static const std::vector<HE::Net::TeamMember> kNone;
	return m_client ? m_client->members() : kNone;
}

const std::vector<HE::Net::TeamSessionInfo>& TeamController::sessions() const
{
	static const std::vector<HE::Net::TeamSessionInfo> kNone;
	return m_client ? m_client->sessions() : kNone;
}

// ─── Pump ────────────────────────────────────────────────────────────────────

void TeamController::update(std::uint64_t nowMs)
{
	m_nowMs = nowMs;

	if (m_client)
	{
		m_client->update(nowMs);

		TeamClient::Event ev;
		while (m_client->pollEvent(ev)) handleEvent(ev);

		if (m_client->loggedIn() && m_status != Status::SignedIn) onLoggedIn();

		if (m_client->state() == TeamClient::State::Closed)
		{
			m_error = m_client->lastError();
			const bool refused = m_client->loginRefused();
			m_client.reset();
			if (refused || !m_wasLoggedIn)
			{
				// Same name and password again would be refused again, and a
				// first login that never got through is for the person to retry.
				m_status = Status::Failed;
				wipe(m_password);
				m_hosted.clear();
				if (m_wasLoggedIn) notice(m_error, true);
				m_wasLoggedIn = false;
			}
			else
			{
				if (m_status == Status::SignedIn)
					notice("Lost the connection to the team server; reconnecting.", true);
				m_status    = Status::Reconnecting;
				m_retryAtMs = nowMs + m_backoffMs;
				m_backoffMs = std::min(m_backoffMs * 2, kRetryMaxMs);
			}
		}
	}
	else if (m_status == Status::Reconnecting && nowMs >= m_retryAtMs)
	{
		if (!connectNow())
		{
			m_retryAtMs = nowMs + m_backoffMs;
			m_backoffMs = std::min(m_backoffMs * 2, kRetryMaxMs);
		}
	}

	// The announcement follows the collab session: whichever way it ended
	// (left, kicked, failed, replaced by a join), the team stops seeing it.
	if (!m_hosted.empty())
	{
		const bool stillHosting = m_collab.active() && m_collab.isHost() &&
		                          m_collab.teamSession() && m_collab.sessionId() == m_hosted;
		if (!stillHosting)
		{
			if (m_client && m_client->loggedIn()) m_client->closeSession(m_hosted);
			m_hosted.clear();
		}
	}
}

void TeamController::onLoggedIn()
{
	m_status      = Status::SignedIn;
	m_error.clear();
	m_wasLoggedIn = true;
	m_backoffMs   = kRetryMinMs;
	// A team is created once; every reconnect after that is a login.
	m_op          = TeamClient::LoginOp::Login;
	Logger::Log(Logger::LogLevel::Info,
	            ("Team: signed in to '" + m_team + "' as '" + m_member + "'").c_str());

	// The announcement died with the old connection while the session itself
	// kept running on the relay: announce it again so the team can find it.
	if (!m_hosted.empty() && m_collab.active() && m_collab.isHost() &&
	    m_collab.sessionId() == m_hosted)
	{
		m_client->openSession(m_hosted, RelayTransport::kHostPeerId, m_hostedOpen,
		                      reportFailure("Could not announce the session again"));
	}
}

void TeamController::handleEvent(const TeamClient::Event& ev)
{
	switch (ev.kind)
	{
	case TeamClient::EventKind::Invited:
		notice(ev.host + " invited you to a collaboration session.", false);
		break;
	case TeamClient::EventKind::Joining:
		if (ev.session == m_hosted) notice(ev.member + " is joining your session.", false);
		break;
	case TeamClient::EventKind::Declined:
		if (ev.session == m_hosted) notice(ev.member + " declined the invitation.", false);
		break;
	default:
		// Presence, roster and session changes are already in the lists.
		break;
	}
}

void TeamController::notice(std::string text, bool error)
{
	if (m_notes)
		m_notes->post(error ? HE::Ed::NoteLevel::Warning : HE::Ed::NoteLevel::Info, text);
	if (m_notices.size() >= kMaxNotices) m_notices.pop_front();
	m_notices.push_back(Notice{ std::move(text), error });
}

bool TeamController::pollNotice(Notice& out)
{
	if (m_notices.empty()) return false;
	out = std::move(m_notices.front());
	m_notices.pop_front();
	return true;
}

bool TeamController::requireSignedIn(const char* what)
{
	if (m_client && m_client->loggedIn()) return true;
	m_error = std::string("Sign in to the team server to ") + what + ".";
	return false;
}

TeamClient::ReplyFn TeamController::reportFailure(std::string what)
{
	// Runs from m_client->update(), inside update(): the controller is alive,
	// and nothing in here may reset m_client.
	return [this, what = std::move(what)](const TeamClient::Reply& r) {
		if (r.ok() || r.result == TeamClient::Result::ConnectionLost) return;
		notice(what + ": " + TeamClient::describeResult(r.result, r.text), true);
	};
}

// ─── Sessions ────────────────────────────────────────────────────────────────

bool TeamController::sessionRunning() const { return m_collab.active(); }

bool TeamController::inSession(const std::string& session) const
{
	return !session.empty() && m_collab.active() && m_collab.sessionId() == session;
}

bool TeamController::hostSession(bool open)
{
	if (!requireSignedIn("host a session")) return false;
	if (m_collab.active())
	{
		m_error = "A collaboration session is already running; leave it first.";
		return false;
	}
	if (!m_collab.startTeamHosting(m_server, member()))
	{
		m_error = m_collab.lastError();
		return false;
	}
	m_hosted     = m_collab.sessionId();
	m_hostedOpen = open;

	const std::string sid = m_hosted;
	m_client->openSession(sid, RelayTransport::kHostPeerId, open,
	                      [this, sid](const TeamClient::Reply& r) {
		// Lost before the answer: the reconnect announces it again.
		if (r.ok() || r.result == TeamClient::Result::ConnectionLost) return;
		notice("The team server did not take the session: " +
		           TeamClient::describeResult(r.result, r.text), true);
		// A team session nobody in the team can find is no use to anyone.
		if (m_hosted == sid)
		{
			m_hosted.clear();
			m_collab.leave();
		}
	});
	return true;
}

bool TeamController::setSessionOpen(bool open)
{
	if (!requireSignedIn("change the session")) return false;
	if (m_hosted.empty())
	{
		m_error = "This editor is not hosting a team session.";
		return false;
	}
	m_hostedOpen = open;
	m_client->setSessionOpen(m_hosted, open, reportFailure("Could not change who may join"));
	return true;
}

bool TeamController::invite(const std::string& member)
{
	if (!requireSignedIn("invite")) return false;
	if (m_hosted.empty())
	{
		m_error = "Host a session first, then invite.";
		return false;
	}
	m_client->invite(m_hosted, member, reportFailure("Could not invite " + member));
	return true;
}

bool TeamController::join(const std::string& session)
{
	if (!requireSignedIn("join a session")) return false;
	if (m_collab.active())
	{
		m_error = "A collaboration session is already running; leave it first.";
		return false;
	}
	m_joining = session;
	m_client->join(session, [this](const TeamClient::Reply& r) {
		m_joining.clear();
		if (r.result == TeamClient::Result::ConnectionLost) return;
		if (!r.ok())
		{
			notice("Could not join: " + TeamClient::describeResult(r.result, r.text), true);
			return;
		}
		// Our member name as the relay peer id, as the protocol recommends:
		// the host can then match the connection to the "joining" it was told.
		const std::string self = member();
		if (!m_collab.joinTeamSession(m_server, r.session, r.peer, self, self))
			notice("Could not join: " + m_collab.lastError(), true);
	});
	return true;
}

bool TeamController::decline(const std::string& session)
{
	if (!requireSignedIn("decline")) return false;
	m_client->decline(session, reportFailure("Could not decline"));
	return true;
}

// ─── Members ─────────────────────────────────────────────────────────────────

bool TeamController::addMember(const std::string& name, const std::string& password, bool admin)
{
	if (!requireSignedIn("add members")) return false;
	m_client->addMember(name, password, admin, [this, name](const TeamClient::Reply& r) {
		if (r.ok()) notice(name + " was added to the team.", false);
		else if (r.result != TeamClient::Result::ConnectionLost)
			notice("Could not add " + name + ": " + TeamClient::describeResult(r.result, r.text), true);
	});
	return true;
}

bool TeamController::removeMember(const std::string& name)
{
	if (!requireSignedIn("remove members")) return false;
	m_client->removeMember(name, [this, name](const TeamClient::Reply& r) {
		if (r.ok()) notice(name + " was removed from the team.", false);
		else if (r.result != TeamClient::Result::ConnectionLost)
			notice("Could not remove " + name + ": " + TeamClient::describeResult(r.result, r.text), true);
	});
	return true;
}

bool TeamController::changePassword(const std::string& oldPassword, const std::string& newPassword)
{
	if (!requireSignedIn("change the password")) return false;
	// The reconnect has to use the new one once the server has taken it.
	m_client->setPassword(oldPassword, newPassword, [this, newPassword](const TeamClient::Reply& r) {
		if (r.ok())
		{
			m_password = newPassword;
			notice("Password changed.", false);
		}
		else if (r.result != TeamClient::Result::ConnectionLost)
			notice("Could not change the password: " + TeamClient::describeResult(r.result, r.text), true);
	});
	return true;
}
