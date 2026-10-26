#include "TeamSyncController.h"

#include "CollabController.h"
#include "GitController.h"
#include "NotificationStore.h"
#include "TeamController.h"

#include <Net/TeamClient.h>

#include <cctype>

namespace {

std::string folded(std::string s)
{
	for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

bool caseInsensitiveEqual(const std::string& a, const std::string& b)
{
	if (a.size() != b.size()) return false;
	for (std::size_t i = 0; i < a.size(); ++i)
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
			return false;
	return true;
}

std::string plural(std::size_t n, const char* one, const char* many)
{
	return std::to_string(n) + " " + (n == 1 ? one : many);
}

} // namespace

TeamSyncController::TeamSyncController(TeamController& team, GitController& git,
                                       const CollabController& collab)
	: m_team(team), m_git(git), m_collab(collab)
{
	// The session host's settled state (CollabSettle) arrives here as keys;
	// the service uploads whatever differs, so the keys only say "now".
	m_git.setTeamServerUpload([this](const std::vector<std::string>&) {
		if (!m_sync.running() || m_blocker != Blocker::None) return false;
		m_sync.requestSync();
		return true;
	});
	m_team.setOnScHead([this](const std::string& project, std::uint64_t rev, const std::string&) {
		if (m_sync.running() && caseInsensitiveEqual(project, m_project.project)) m_sync.notifyHead(rev);
	});
}

TeamSyncController::~TeamSyncController()
{
	m_git.setTeamServerUpload({});
	m_team.setOnScHead({});
	m_sync.stop();
}

void TeamSyncController::close()
{
	m_sync.stop();
	m_key.clear();
	m_project  = {};
	m_saveAtMs = 0;
	m_wasClient = m_wasHost = false;
}

void TeamSyncController::noteSaved()
{
	if (!m_sync.running()) return;
	m_saveAtMs = m_nowMs + kDebounceMs;
}

void TeamSyncController::syncNow(const std::string& message)
{
	m_saveAtMs = 0;
	m_sync.requestSync(message);
}

void TeamSyncController::restart(const Project& project)
{
	m_sync.stop();
	m_project = project;
	HE::Sc::TeamSyncService::Settings s;
	s.projectRoot = project.root;
	s.host        = m_team.serverAddress().host;
	s.port        = m_team.serverAddress().port;
	s.team        = m_team.team();
	s.member      = m_team.member();
	s.project     = project.project;
	TeamController* team = &m_team;
	// The reply runs inside TeamController::update, so the controller is alive;
	// the service's own half is behind a weak_ptr (Deliver).
	m_sync.start(s, [team](const std::string& name, HE::Sc::TeamSyncService::Deliver deliver) {
		return team->requestScTicket(name, [team, deliver](const HE::Net::TeamClient::Reply& r) {
			deliver(r.ticket,
			        r.ok() ? std::string() : HE::Net::TeamClient::describeResult(r.result, r.text),
			        r.result == HE::Net::TeamClient::Result::NoSuchProject, team->isAdmin());
		});
	});
	m_wasClient = m_wasHost = false;
	m_wasSignedIn = m_team.signedIn();
}

std::vector<std::string> TeamSyncController::update(std::uint64_t nowMs, const Project& project,
                                                    bool playing)
{
	m_nowMs = nowMs;
	if (!project.teamServer || project.root.empty() || project.team.empty() || project.project.empty())
	{
		m_blocker = Blocker::NotTeamMode;
		if (m_sync.running()) close();
		return {};
	}
	m_project.team    = project.team;
	m_project.project = project.project;

	// A connection that is up, or being brought back, counts: the service
	// waits for its ticket. Signed out (or failed) there is nobody to sync as.
	const TeamController::Status ts = m_team.status();
	const bool haveSignIn = ts == TeamController::Status::SignedIn ||
	                        ts == TeamController::Status::Reconnecting ||
	                        (ts == TeamController::Status::Connecting && !m_team.team().empty());
	if (!haveSignIn || !caseInsensitiveEqual(m_team.team(), project.team))
	{
		m_blocker = haveSignIn ? Blocker::OtherTeam : Blocker::SignedOut;
		if (m_sync.running()) m_sync.stop();
		m_key.clear();
		return {};
	}
	m_blocker = Blocker::None;

	const auto& at = m_team.serverAddress();
	const std::string key = project.root.generic_string() + "\n" + folded(project.team) + "\n" +
	                        folded(project.project) + "\n" + folded(m_team.member()) + "\n" + at.host +
	                        ":" + std::to_string(at.port);
	if (key != m_key || !m_sync.running())
	{
		m_key = key;
		restart(project);
	}

	// The team connection came back: an sc-head may have gone by meanwhile.
	const bool signedIn = m_team.signedIn();
	if (signedIn && !m_wasSignedIn) m_sync.refresh();
	m_wasSignedIn = signedIn;

	// Live session roles (design section 2, audit 3.1 and 3.8).
	const bool inSession = m_collab.inSession();
	const bool client    = inSession && !m_collab.isHost();
	const bool host      = inSession && m_collab.isHost();
	if (client != m_wasClient) m_sync.setPaused(client, /*adoptTeam=*/!client);
	if (host != m_wasHost) m_sync.setHoldIncoming(host);
	m_wasClient = client;
	m_wasHost   = host;

	if (m_saveAtMs != 0 && nowMs >= m_saveAtMs)
	{
		m_saveAtMs = 0;
		m_sync.requestSync();
	}

	std::vector<std::string> written = m_sync.pump(!playing, m_keepLocal);

	std::string text;
	bool        error = false;
	while (m_sync.pollNotice(text, error))
		if (m_notes)
			m_notes->post(error ? HE::Ed::NoteLevel::Important : HE::Ed::NoteLevel::Info, "Team sync: " + text);

	std::filesystem::path opened;
	while (m_sync.takeOpened(opened))
	{
		if (m_notes)
			m_notes->post(HE::Ed::NoteLevel::Info, "Your version is saved in " + opened.generic_string());
		// The folder, not the file: a scene or asset opened by the OS would land
		// in whatever app claims the extension, maybe a second editor.
		if (m_openFolder) m_openFolder(opened.parent_path());
	}
	return written;
}

std::string TeamSyncController::stateLine() const
{
	switch (m_blocker)
	{
	case Blocker::NotTeamMode: return {};
	case Blocker::SignedOut:
		return "Sign in to team \"" + m_project.team + "\" (Window > Team) to sync this project.";
	case Blocker::OtherTeam:
		return "Signed in to team \"" + m_team.team() + "\", but this project belongs to team \"" +
		       m_project.team + "\".";
	case Blocker::None: break;
	}
	using Phase = HE::Sc::TeamSyncService::Phase;
	const HE::Sc::TeamSyncService::Status s = m_sync.status();
	const std::string waiting = s.waiting ? " " + plural(s.waiting, "change", "changes") + " waiting." : "";
	if (s.paused) return "Paused: in this live session the host saves for everyone.";
	if (!s.firstSyncDiffers.empty())
		return "First sync: " + plural(s.firstSyncDiffers.size(), "file differs", "files differ") +
		       " from the team's version.";
	if (s.projectMissing)
		return s.isAdmin ? "The project is not on the team server yet."
		                 : "The project is not on the team server yet; a team admin creates it.";
	switch (s.phase)
	{
	case Phase::Off:           return "Not running.";
	case Phase::WaitingTicket: return "Waiting for the team connection." + waiting;
	case Phase::Connecting:    return "Connecting to the team server...";
	case Phase::Working:       return s.activity.empty() ? std::string("Syncing...") : s.activity;
	case Phase::Waiting:       return "Waiting." + waiting;
	case Phase::Offline:
		return "Offline: " + (s.error.empty() ? std::string("the team server cannot be reached.") : s.error) + waiting;
	case Phase::Idle:
		if (s.waiting) return plural(s.waiting, "change", "changes") + " not on the team server yet (r" +
		                      std::to_string(s.rev) + ").";
		return "In sync with the team at r" + std::to_string(s.rev) + ".";
	}
	return {};
}
