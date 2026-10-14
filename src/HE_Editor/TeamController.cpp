#include "TeamController.h"

#include "CollabController.h"

#include <Diagnostics/Logger.h>
#include <Net/RelayTransport.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

// ─── The token file ──────────────────────────────────────────────────────────
// One sign-in, five lines: a format line, then host, port, team, member and
// token. Names are the relay's id alphabet and the token is hex, so nothing
// needs quoting. One entry, not a list: signing in as somebody else replaces
// it (the old token is left to expire on the server after 90 days unused).
constexpr const char* kTokenFileFormat = "heteamtoken 1";

struct StoredSignIn
{
	std::string   host;
	std::uint16_t port = 0;
	std::string   team, member, token;
};

bool sameName(const std::string& a, const std::string& b)
{
	if (a.size() != b.size()) return false;
	for (std::size_t i = 0; i < a.size(); ++i)
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
			return false;
	return true;
}

bool isHexToken(const std::string& t)
{
	if (t.empty() || t.size() > 255) return false;
	return std::all_of(t.begin(), t.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; });
}

bool readSignIn(const std::filesystem::path& file, StoredSignIn& out)
{
	if (file.empty()) return false;
	std::ifstream in(file, std::ios::binary);
	if (!in) return false;
	std::string line, key, value;
	if (!std::getline(in, line) || line != kTokenFileFormat) return false;
	StoredSignIn s;
	while (std::getline(in, line))
	{
		std::istringstream ls(line);
		if (!(ls >> key >> value)) continue;
		if (key == "host") s.host = value;
		else if (key == "port")
		{
			const unsigned long p = std::strtoul(value.c_str(), nullptr, 10);
			s.port = p > 0 && p <= 65535 ? static_cast<std::uint16_t>(p) : 0;
		}
		else if (key == "team")   s.team   = value;
		else if (key == "member") s.member = value;
		else if (key == "token")  s.token  = value;
		wipe(line);
		wipe(value);
	}
	if (s.host.empty() || s.port == 0 || s.team.empty() || s.member.empty() || !isHexToken(s.token))
	{
		wipe(s.token);
		return false;
	}
	out = std::move(s);
	return true;
}

// Written beside the target and renamed over it, so a crash leaves the old
// file or the new one. Owner-only from the first byte on POSIX (open() with
// 0600, as McpBridge's endpoint file: an ofstream would create it 0644 and be
// corrected afterwards); on Windows the per-user profile directory is what
// restricts it, as for config.json next to it.
bool writeSignIn(const std::filesystem::path& file, const StoredSignIn& s)
{
	std::error_code ec;
	if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
	std::string text = std::string(kTokenFileFormat) + "\nhost " + s.host + "\nport " + std::to_string(s.port) +
	                   "\nteam " + s.team + "\nmember " + s.member + "\ntoken " + s.token + "\n";
	std::filesystem::path tmp = file;
	tmp += ".tmp";
	bool ok = false;
#ifndef _WIN32
	const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd >= 0)
	{
		::fchmod(fd, 0600);   // a leftover .tmp keeps its old mode through O_CREAT
		ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
		ok = (::fsync(fd) == 0) && ok;
		ok = (::close(fd) == 0) && ok;
	}
#else
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (out)
		{
			out.write(text.data(), static_cast<std::streamsize>(text.size()));
			out.close();
			ok = !out.fail();
		}
	}
#endif
	wipe(text);
	if (ok)
	{
		std::filesystem::rename(tmp, file, ec);
		ok = !ec;
	}
	if (!ok) std::filesystem::remove(tmp, ec);
	return ok;
}
} // namespace

TeamController::TeamController(CollabController& collab) : m_collab(collab) {}

TeamController::~TeamController() { disconnect(); }

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
	// Not signOut(): typing the password again (after a refused token, or to
	// switch member) is no reason to revoke anything.
	disconnect();

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
	m_secret     = password;
	m_op         = op;
	m_backoffMs  = kRetryMinMs;
	if (!connectNow())
	{
		m_status = Status::Failed;
		wipe(m_secret);
		return false;
	}
	m_status = Status::Connecting;
	return true;
}

bool TeamController::resume(const std::string& server, const std::string& team, const std::string& member)
{
	if (!hasSavedSignIn(server, team, member)) return false;
	StoredSignIn s;
	if (!readSignIn(m_tokenFile, s)) return false;
	disconnect();
	HE::Net::parseRelayAddress(server, m_server, nullptr);
	m_serverText = server;
	m_team       = team;
	m_member     = member;
	m_secret     = std::move(s.token);
	m_op         = TeamClient::LoginOp::Token;
	m_backoffMs  = kRetryMinMs;
	Logger::Log(Logger::LogLevel::Info,
	            ("Team: signing in to '" + team + "' as '" + member + "' with the saved session").c_str());
	if (!connectNow())
	{
		// The address resolved a moment ago; a refusal on the spot is the OS,
		// not the token, so it stays for the next try.
		m_status = Status::Failed;
		wipe(m_secret);
		m_op = TeamClient::LoginOp::Login;
		return false;
	}
	m_status = Status::Connecting;
	return true;
}

bool TeamController::hasSavedSignIn(const std::string& server, const std::string& team,
                                    const std::string& member) const
{
	HE::Net::RelayAddress address;
	if (!HE::Net::parseRelayAddress(server, address, nullptr)) return false;
	StoredSignIn s;
	if (!readSignIn(m_tokenFile, s)) return false;
	const bool match = s.host == address.host && s.port == address.port && sameName(s.team, team) &&
	                   sameName(s.member, member);
	wipe(s.token);
	return match;
}

std::string TeamController::storedToken() const
{
	StoredSignIn s;
	if (!readSignIn(m_tokenFile, s)) return {};
	if (s.host != m_server.host || s.port != m_server.port || !sameName(s.team, m_team) ||
	    !sameName(s.member, m_member))
	{
		wipe(s.token);
		return {};
	}
	return std::move(s.token);
}

void TeamController::forgetStoredToken(const std::string& token)
{
	if (m_tokenFile.empty()) return;
	std::string stored = storedToken();
	if (stored.empty() || (!token.empty() && stored != token))
	{
		wipe(stored);
		return;
	}
	wipe(stored);
	std::error_code ec;
	std::filesystem::remove(m_tokenFile, ec);
	if (ec)
		Logger::Log(Logger::LogLevel::Warning,
		            ("Team: could not delete the saved sign-in " + m_tokenFile.string() + ": " + ec.message()).c_str());
}

bool TeamController::connectNow()
{
	m_client = TeamClient::connect(m_server, m_team, m_member, m_secret, m_op);
	if (!m_client)
	{
		m_error = "Could not open a connection to the team server at " + m_server.host + ":" +
		          std::to_string(m_server.port) + ".";
		return false;
	}
	return true;
}

void TeamController::disconnect()
{
	if (m_client) m_client->close();
	m_client.reset();
	if (m_leaving) m_leaving->close();
	m_leaving.reset();
	wipe(m_secret);
	m_op          = TeamClient::LoginOp::Login;
	m_status      = Status::SignedOut;
	m_error.clear();
	m_wasLoggedIn = false;
	m_hosted.clear();
	m_joining.clear();
}

void TeamController::signOut()
{
	// The token this editor holds: the one it signed in with, else the one it
	// stored after a password login whose reconnect credential is still the
	// password (the token file could not be written).
	std::string token = usingToken() ? m_secret : storedToken();
	forgetStoredToken(token);
	const bool canRevoke = !token.empty() && m_client && m_client->loggedIn();
	if (canRevoke) leaveRevoking(false, token);
	wipe(token);
	std::unique_ptr<TeamClient> leaving = std::move(m_leaving);
	disconnect();
	m_leaving = std::move(leaving);
}

bool TeamController::signOutEverywhere()
{
	if (!requireSignedIn("sign out everywhere")) return false;
	std::string token = usingToken() ? m_secret : storedToken();
	forgetStoredToken(token);
	wipe(token);
	leaveRevoking(true, {});
	std::unique_ptr<TeamClient> leaving = std::move(m_leaving);
	disconnect();
	m_leaving = std::move(leaving);
	return true;
}

void TeamController::leaveRevoking(bool all, const std::string& token)
{
	// A frame still queued when the socket closes is dropped (TcpTransport::
	// disconnect), so the connection lives on in m_leaving until the server
	// has answered. A late or lost answer only costs the server-side cleanup:
	// the token is already gone from this machine.
	if (m_leaving) m_leaving->close();
	m_leaving        = std::move(m_client);
	m_leavingDone    = false;
	m_leavingUntilMs = m_nowMs + kRevokeGraceMs;
	auto done = [this, all](const TeamClient::Reply& r) {
		m_leavingDone = true;
		if (r.ok())
			Logger::Log(Logger::LogLevel::Info,
			            all ? ("Team: signed out on every device (" + r.text + ")").c_str()
			                : "Team: session token revoked on the server");
		else if (r.result != TeamClient::Result::ConnectionLost)
			Logger::Log(Logger::LogLevel::Warning,
			            ("Team: the server did not revoke the session token: " +
			             std::string(TeamClient::resultName(static_cast<std::uint8_t>(r.result))))
			                .c_str());
	};
	if (all) m_leaving->revokeAllTokens(done);
	else     m_leaving->revokeToken(token, done);
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

	if (m_leaving)
	{
		if (m_leavingUntilMs == 0) m_leavingUntilMs = nowMs + kRevokeGraceMs;   // signed out before any update
		m_leaving->update(nowMs);
		if (m_leavingDone || m_leaving->state() == TeamClient::State::Closed || nowMs >= m_leavingUntilMs)
		{
			m_leaving->close();
			m_leaving.reset();
		}
	}

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
			const std::string why = m_client->closeText();
			m_client.reset();
			if (refused && usingToken())
			{
				// A token is refused for good: revoked (Sign Out elsewhere,
				// password changed, member removed) or unused too long. The
				// server says which only to a live connection ("session token
				// revoked", "password changed"); a refused login only gets its
				// generic "wrong team, member or password", which would mislead
				// here. Either way it is the password next.
				forgetStoredToken(m_secret);
				m_error = "The saved sign-in is no longer valid";
				m_error += !m_wasLoggedIn || why.empty() ? std::string(".") : " (" + why + ").";
				m_error += " Sign in with your password.";
				// At start nobody has the Team window open, and the editor was
				// expected to be signed in: say so where it is seen. (A live
				// connection gets its notice below.)
				if (!m_wasLoggedIn) notice(m_error, true);
				Logger::Log(Logger::LogLevel::Info,
				            ("Team: the saved session was refused" + (why.empty() ? std::string() : " — " + why)).c_str());
			}
			const bool retryToken = usingToken() && !refused;
			if (refused || (!m_wasLoggedIn && !retryToken))
			{
				// Same name and password again would be refused again, and a
				// first login that never got through is for the person to retry.
				// (A saved token is the exception: the server being down at
				// start is no reason to forget it, so that keeps retrying.)
				m_status = Status::Failed;
				wipe(m_secret);
				m_op = TeamClient::LoginOp::Login;
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
	Logger::Log(Logger::LogLevel::Info,
	            ("Team: signed in to '" + m_team + "' as '" + m_member + "'" +
	             (usingToken() ? " with the saved session" : "")).c_str());
	// A team is created once; every reconnect after that is a login — with a
	// token from here on if the server hands one out.
	if (!usingToken())
	{
		m_op = TeamClient::LoginOp::Login;
		requestToken();
	}

	// The announcement died with the old connection while the session itself
	// kept running on the relay: announce it again so the team can find it.
	if (!m_hosted.empty() && m_collab.active() && m_collab.isHost() &&
	    m_collab.sessionId() == m_hosted)
	{
		m_client->openSession(m_hosted, RelayTransport::kHostPeerId, m_hostedOpen,
		                      reportFailure("Could not announce the session again"));
	}
}

void TeamController::requestToken()
{
	if (m_tokenFile.empty() || !m_client) return;
	m_client->issueToken([this](const TeamClient::Reply& r) {
		// Runs inside m_client->update(): m_client is alive and stays so.
		if (!r.ok())
		{
			// A server from before tokens answers bad-request: then it is the
			// password in memory, as before, and again after a restart.
			if (r.result != TeamClient::Result::ConnectionLost)
				Logger::Log(Logger::LogLevel::Info,
				            ("Team: no session token from the server (" +
				             std::string(TeamClient::resultName(static_cast<std::uint8_t>(r.result))) +
				             "); the password is needed again after a restart").c_str());
			return;
		}
		StoredSignIn s;
		s.host   = m_server.host;
		s.port   = m_server.port;
		s.team   = m_team;
		s.member = m_client ? m_client->member() : m_member;   // the server's spelling
		s.token  = r.token;
		if (!writeSignIn(m_tokenFile, s))
			Logger::Log(Logger::LogLevel::Warning,
			            ("Team: could not save the sign-in to " + m_tokenFile.string() +
			             "; the password is needed again after a restart").c_str());
		wipe(s.token);
		// Reconnects use the token from now on; the password leaves memory.
		wipe(m_secret);
		m_secret = r.token;
		m_op     = TeamClient::LoginOp::Token;
	});
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
			// SET_PASSWORD revoked every token of ours, the stored one too,
			// and this connection may ask for a new one now (also when it
			// logged in with a token: it has proven the password).
			forgetStoredToken(usingToken() ? m_secret : std::string());
			wipe(m_secret);
			m_secret = newPassword;
			m_op     = TeamClient::LoginOp::Login;
			requestToken();
			notice("Password changed.", false);
		}
		else if (r.result != TeamClient::Result::ConnectionLost)
			notice("Could not change the password: " + TeamClient::describeResult(r.result, r.text), true);
	});
	return true;
}
