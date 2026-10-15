#pragma once
// ─── Saving through the team server (topic 199) ──────────────────────────────
// With Config/ProjectSettings.json → sourceControl.mode = "teamServer" the
// project's history lives on the team server, which merges every member's
// saves (docs/team-server-source-control-design-2026-10-13.md, sections 6 and
// 12). This is the editor's half: it keeps the per-file base (the content the
// editor last received from or had accepted by the server) and, on every sync,
//
//   scan      the project tree against the bases (a size/mtime cache keeps
//             unchanged files from being hashed again)
//   upload    HAVE → PUT what the server lacks → one COMMIT with every change
//   reconcile MANIFEST of head: a file the server's merge changed, or that
//             somebody else changed, is written here when the local copy still
//             equals its base; a locally changed one waits for the next upload,
//             where the server merges it against the right base
//
// The same reconcile runs when somebody else's save moves the head (sc-head
// on the team connection: notifyHead).
//
// Threading, as GitService: one owned worker thread does the network and the
// hashing; the main thread posts commands and calls pump(). The one thing the
// worker never does is write into the project: a file coming from the server
// is handed to pump(), which writes it on the main thread (between two editor
// saves, never during Play when the caller says so) after checking that the
// local copy is still the one the decision was made on. So a save and a pull
// can never race on the same file.
//
// No HE_Net types in here: the sync connection (HE::Net::ScClient) and the
// ticket it needs are behind the TicketFn seam, which the editor fills from
// its TeamController and tests fill from a fake team server.
#include "SourceControl/ScCommon.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace HE::Sc {

class HE_SC_API TeamSyncService {
public:
	struct Settings
	{
		std::filesystem::path projectRoot;
		std::string           host;          // the team server (TeamController's address)
		std::uint16_t         port = 0;
		std::string           team, member, project;
		// Saved/TeamSync/<team>-<project>.state under the project by default.
		std::filesystem::path stateFile;
	};

	enum class Phase : std::uint8_t
	{
		Off,            // not started
		WaitingTicket,  // needs a sync ticket from the team connection (sign-in)
		Connecting,
		Idle,           // connected (or ready to connect), nothing to do
		Working,        // scanning, uploading, downloading
		Waiting,        // paused: a session client, or a first sync waiting for a decision
		Offline,        // the last attempt failed; retried on the next trigger
	};

	// What the server did with one file of OUR upload (COMMIT outcome line).
	struct FileResult
	{
		std::string outcome;   // applied, same, merged, merged-arrival, replaced, delete-dropped, restored
		std::string path;
		std::string detail;    // "against r4 by Alex (1 fields) lost=horizon-conflicts:r5/…"
	};
	// One revision from the server's log.
	struct Revision
	{
		std::uint64_t            rev = 0;
		std::string              member;
		std::int64_t             time = 0;      // unix seconds
		std::string              subject;
		std::vector<std::string> merges;        // "Horizon-Merge:" values, one per file
	};
	// A later save by somebody else won over a version of ours: the server
	// kept ours on its horizon-conflicts branch.
	struct Lost
	{
		std::uint64_t rev = 0;        // the revision that overruled us
		std::uint64_t ourRev = 0;     // the revision of ours that lost ("against rN")
		std::string   path;
		std::string   winner;         // who saved later
		std::string   outcome;        // merged-arrival (fields of ours lost) or replaced (the whole file)
		std::string   detail;
	};

	struct Status
	{
		Phase         phase = Phase::Off;
		std::uint64_t rev = 0;               // the head this editor is in sync with
		std::size_t   waiting = 0;           // local changes not uploaded yet (last scan)
		bool          paused = false;        // a session client: nothing goes up or comes down
		bool          projectMissing = false;// the server has no such project (admins may create it)
		bool          isAdmin = false;       // as the ticket source said
		bool          connected = false;
		std::string   error;                 // the last failure, for the UI; empty when fine
		std::string   activity;              // "Uploading 3 files…", empty when idle
		// A first sync (no state file yet) found local files that differ from
		// the team's: nothing is uploaded or overwritten until resolveFirstSync.
		std::vector<std::string> firstSyncDiffers;
		std::uint64_t lastSyncUnix = 0;      // last finished upload or reconcile
	};

	// Files that came from the server and wait for the main thread.
	struct Incoming
	{
		std::string               path;      // project-relative, '/'-separated
		bool                      remove = false;
		std::vector<std::uint8_t> bytes;
		std::string               sha;       // of bytes (empty for remove)
		std::string               expect;    // the local copy's hash the decision was made on ("" = absent)
	};

	// Called on the MAIN thread (from pump) when the worker needs a ticket.
	// Ask the team server and call `deliver(ticket, error, isAdmin)` once,
	// from any later main-thread moment; ticket empty = refused (error says
	// why). Return false when it cannot even be asked (not signed in): the
	// service then waits and asks again at the next trigger.
	using Deliver  = std::function<void(const std::string& ticket, const std::string& error,
	                                    bool projectMissing, bool isAdmin)>;
	using TicketFn = std::function<bool(const std::string& project, Deliver deliver)>;

	TeamSyncService();
	~TeamSyncService();
	TeamSyncService(const TeamSyncService&)            = delete;
	TeamSyncService& operator=(const TeamSyncService&) = delete;

	// Starts (or re-targets) the worker and runs a first reconcile. A project
	// without a state file is a first sync (see Status::firstSyncDiffers).
	void start(const Settings& settings, TicketFn tickets);
	void stop();
	bool running() const { return m_running; }
	const Settings& settings() const { return m_settings; }

	// ── Triggers (main thread; collapse while one is pending) ──
	// Upload whatever differs from the bases. `message` is the revision's
	// message (empty: the server's "Save by <member>").
	void requestSync(const std::string& message = {});
	// Somebody's save moved the head to `rev` (sc-head event). Ours too: the
	// server sends it to the saver as well; a rev we already have is ignored.
	void notifyHead(std::uint64_t rev);
	// Check the head without a save: the team connection came back, and an
	// sc-head may have gone by while it was down.
	void refresh();
	// A session client neither uploads nor pulls (its saves go to the host).
	// resume with adoptTeam = true when the session ended: every file that
	// differs from its base takes the team's version (the host uploaded the
	// session's state), so a stale copy never beats a newer save.
	void setPaused(bool paused, bool adoptTeam = false);
	// While set (the host of a session), uploads go out but nothing coming
	// from the server is written: a file changing under the session would fork
	// it from the peers. Cleared, the next reconcile catches up.
	void setHoldIncoming(bool hold);
	// First sync: keepMine uploads the differing files (the server merges them
	// with the team's as two additions), otherwise the team's versions replace them.
	void resolveFirstSync(bool keepMine);
	// Admins: create the project on the server, then upload the tree as its
	// first revision.
	void createProject();
	// Fetch our overruled version of `lost` into
	// Saved/TeamSync/Lost/r<rev>/<path>; the path arrives through takeOpened().
	void fetchLost(const Lost& lost);

	// ── Main thread ──
	// Delivers events, runs the ticket source when asked and writes incoming
	// files when `mayWrite` (false during Play: they wait). Returns the
	// project-relative paths written or removed this call, for reloads.
	std::vector<std::string> pump(bool mayWrite = true);

	Status status() const;
	// Our last upload's per-file results (newest sync only).
	std::vector<FileResult> lastResults() const;
	// The server's log, newest first (at most kHistory revisions).
	std::vector<Revision> history() const;
	// Where later saves by others won over ours, newest first.
	std::vector<Lost> lost() const;
	// Sentences for a toast (somebody else's save won over ours, a refusal).
	bool pollNotice(std::string& text, bool& error);
	// Paths fetchLost() wrote, absolute.
	bool takeOpened(std::filesystem::path& out);

	static constexpr std::size_t kHistory = 100;

	// ── Pieces with no thread, public for tests ──
	// .gitignore-style rules: "dir/" (any level), "a/b/" (anchored), "*.log",
	// "name". No negation. Always ignored: .git/, Saved/ (state lives there).
	struct IgnoreRules
	{
		struct Rule { std::string glob; bool dirOnly = false; bool anchored = false; };
		std::vector<Rule> rules;
		void addText(const std::string& gitignore);
		bool ignored(const std::string& relPath, bool isDir) const;
		static bool glob(const char* pattern, const char* text);
	};
	// A "Horizon-Merge:" trailer value: "<outcome> <path>[ <detail>]".
	static bool parseMerge(const std::string& value, std::string& outcome, std::string& path,
	                       std::string& detail);
	// "against r4 by Alex …" / "changed in r4 by Alex": rev and member, or false.
	static bool parseAgainst(const std::string& detail, std::uint64_t& rev, std::string& member);
	// The Lost entries a revision implies for `self` (case-insensitive).
	static std::vector<Lost> lostFor(const Revision& revision, const std::string& self);

private:
	struct Impl;

	Settings                m_settings;
	TicketFn                m_tickets;
	bool                    m_running = false;
	// Shared: a ticket that arrives after stop() (the team server answering
	// late) finds its Impl gone through a weak_ptr instead of a dangling this.
	std::shared_ptr<Impl>   m_impl;
	std::thread             m_thread;
};

} // namespace HE::Sc
