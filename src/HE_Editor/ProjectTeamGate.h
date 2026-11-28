#pragma once
#include "TeamController.h"
#include "TeamSyncController.h"

#include <SourceControl/TeamSyncService.h>

#include <cstdint>
#include <string>

// ── Opening a project whose history the team server keeps ────────────────────
// When Config/ProjectSettings.json says sourceControl.mode = "teamServer", the project
// preflight (ProjectPreflight.h) asks the team server for the newest version before the
// project loads, as it asks the EngineContent server for what the project references:
// the team sync runs on the project's folder while nothing of it is open yet, so what
// arrives is simply on disk when the load reads it.
//
// This file is the decision and the two dialogs, ImGui plus EditorWidgets and no
// AppContext, so the UI tests draw them (tests/test_team_sync_ui.cpp).
namespace ProjectTeamGate
{
	enum class State : std::uint8_t
	{
		Waiting,   // signing in, connecting, downloading
		AskUser,   // the sync needs a person: a first sync that differs, or an admin may create the project
		Ready,     // up to date with the team server: open
		Failed,    // could not be updated: ask whether to open anyway
	};

	struct Input
	{
		TeamController::Status          team = TeamController::Status::SignedOut;
		std::string                     teamError;      // TeamController::lastError()
		std::string                     teamName;       // the project's team
		std::string                     projectName;    // the project on the server
		TeamSyncController::Blocker     blocker = TeamSyncController::Blocker::None;
		bool                            syncRunning = false;
		HE::Sc::TeamSyncService::Status sync;
		std::uint64_t                   elapsedMs = 0;  // since the gate started
		std::int64_t                    startUnix = 0;  // wall clock (s) when it started
	};

	struct Result
	{
		State       state = State::Waiting;
		std::string stage;         // Waiting: what is happening, one line
		std::string headline;      // Failed: the first line of the dialog
		std::string reason;        // Failed: why, in the server's or the client's words
		bool        unreachable = false;   // Failed: the server did not answer (vs. refused / not signed in)
	};

	// No answer while the link is not up for this long counts as unreachable. A download
	// that is under way is never cut short.
	constexpr std::uint64_t kConnectTimeoutMs = 20'000;

	Result evaluate(const Input& in);

	enum class Answer : std::uint8_t { None, TryAgain, OpenAnyway, CloseProject };

	// The body of the progress dialog while the gate waits (state line, activity), or the
	// team sync's own question when it needs a person. Call inside an open popup.
	// Returns OpenAnyway for "Open Without Updating", CloseProject for "Cancel".
	Answer drawWaiting(const std::string& projectName, const Result& r, TeamSyncController* sync);
	// The question after a failure. Call inside an open popup.
	Answer drawFailed(const std::string& projectName, const Result& r);
}
