#pragma once

// ─── Team UI: Preferences ▸ Team and the Team window (topic 197) ─────────────
// The drawing half of the team subsystem; TeamController is the state half.
// Both surfaces draw from here so they cannot disagree:
//
//   Preferences ▸ Editor ▸ Team   the team server's address and the sign-in
//                                  (team, member, password; Sign In / Create Team)
//   Window ▸ Team                  who is in the team and online, the sessions
//                                  the team announced, Host / Invite / Join /
//                                  Decline, and (for admins) adding and removing
//                                  members
//
// Takes a TeamController and the three config strings, never AppContext — which
// is what lets tests/test_team_ui.cpp draw the real thing headless against a
// fake team server. Gated on __has_include(<imgui.h>), not HE_IMGUI_ENABLED:
// the tests compile this file without that define.
//
// The password lives in Form, in memory, and only until it has been handed to
// the controller: neither the config nor this struct keeps it after a press.

#include "TeamController.h"

#include <cstddef>
#include <string>
#include <vector>

namespace TeamView
{
	// What is persisted in the editor config. The server address is the same
	// field the relay used to be (EditorConfig::CollabRelayAddress): the team
	// server IS the relay, so one address serves team sessions and the ones
	// hosted from the Collaboration window alike.
	struct Settings
	{
		std::string* server = nullptr;
		std::string* team   = nullptr;
		std::string* member = nullptr;
	};

	// The typing in progress, kept by the caller between frames.
	struct Form
	{
		std::string password;            // wiped once handed over
		bool        hostOpen = false;    // "anyone in the team may join" for the next Host

		// Admin: Add Member
		std::string newName;
		std::string newPassword;
		bool        newAdmin = false;
		// The member whose Remove was pressed once and now shows Confirm Remove.
		std::string removeArmed;

		// Your own password
		std::string oldPassword;
		std::string changedPassword;

		// The controller's notices, newest last, so the window can say what
		// happened to a press (a refusal arrives a few frames later).
		std::vector<TeamController::Notice> recent;
		static constexpr std::size_t kRecent = 6;

		void wipeSecrets();
	};

	// The editor's one Form, shared by the Preferences page and the window so a
	// notice drained by one is still listed by the other.
	Form& editorForm();

	// "Signed in", "Connecting…", … — the status line both surfaces show.
	const char* statusText(TeamController::Status s);

	// Moves the controller's notices into form.recent (bounded). Called by
	// drawPanel(); the Preferences page calls it too so a refused sign-in is
	// explained where it was pressed.
	void collectNotices(TeamController& team, Form& form);

	// Preferences ▸ Team: the server field (with the parse check under it) and
	// the sign-in block. Pushes the "Team" help scope itself.
	void drawSettings(TeamController& team, const Settings& settings, Form& form);

	// Window ▸ Team, the body (the caller owns Begin/End). Signed out it shows
	// the same sign-in block, so a team can be created from here.
	void drawPanel(TeamController& team, const Settings& settings, Form& form);

	// ── Test seam ──
	// Where each control was drawn in the last frame, by label and the row it
	// belongs to (a member's or a host's name), so a headless test can press
	// "Invite" next to Bob without re-deriving ImGui's id stack. Null (the
	// default, and always in the editor) records nothing.
	struct ProbedItem
	{
		std::string label;       // as passed to the widget, "##…" included
		std::string row;         // member / session host; empty outside the lists
		float       x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		bool        disabled = false;
	};
	void setProbe(std::vector<ProbedItem>* out);
}
