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
class TeamSyncController;

namespace TeamSyncView
{
	void draw(TeamSyncController& sync);
}
