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
//   both, signed in                your profile: the colour and picture the team
//                                  sees next to your name (topic 202)
//   the footer, right of the bell  your own status: your face, or a red X that
//                                  opens a sign-in modal (topic 202)
//   the footer, left of the        who else in the team is online and not in
//   session's faces                the session, and the slide across to the
//                                  session's faces when one joins (topic 202)
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
#include <functional>
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

		void wipeSecrets();
	};

	// The editor's one Form, shared by the Preferences page and the window so a
	// notice drained by one is still listed by the other.
	Form& editorForm();

	// "Signed in", "Connecting…", … — the status line both surfaces show.
	const char* statusText(TeamController::Status s);

	// Empties the controller's notice queue. What it holds is also posted to the
	// notification channel (TeamController::notice), which is where the user reads
	// it — a refusal, an invitation, someone joining — so the window no longer lists
	// it a second time. Called by drawPanel() and the Preferences page so the queue
	// does not fill up behind a window nobody opens.
	void collectNotices(TeamController& team, Form& form);

	// Preferences ▸ Team: the server field (with the parse check under it) and
	// the sign-in block. Pushes the "Team" help scope itself.
	void drawSettings(TeamController& team, const Settings& settings, Form& form);

	// Window ▸ Team, the body (the caller owns Begin/End). Signed out it shows
	// the same sign-in block, so a team can be created from here.
	void drawPanel(TeamController& team, const Settings& settings, Form& form);

	// ── The footer: your own status, right of the bell (topic 202) ──
	// Signed in it is your face (picture, or colour and initial); otherwise a
	// red X (amber while a sign-in is under way) that opens the Team Sign-In
	// modal: name and password, Sign In tries again. The modal is drawn by
	// drawFooterStatus itself, in whatever window the footer is, and closes
	// once the sign-in is through.
	//
	// One text line tall, like the bell (NotificationBar.cpp has why): the
	// footer right-aligns against footerStatusWidth(), so the two must agree.
	float footerStatusWidth();
	// True when your face was clicked: the caller opens Window ▸ Team.
	bool drawFooterStatus(TeamController& team, const Settings& settings, Form& form);

	// ── The footer: who else in the team is online (topic 202) ──
	// The members TeamController::onlineOutsideSession() names, left of the
	// session's own cluster (CollabPresenceBar). Set apart from it on purpose:
	// team faces are rounded squares with a green "online" dot, the session's
	// are rings in the participant colour, and a thin rule stands between the
	// two while both are there. Joining the session moves a member from this
	// cluster to that one.
	//
	// The faces overlap in a stack, to keep the footer short; resting the mouse
	// on it opens a menu that lists every one of them on its own row with an
	// Invite (while hosting a team session). Not Remove: taking someone out of
	// the team is the Team window's, behind its confirmation, never a footer's.
	//
	// Split in two like CollabPresenceBar, for the same reason: the cluster is
	// drawn inside the footer window, the menu over everything else from the
	// editor's overlay pass.
	//
	// footerOnlineWidth is 0 when there is nobody to show (not signed in, or
	// everyone online is in the session) and the footer leaves the slot out.
	float footerOnlineWidth(const TeamController& team);
	// `sessionBeside`: the session cluster is drawn right of this one, so the
	// rule between them is drawn. A click pins the menu open.
	void drawFooterOnline(TeamController& team, bool sessionBeside);
	// The menu. Call from the overlay pass, after every panel; draws nothing
	// unless drawFooterOnline ran this frame. True when its Open Team Window
	// was pressed: the caller opens Window ▸ Team.
	bool drawFooterOnlineMenu(TeamController& team);

	// ── The footer: joining the session (topic 202) ──
	// A member who leaves the stack because they joined the session does not
	// just vanish from one cluster and pop up in the other: their face slides
	// from where it stood in the stack (or from the "+N", if it was not shown)
	// to their place among the session's faces, turning from the team's square
	// into the session's round face on the way, and the session cluster leaves
	// that face out until the slide has landed. Keyed by name, like the rule
	// that moved them. Joining only: someone who leaves the session reappears
	// in the stack at once.
	//
	// Where the session's faces are is CollabPresenceBar's to know; the editor
	// hands them over, which keeps this file free of AppContext (see the top).
	struct SessionFace
	{
		std::string name;    // the participant's, matched to a member in any case
		float       x = 0, y = 0, size = 0;
	};
	inline constexpr float kJoinSlideSeconds = 0.4f;
	// True while `name`'s face is still on its way. The session cluster asks
	// before it draws a face, skips it while true, and still reports where it
	// would be: that is where the slide lands.
	bool footerJoining(const TeamController& team, const std::string& name);
	// The sliding faces. Call inside the footer window after both clusters,
	// every frame, also when the stack itself is empty: the last one online
	// may have just joined. A slide whose member has no face in
	// `sessionFaces` (behind the session's "+N") is dropped, not drawn.
	void drawFooterJoins(const TeamController& team, const std::vector<SessionFace>& sessionFaces);

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
	// Replaces the OS file dialog behind "Choose Picture...": returns the path
	// to use, empty for "cancelled". Empty function (the default): the editor
	// opens the SDL dialog; a build without it (the tests) does nothing.
	void setPictureChooser(std::function<std::string()> choose);
}
