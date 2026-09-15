#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

// ── Periodic scene autosave onto a RECOVERY path ─────────────────────────────
// Until now the only timed save in the editor lived inside the collaboration
// sync (EditorApplication::updateAssetCollabSync), and that whole function is
// gated on m_collab.inSession(). Somebody working alone had exactly one save:
// the one they pressed. A crash took everything since.
//
// This is the solo path, and it is deliberately NOT a save of the scene file:
//  * The user's file is the user's. Writing it on a timer would turn "I tried
//    something and closed without saving" into "it is saved now" — and it
//    would move m_savedRevision, so the "*" in the title bar and the quit
//    prompt would both stop telling the truth.
//  * Instead the world is serialised into the project's Saved/Autosave folder,
//    next to a manifest that says which scene it came from, when, and at which
//    undo revision. A clean exit or a real save deletes both; a crash leaves
//    them behind, which is how the next start knows there is something to
//    offer (the recovery dialog is the next step on the board, not this one).
//
// The class is ImGui-free and knows nothing about the editor: it is handed the
// clock, the dirty flag, the undo revision and a writer callback, and it
// answers "write now or not". That is what lets the timing rules be tested
// with a fake clock instead of a stopwatch.
//
// Two snapshots can exist side by side, and the distinction matters:
//  * the LIVE one (autosave.hescene) is what this run keeps writing;
//  * the PENDING one (recovery.hescene) is what a previous run left behind.
// promoteStale() moves the first into the second at project load, BEFORE this
// run's first timer fires — otherwise the first autosave of the new session
// would overwrite the only copy of what the crash lost.
namespace HE::Ed
{
	// The manifest beside a snapshot: everything a recovery offer has to say.
	struct RecoveryInfo
	{
		std::string   scenePath;     // the .hescene it was edited from; empty = a new, never-saved scene
		std::string   projectPath;   // the .hproject it belongs to
		std::uint64_t revision    = 0; // undo revision at the time of the snapshot
		std::int64_t  savedAtUnix = 0; // seconds since the epoch
		std::string   snapshotPath;  // the .hescene the manifest describes (filled in by read())
	};

	class SceneAutosave
	{
	public:
		// <project dir>/Saved/Autosave — beside the thumbnail cache, outside
		// Content/, so the Content Browser never indexes it.
		static std::string recoveryDirForProject(const std::string& projectFilePath);

		// The four file names, exposed so a test and the future dialog agree
		// with the writer on where things are.
		static std::string livePath(const std::string& dir);
		static std::string liveManifestPath(const std::string& dir);
		static std::string pendingPath(const std::string& dir);
		static std::string pendingManifestPath(const std::string& dir);

		// Points the autosave at a project. Resets the timer; does not touch
		// any file. An empty dir disables it until the next configure.
		void configure(const std::string& recoveryDir);
		const std::string& dir() const { return m_dir; }

		void setEnabled(bool on)                   { m_enabled = on; }
		bool enabled() const                       { return m_enabled; }
		// Floor of 10 s: below that the serialisation itself becomes the hitch
		// the setting was meant to prevent.
		void setIntervalMs(std::uint64_t ms);
		std::uint64_t intervalMs() const           { return m_intervalMs; }

		// Writes the world to `path` (a temporary name chosen here) and answers
		// whether it succeeded. SceneSerializer::save, from the editor's side.
		using Writer = std::function<bool(const std::string& path)>;

		// One call per frame. Writes a snapshot when ALL of these hold:
		//   * enabled, configured, and `dirty`;
		//   * `revision` differs from the one of the last snapshot (an unchanged
		//     scene is not re-serialised on every tick);
		//   * at least intervalMs have passed since the last write attempt (or
		//     since configure()).
		// The write is temp-file + rename, the manifest goes second, so a crash
		// mid-write leaves the previous snapshot whole and a manifest never
		// points at a half file. Returns true when a snapshot was written.
		bool update(std::uint64_t nowMs, bool dirty, std::uint64_t revision,
		            const RecoveryInfo& info, const Writer& writer);

		// Removes the LIVE snapshot and its manifest. For a real save, a scene
		// switch and a clean exit — every moment where "what is on disk" and
		// "what the user meant" agree again. Forgets the last snapshot revision
		// too, so the next edit is snapshotted after one interval.
		void clear();

		// Moves a live snapshot a previous run left behind to the pending name,
		// replacing an older pending one. Returns what it found (or what was
		// already pending) so the caller can say so. Call at project load,
		// before the first update().
		std::optional<RecoveryInfo> promoteStale();

		// The pending snapshot, if any: what a recovery dialog would offer.
		std::optional<RecoveryInfo> pending() const;
		// Drops the pending snapshot — the user restored it or said no.
		void discardPending();

		// The manifest at `manifestPath`, or nothing if it is missing or unreadable.
		static std::optional<RecoveryInfo> readManifest(const std::string& manifestPath);

		std::uint64_t lastSnapshotRevision() const { return m_lastRevision; }
		bool          hasLiveSnapshot() const;

	private:
		std::string   m_dir;
		bool          m_enabled      = true;
		std::uint64_t m_intervalMs   = 60'000;
		std::uint64_t m_lastAttemptMs = 0;
		bool          m_armed        = false;   // false until the first update() after configure()
		std::uint64_t m_lastRevision = 0;
		bool          m_haveRevision = false;
	};
}
