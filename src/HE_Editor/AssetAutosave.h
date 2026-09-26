#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ── Periodic autosave of dirty ASSET TABS onto a recovery path ───────────────
// SceneAutosave's sibling, for everything that is not the world: a script, a
// C++ class, a material graph, a widget, a HorizonCode class, an input asset,
// a type, a theme, a bone mask, a blend space, a sequence, a particle graph,
// an animator state machine, an animation clip's notifies. Each of those tabs
// holds its edits in panel state until Save, and before this a crash lost
// every one of them.
//
// The same rule as the scene: the user's file is the user's. What is written
// on the timer is a COPY in <project>/Saved/Autosave/Assets/, one per dirty
// file, next to a manifest that says which file it belongs to and how that
// file looked when the copy was taken. Nothing here ever marks a tab clean.
//
// One mechanism for fourteen panels, and the panels see almost none of it:
//  * a panel only has to answer "these files of mine are dirty, and this is
//    how to write the bytes a Save would write to another path" — an
//    AssetSnapshotSource each. EditorUI::appendAssetSnapshots asks them all,
//    the same ask-everyone dispatch saveAsset and tabHasUnsavedEdits use;
//  * "a Save removes the copy" is not wired into fourteen save paths. Every
//    tick compares the dirty list with the copies this run has written and
//    deletes the copy of every file that left it — saved, reverted, discarded
//    or deleted, it does not matter which, it is not unsaved any more.
//
// Two folders, the same split SceneAutosave makes with two file names:
//  * Live/    what this run keeps writing;
//  * Pending/ what an earlier run left behind. promoteStale() moves the one
//    into the other at project load, before the first timer of this run can
//    overwrite the only copy of what the crash lost.
// Restore copies the file it replaces into Replaced/ first, so answering the
// recovery dialog can never cost the version that was on disk.
//
// ImGui-free and handed the clock, like SceneAutosave, so the rules run on a
// fake clock in the tests.
namespace HE::Ed
{
	// One dirty file and how to write its unsaved state somewhere else.
	struct AssetSnapshotSource
	{
		// Absolute path of the file the edits belong to — the one a Save would
		// write. A panel that edits two files (a C++ class: header + source)
		// hands one source per dirty file.
		std::string file;
		// Writes the bytes a Save would write, to `destPath`. Must not touch the
		// panel's dirty flag or the asset's own file.
		std::function<bool(const std::string& destPath)> write;
	};

	// A copy an earlier session left behind: what the recovery dialog offers.
	struct AssetRecoveryEntry
	{
		std::string   key;            // handle for restorePending/discardPending
		std::string   relativePath;   // project-relative, '/'-separated
		std::string   targetPath;     // absolute; empty if the manifest points outside the project
		std::string   snapshotPath;   // the copy itself
		std::int64_t  savedAtUnix   = 0;
		bool          targetMissing = false;  // the file is gone from disk
		// The file on disk was written after the copy was taken — by a save the
		// crash cut short, a pull, another tool. The copy is then older than the
		// file, and restoring it would roll that change back.
		bool          changedSince  = false;
	};

	class AssetAutosave
	{
	public:
		// <project dir>/Saved/Autosave/Assets — beside the scene's snapshot,
		// outside Content/, so the Content Browser never indexes it.
		static std::string recoveryDirForProject(const std::string& projectFilePath);
		// The folder a .hproject lives in (or the folder itself, if handed one).
		static std::string projectRootFor(const std::string& projectFilePath);

		// Points the autosave at a project: where copies go, and the root the
		// manifests' paths are relative to. Resets the timer and forgets which
		// copies this run wrote; touches no file. Empty disables it.
		void configure(const std::string& recoveryDir, const std::string& projectRoot);
		const std::string& dir() const  { return m_dir; }
		std::string liveDir() const;
		std::string pendingDir() const;
		std::string replacedDir() const;

		void setEnabled(bool on)          { m_enabled = on; }
		bool enabled() const              { return m_enabled; }
		// Floor of 10 s, the same as the scene's.
		void setIntervalMs(std::uint64_t ms);
		std::uint64_t intervalMs() const  { return m_intervalMs; }

		// One call per frame with every file that is dirty right now.
		//  * Every call: the copy of a file that is no longer in `dirty` goes.
		//  * Every intervalMs (timed from the first call after configure), if
		//    enabled and anything is dirty: each source is written to a temp
		//    name, renamed over its copy, and its manifest goes last.
		// A source whose file is outside the project root is skipped: its copy
		// could not be put back without writing outside the project.
		// Returns how many copies were written.
		std::size_t update(std::uint64_t nowMs, const std::vector<AssetSnapshotSource>& dirty);

		// Removes every copy this run holds (Live/). For a clean exit and a
		// project switch the user answered.
		void clear();
		std::size_t liveCount() const     { return m_live.size(); }

		// Moves what an earlier run left in Live/ to Pending/ (replacing an older
		// pending copy of the same file) and answers how many are pending now.
		// Call at project load, before the first update().
		std::size_t promoteStale();

		// Every pending copy with a complete manifest, sorted by path.
		std::vector<AssetRecoveryEntry> pending() const;
		// The user said no to one copy.
		void discardPending(const std::string& key);
		// The user said yes: the file on disk (if any) is copied into
		// Replaced/, then the copy is written over it (temp + rename) and the
		// pending pair is dropped. Returns the file written, or nothing with
		// `error` set — nothing is overwritten unless the backup succeeded.
		std::optional<std::string> restorePending(const std::string& key, std::string* error = nullptr);

		// The key a file's copy is stored under, or "" if it is outside the root.
		std::string keyFor(const std::string& absoluteFile);

	private:
		std::string relativeOf(const std::string& absoluteFile) const;
		bool        writeOne(const AssetSnapshotSource& src, const std::string& rel, const std::string& key);
		void        removeLive(const std::string& key);

		std::string   m_dir;
		std::string   m_root;
		bool          m_enabled       = true;
		std::uint64_t m_intervalMs    = 60'000;
		std::uint64_t m_lastAttemptMs = 0;
		bool          m_armed         = false;
		std::unordered_set<std::string> m_live;                       // keys written this run
		std::unordered_map<std::string, std::string> m_keyCache;      // absolute file → key ("" = outside)
	};
}
