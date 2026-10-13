#pragma once

// ─── Team subsystem (topic 197) ──────────────────────────────────────────────
// The editor's half of the team server: one HE::Net::TeamClient, kept signed
// in (heartbeat, reconnect), and the glue that turns team actions into a collab
// session on the CollabController next to it:
//
//   host     CollabController::startTeamHosting(team server)  → OPEN_SESSION
//   invite   INVITE(member)              → the member's editors get "invited"
//   join     JOIN(session)  → CollabController::joinTeamSession(team server, …)
//   decline  DECLINE(session)            → the host gets "declined"
//   leave    the collab session ends     → CLOSE_SESSION
//
// The session itself is the relay session from topic 193 on the same server;
// nothing here touches its bytes. The team server is the relay: one address.
//
// Poll-driven: update() once per frame, after nothing else in particular — it
// only reads CollabController's state and calls its entry points. No ImGui;
// the Team panel and the Preferences page sit on top (next step of the topic).
//
// The password is kept in memory for the reconnect and never written to the
// editor config; signOut() forgets it.

#include "NotificationStore.h"

#include <Net/TeamClient.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

class CollabController;

class TeamController
{
public:
	enum class Status : std::uint8_t
	{
		SignedOut,     // nothing running
		Connecting,    // first login under way
		SignedIn,      // logged in; members and sessions are live
		Reconnecting,  // was signed in, the link dropped, retrying with backoff
		Failed,        // login refused or the address unusable — see lastError()
	};

	explicit TeamController(CollabController& collab);
	~TeamController();

	TeamController(const TeamController&)            = delete;
	TeamController& operator=(const TeamController&) = delete;

	// Invitations and "X is joining" go here as well as to pollNotice(), so
	// they are seen even while no Team panel is open. Null in tests.
	void setNotifications(HE::Ed::NotificationStore* store) { m_notes = store; }

	// ── Signing in ──
	// `server` is what a person types ("host:port", "host", "[v6]:port";
	// default port 7777, see HE::Net::parseRelayAddress). False with
	// lastError() when the address or the names cannot be used; the login
	// itself completes asynchronously — watch status().
	bool signIn(const std::string& server, const std::string& team,
	            const std::string& member, const std::string& password);
	// Same, but creates the team with `member` as its first admin.
	bool createTeam(const std::string& server, const std::string& team,
	                const std::string& member, const std::string& password);
	// Closes the connection (announced sessions end with it) and forgets the
	// password. Leaves a running collab session alone.
	void signOut();

	void update(std::uint64_t nowMs);

	Status             status() const { return m_status; }
	bool               signedIn() const { return m_status == Status::SignedIn; }
	const std::string& lastError() const { return m_error; }
	const std::string& serverText() const { return m_serverText; }
	const std::string& team() const { return m_team; }
	// Our member name, in the server's spelling once the roster is in.
	const std::string& member() const;
	bool               isAdmin() const { return m_client && m_client->isAdmin(); }

	// The live lists (empty while not signed in) and a counter that moves
	// whenever they do.
	const std::vector<HE::Net::TeamMember>&      members() const;
	const std::vector<HE::Net::TeamSessionInfo>& sessions() const;
	std::uint64_t revision() const { return m_client ? m_client->revision() : 0; }

	// ── Sessions ──
	// Host a collab session through the team server and announce it to the
	// team. `open`: members may join without an invitation. False (with
	// lastError()) when not signed in or a collab session is already running.
	bool hostSession(bool open);
	// The session this editor announced, or empty.
	const std::string& hostedSession() const { return m_hosted; }
	bool               hostedSessionOpen() const { return m_hostedOpen; }
	bool setSessionOpen(bool open);
	bool invite(const std::string& member);
	// Accept an invitation or join an open session: JOIN on the team server,
	// then the relay connect. False only when it cannot even be asked (not
	// signed in, a session already running); a refusal arrives as a notice.
	bool join(const std::string& session);
	bool decline(const std::string& session);

	// ── Members (admins) ──
	bool addMember(const std::string& name, const std::string& password, bool admin);
	bool removeMember(const std::string& name);
	bool changePassword(const std::string& oldPassword, const std::string& newPassword);

	// What happened that the user should read: refusals, invitations, someone
	// joining or declining. `error` marks the ones that are failures.
	struct Notice
	{
		std::string text;
		bool        error = false;
	};
	bool pollNotice(Notice& out);

	// Reconnect backoff bounds.
	static constexpr std::uint64_t kRetryMinMs = 1'000;
	static constexpr std::uint64_t kRetryMaxMs = 30'000;

private:
	bool begin(const std::string& server, const std::string& team, const std::string& member,
	           const std::string& password, HE::Net::TeamClient::LoginOp op);
	bool connectNow();
	void onLoggedIn();
	void handleEvent(const HE::Net::TeamClient::Event& ev);
	void notice(std::string text, bool error);
	bool requireSignedIn(const char* what);
	// A reply callback that turns a refusal into a notice. Captures no `this`
	// beyond the controller's lifetime: the client it is handed to is owned here.
	HE::Net::TeamClient::ReplyFn reportFailure(std::string what);

	CollabController&            m_collab;
	HE::Ed::NotificationStore*   m_notes = nullptr;
	std::unique_ptr<HE::Net::TeamClient> m_client;

	Status                m_status = Status::SignedOut;
	std::string           m_error;
	std::string           m_serverText;    // as typed, for the UI
	HE::Net::RelayAddress m_server;
	std::string           m_team;
	std::string           m_member;
	std::string           m_password;      // memory only, for the reconnect
	HE::Net::TeamClient::LoginOp m_op = HE::Net::TeamClient::LoginOp::Login;
	bool                  m_wasLoggedIn = false;

	std::uint64_t         m_nowMs       = 0;
	std::uint64_t         m_retryAtMs   = 0;
	std::uint64_t         m_backoffMs   = kRetryMinMs;

	std::string           m_hosted;        // announced session id
	bool                  m_hostedOpen = false;
	std::string           m_joining;       // JOIN sent, reply pending

	std::deque<Notice>    m_notices;
};
