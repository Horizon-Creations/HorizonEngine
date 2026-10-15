#pragma once
// ─── Saving through the team server: the editor's glue (topic 199) ───────────
// With Config/ProjectSettings.json → sourceControl.mode = "teamServer" every
// save goes to the team server, which merges it with everybody else's
// (HE::Sc::TeamSyncService does the work; design:
// docs/team-server-source-control-design-2026-10-13.md). This class only
// decides when, and with whose sign-in:
//
//   * the team connection is TeamController's: its server, its member, its
//     tickets (SC_TICKET) and its sc-head events. Signed out, or signed in to
//     another team than the project names, nothing syncs and the panel says so.
//   * a save (scene, asset, settings) arms a one-second debounce, so "Save All"
//     is one revision; GitController's team-server slot (the session host's
//     settled state) and "Sync now" trigger it too.
//   * a live session: a client neither uploads nor pulls (the host saves for
//     everyone); when the session ends it takes the team's version of whatever
//     the session changed here. The host uploads, but files from the server
//     wait until the session is over (they would fork it from the peers).
//   * files from the server are written on the main thread, never during Play;
//     update() returns them so the editor reloads what is open.
#include <SourceControl/TeamSyncService.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class CollabController;
class GitController;
class TeamController;
namespace HE::Ed { class NotificationStore; }

class TeamSyncController
{
public:
	TeamSyncController(TeamController& team, GitController& git, const CollabController& collab);
	~TeamSyncController();

	TeamSyncController(const TeamSyncController&)            = delete;
	TeamSyncController& operator=(const TeamSyncController&) = delete;

	void setNotifications(HE::Ed::NotificationStore* store) { m_notes = store; }
	// The editor's "I hold unsaved edits for this file" (a dirty scene, a dirty
	// asset tab): such a file is not overwritten from the server, see
	// TeamSyncService::pump.
	void setKeepLocal(HE::Sc::TeamSyncService::KeepLocalFn fn) { m_keepLocal = std::move(fn); }
	// Shows a folder in the OS file manager (the editor: SDL_OpenURL). "Open
	// My Version" opens the folder its download landed in.
	void setOpenFolder(std::function<void(const std::filesystem::path&)> fn) { m_openFolder = std::move(fn); }

	// The open project as far as syncing goes. teamServer false (or no root):
	// nothing runs.
	struct Project
	{
		std::filesystem::path root;
		bool                  teamServer = false;
		std::string           team, project;
	};

	// Once per frame, after TeamController::update and the collab pump.
	// `playing`: Play mode, files from the server wait. Returns the
	// project-relative paths written or removed this frame.
	std::vector<std::string> update(std::uint64_t nowMs, const Project& project, bool playing);

	// A save landed on disk (scene, asset, settings): sync kDebounceMs after
	// the last update()'s clock, so a run of saves is one revision.
	void noteSaved();
	// "Sync now", with an optional note for the revision.
	void syncNow(const std::string& message = {});
	// The project closed: stop, forget.
	void close();

	// Why nothing syncs, for the panel.
	enum class Blocker : std::uint8_t
	{
		None,          // syncing (or ready to)
		NotTeamMode,   // the project uses git directly
		SignedOut,     // no team sign-in (Window ▸ Team)
		OtherTeam,     // signed in to a different team than the project names
	};
	Blocker            blocker() const { return m_blocker; }
	bool               running() const { return m_sync.running(); }
	const std::string& team() const { return m_project.team; }
	const std::string& projectName() const { return m_project.project; }

	HE::Sc::TeamSyncService&       service() { return m_sync; }
	const HE::Sc::TeamSyncService& service() const { return m_sync; }

	// One sentence for the panel header and the footer.
	std::string stateLine() const;

	static constexpr std::uint64_t kDebounceMs = 1000;

private:
	void restart(const Project& project);

	TeamController&            m_team;
	GitController&             m_git;
	const CollabController&    m_collab;
	HE::Ed::NotificationStore* m_notes = nullptr;
	HE::Sc::TeamSyncService::KeepLocalFn m_keepLocal;
	std::function<void(const std::filesystem::path&)> m_openFolder;
	HE::Sc::TeamSyncService    m_sync;

	Project       m_project;           // what the service runs for
	std::string   m_key;               // project + sign-in it was started with
	Blocker       m_blocker = Blocker::NotTeamMode;
	std::uint64_t m_saveAtMs = 0;      // a save arms this; 0 = none pending
	std::uint64_t m_nowMs = 0;
	bool          m_wasClient = false; // a session client last frame
	bool          m_wasHost   = false;
	bool          m_wasSignedIn = false;
};
