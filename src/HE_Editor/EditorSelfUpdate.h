#pragma once

// ── Help ▸ Check for Updates…: the part of it that is not a window ────────────
// The self-update module (src/HE_SelfUpdate, docs/editor-selfupdate-sftp-design.md)
// knows how to check, download and hand over to HorizonUpdater. What it does not
// know is the editor: that a check must not block a frame, that the open project
// has to come back after the restart, and that the one-way step may only run
// once the unsaved-changes prompt has had its say. This file is that glue, kept
// free of ImGui and of AppContext so he_tests can drive it on every platform.
//
// The order the editor goes through, and who decides what:
//
//   startCheck()     worker thread: manifest over the update account.
//   startDownload()  worker thread: this platform's build into staging, verified,
//                    then preflightTakeover() — so "Restart to Update" is only
//                    offered when the takeover can actually be started.
//   (the guard)      EditorUI's requestGuarded(GuardedAction::InstallUpdate): the
//                    same Save / Don't Save / Cancel prompt as Quit, because the
//                    update quits the editor. Nothing in here asks about unsaved
//                    work, and nothing in here quits.
//   install()        worker thread: beginTakeover() with the open project as the
//                    relaunch argument. It re-hashes and unpacks the staged build
//                    (seconds to minutes), so it must not run on the frame. Only
//                    once the phase is Installed does the caller quit; on failure
//                    the phase goes back to Ready and the editor keeps running
//                    exactly as it was.
//
// The open project survives through the relaunch arguments: the new build gets
// the .heproj path as its first argument, which EditorApplication's launch pick
// (ProjectLaunchOpen::pickFromArguments) opens through ProjectPreflight like a
// double-clicked project file.

#include <SelfUpdate/SelfUpdateCheck.h>
#include <SelfUpdate/Takeover.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace EditorSelfUpdate
{
	// What the relaunched build is started with: the open project, or nothing on
	// the Project Hub (it then opens the last project as on any other start).
	std::vector<std::string> relaunchArgsFor(bool projectLoaded, const std::string& projectPath);

	enum class Phase
	{
		Idle,            // nothing asked yet
		Checking,
		NotConfigured,   // no usable update account (compiled in or HE_SELFUPDATE_*); `message` says why
		UpToDate,
		RemoteOlder,     // the server's build is older; never offered
		NoBuild,         // newer, but nothing for this platform
		Available,       // newer and downloadable
		Downloading,
		Ready,           // staged + verified; `installBlocker` empty = Restart is possible
		Installing,      // beginTakeover running on the worker
		Installed,       // HorizonUpdater started and waiting for this process: quit now
		Failed,         // check or download failed; `message` says why
	};

	const char* phaseName(Phase p);

	struct Snapshot
	{
		Phase         phase = Phase::Idle;
		std::string   localVersion;
		std::string   remoteVersion;
		std::string   notes;            // the manifest's release notes
		std::string   message;          // error / not-configured text; never the password
		std::string   installBlocker;   // Ready but preflightTakeover() said no
		std::uint64_t done  = 0;        // download progress
		std::uint64_t total = 0;
		bool busy() const { return phase == Phase::Checking || phase == Phase::Downloading ||
		                           phase == Phase::Installing; }
		bool canInstall() const { return phase == Phase::Ready && installBlocker.empty(); }
	};

	// The four calls into HorizonSelfUpdate, swappable so a test can run the whole
	// sequence against a directory instead of an SFTP account and watch what the
	// takeover would have been started with, without starting it.
	struct Backend
	{
		std::function<HE::Su::CheckResult()> check;
		std::function<HE::Su::DownloadResult(const HE::Su::CheckResult&, const HE::Su::SuProgressFn&)> download;
		std::function<HE::Su::TakeoverCheck(const HE::Su::TakeoverOptions&)>  preflight;
		std::function<HE::Su::TakeoverResult(const HE::Su::TakeoverOptions&)> begin;
		std::filesystem::path stagingRoot;
	};

	// The real thing: the update account (loadSelfUpdateConfig: HE_SELFUPDATE_* or
	// the compiled-in one, read when a check starts, so a changed environment
	// needs no restart), SFTP, defaultStagingDir(), and the real
	// preflightTakeover/beginTakeover.
	Backend productBackend();

	class Session
	{
	public:
		explicit Session(Backend backend);
		~Session();   // joins the worker
		Session(const Session&) = delete;
		Session& operator=(const Session&) = delete;

		// Both return false (and do nothing) while the worker is busy.
		bool startCheck();
		bool startDownload();   // needs Phase::Available

		Snapshot snapshot() const;
		void     join();

		// The one-way step. Call it ONLY from the editor's guarded action, after
		// the unsaved-changes prompt. Returns false (and starts nothing) unless a
		// verified build is Ready and nothing else runs; otherwise the takeover
		// runs on the worker with the phase at Installing. Phase Installed: the
		// caller must quit right away (HorizonUpdater is already waiting for this
		// process to exit). On failure the phase goes back to Ready with the error
		// as `installBlocker`, the staged build stays where it is, and the caller
		// must NOT quit.
		bool install(const std::vector<std::string>& relaunchArgs);

	private:
		void runWorker(std::function<void()> body);
		void set(const std::function<void(Snapshot&)>& edit);

		Backend                 m_backend;
		mutable std::mutex      m_mutex;
		Snapshot                m_state;
		HE::Su::CheckResult     m_check;   // the last check; what startDownload fetches
		std::thread             m_worker;
		std::atomic<bool>       m_running{ false };
	};
}
