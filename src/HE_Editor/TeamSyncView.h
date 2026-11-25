#pragma once
// ─── The team-sync view of the Source Control window (topic 199) ────────────
// Drawn instead of the git view when the project saves through the team
// server: where the project stands against the server, what the last save did
// per file (taken, merged, which side won), the versions of yours that a
// later save overruled (with the way back), and the server's history with its
// merge trailers. The server never leaves a conflict for the user to resolve;
// this is where the user sees what it decided.
//
// Its own file, like TeamView, so the UI tests draw the real thing over a
// TeamSyncController (tests/test_team_sync_ui.cpp). ImGui plus EditorWidgets;
// call inside an open window.
#include <SourceControl/TeamSyncService.h>
#include <SourceControl/TextMerge.h>

#include <functional>
#include <string>
#include <vector>

class TeamController;
class TeamSyncController;

namespace TeamSyncView
{
	void draw(TeamSyncController& sync);

	// ── Switching who keeps the history ──
	// Both write Config/ProjectSettings.json through `apply` (the panel: the project's
	// settings, then saveProjectSettings). The file is synced, so the whole team flips
	// together. `locked`: not now (a collaboration session decides for this editor).
	//
	// The way in, drawn above the git view while signed in to a team (nothing otherwise):
	// a line, and "Use the Team Server..." with the project's name on the server. True on
	// the frame it switched.
	using ApplyTeam = std::function<bool(const std::string& team, const std::string& project)>;
	bool drawSwitchToTeam(const TeamController* team, const std::string& projectName, bool locked,
	                      const ApplyTeam& apply);
	// The way out, under the team view: back to the local git repository, which picks up
	// where it froze at the switch. True on the frame it switched.
	bool drawSwitchToGit(bool locked, const std::function<bool()>& apply);

	// The project's name as the server takes it: letters, digits, '-' and '_', at most 64.
	std::string serverProjectName(const std::string& projectName);
	bool        validServerName(const std::string& name);

	// ── Conflicts (TeamSyncService::lost) ──
	// Opens the conflict window on `lost` (the Resolve button, the notification's Resolve).
	void openConflict(const HE::Sc::TeamSyncService::Lost& lost);
	// Asked for once since the last call: the editor shows the Source Control window.
	bool takeWindowRequest();
	// The window itself: both versions, the changed blocks with a pick each (or the
	// whole file for graphs and binaries), "Use This Result" / "Keep Theirs". Call
	// every frame, after draw(), outside any text wrap.
	void drawConflictWindow(TeamSyncController& sync);
	bool conflictOpen();
	// For the tests: the picks, the merged text, and the JSON check that guards "Use This Result".
	void        setConflictPicks(const std::vector<HE::Sc::Pick>& picks);
	std::string conflictResult();
	bool        resultIsValid(const std::string& mine, const std::string& theirs, const std::string& result);

	// Where a named control was drawn last ({0,0,0,0} when not), for the UI tests.
	struct Rect { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
	Rect controlRect(const std::string& name);
}
