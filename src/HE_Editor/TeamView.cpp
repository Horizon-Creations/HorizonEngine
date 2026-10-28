#include "TeamView.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <set>

#if __has_include(<imgui.h>)
#include "EditorHelp.h"      // "Team/<label>" for every control below
#include "NotificationStore.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include <Net/CollabSession.h>    // kParticipantPalette, the colour presets
#include <Net/RelayTransport.h>   // the server field's parse check
#include <imgui.h>
#endif
#if defined(HE_IMGUI_ENABLED) && __has_include(<SDL3/SDL_dialog.h>)
#include <SDL3/SDL_dialog.h>
#define HE_TEAMVIEW_FILE_DIALOG 1
#endif

namespace TeamView
{

namespace
{
std::vector<ProbedItem>* s_probe = nullptr;

// "Choose Picture...": the OS dialog answers on whichever thread it likes, so
// the path waits here until the next frame picks it up (as in CollabPanel).
std::function<std::string()> s_chooser;
std::string                  s_pickPath;
std::atomic<bool>            s_pickReady{ false };

void wipe(std::string& secret)
{
	std::fill(secret.begin(), secret.end(), '\0');
	secret.clear();
}
} // namespace

void setProbe(std::vector<ProbedItem>* out) { s_probe = out; }
void setPictureChooser(std::function<std::string()> choose) { s_chooser = std::move(choose); }

void Form::wipeSecrets()
{
	wipe(password);
	wipe(newPassword);
	wipe(oldPassword);
	wipe(changedPassword);
}

Form& editorForm()
{
	static Form form;
	return form;
}

const char* statusText(TeamController::Status s)
{
	switch (s)
	{
	case TeamController::Status::SignedOut:    return "Not signed in";
	case TeamController::Status::Connecting:   return "Signing in\xe2\x80\xa6";
	case TeamController::Status::SignedIn:     return "Signed in";
	case TeamController::Status::Reconnecting: return "Connection lost \xe2\x80\x94 reconnecting\xe2\x80\xa6";
	case TeamController::Status::Failed:       return "Not signed in";
	}
	return "";
}

void collectNotices(TeamController& team, Form& form)
{
	(void)form;
	TeamController::Notice n;
	while (team.pollNotice(n)) {}
}

#if __has_include(<imgui.h>)
namespace
{
namespace Theme = HE::Ed::Theme;

// Records the item just submitted for the test seam (see setProbe).
void probe(const char* label, const std::string& row = {})
{
	if (!s_probe) return;
	const ImVec2 mn = ImGui::GetItemRectMin();
	const ImVec2 mx = ImGui::GetItemRectMax();
	s_probe->push_back({ label, row, mn.x, mn.y, mx.x, mx.y,
	                     (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0 });
}

// The same for something drawn without an item of its own (a face inside a
// cluster that has one hit box).
void probeRect(const char* label, const std::string& row, ImVec2 mn, ImVec2 mx)
{
	if (!s_probe) return;
	s_probe->push_back({ label, row, mn.x, mn.y, mx.x, mx.y, false });
}

// Defined below the two entry points. The entry points come first because they
// open the "Team" help scope every control in this file is looked up under,
// and scripts/editor_help_audit.py reads a file top to bottom.
void drawStatus(TeamController& team);
void drawServerField(TeamController& team, const Settings& settings);
void drawSignIn(TeamController& team, const Settings& settings, Form& form);
void drawMembers(TeamController& team, Form& form);
void drawSessions(TeamController& team, Form& form);
void drawAdmin(TeamController& team, Form& form);
void drawProfile(TeamController& team, Form& form);
void drawOwnPassword(TeamController& team, Form& form);
bool connectionUp(TeamController::Status s);
// `roundness`: corner radius as a share of the size; 0.5 is a circle (the
// joining slide turns a team face into a session face on the way).
void faceAt(ImDrawList* dl, ImVec2 p, const HE::Net::TeamMember* m, HE::Net::TeamColor colour,
            const std::string& name, float size, float roundness = 0.2f);
void face(const HE::Net::TeamMember* m, HE::Net::TeamColor colour, const std::string& name, float size);
void drawSignInModal(TeamController& team, const Settings& settings, Form& form);

constexpr const char* kSignInModal = "Team Sign-In###teamSignIn";

} // namespace

void drawSettings(TeamController& team, const Settings& settings, Form& form)
{
	HE::Ed::Help::Scope helpScope("Team");
	if (s_probe) s_probe->clear();
	collectNotices(team, form);
	drawServerField(team, settings);
	EditorWidgets::hint("The team server keeps the team's members and passwords, shows who is "
	                    "online and is where team sessions are announced. It is also the relay "
	                    "those sessions run on, and the one sessions hosted from the "
	                    "Collaboration window use: leave it empty to work peer to peer without "
	                    "a team. Port 7777 if none is given; IPv6 addresses go in brackets.");
	ImGui::Spacing();
	drawSignIn(team, settings, form);
	EditorWidgets::hint("The password is never written to disk. Signed in, this computer keeps "
	                    "a session token from the team server and signs in with it after a "
	                    "restart, until you sign out. Create Team makes a new team on the server "
	                    "with you as its first admin. Members, presence and invitations are in "
	                    "Window \xc2\xbb Team.");
	if (team.signedIn())
	{
		ImGui::Spacing();
		drawProfile(team, form);
	}
}

void drawPanel(TeamController& team, const Settings& settings, Form& form)
{
	HE::Ed::Help::Scope helpScope("Team");
	if (s_probe) s_probe->clear();
	collectNotices(team, form);

	if (team.status() != TeamController::Status::SignedIn &&
	    team.status() != TeamController::Status::Reconnecting)
	{
		drawServerField(team, settings);
		drawSignIn(team, settings, form);
		EditorWidgets::hint("Members sign in with the name and password an admin gave them. "
		                    "Create Team starts a new team with you as its admin.");
		return;
	}

	drawStatus(team);
	const bool out = EditorWidgets::smallButton("Sign Out");
	probe("Sign Out");
	bool everywhere = false;
	if (team.signedIn())
	{
		ImGui::SameLine();
		everywhere = EditorWidgets::smallButton("Sign Out Everywhere");
		probe("Sign Out Everywhere");
	}
	if (out || everywhere)
	{
		if (everywhere) team.signOutEverywhere();
		else            team.signOut();
		form.wipeSecrets();
		return;
	}

	drawMembers(team, form);
	drawSessions(team, form);
	if (team.isAdmin()) drawAdmin(team, form);
	ImGui::Spacing();
	if (team.signedIn()) drawProfile(team, form);
	drawOwnPassword(team, form);
}

float footerStatusWidth()
{
	return ImGui::GetTextLineHeight();
}

bool drawFooterStatus(TeamController& team, const Settings& settings, Form& form)
{
	HE::Ed::Help::Scope helpScope("Team");
	const TeamController::Status s    = team.status();
	const float                  size = footerStatusWidth();

	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##teamStatus", ImVec2(size, size));
	probe("##teamStatus");
	const bool clicked = ImGui::IsItemClicked();
	const bool hovered = ImGui::IsItemHovered();

	ImDrawList* dl = ImGui::GetWindowDrawList();
	if (s == TeamController::Status::SignedIn)
	{
		faceAt(dl, p, team.self(), team.ownColor(), team.member(), size);
	}
	else
	{
		// Two strokes, not a glyph, for the bell's reason (NotificationBar.cpp):
		// the font is not guaranteed to carry one. Amber while a sign-in is
		// under way: nothing to fix yet, but not signed in either.
		const ImU32 col   = Theme::u32(s == TeamController::Status::Connecting ? Theme::Warn : Theme::Bad);
		const float inset = std::floor(size * 0.22f);
		const float thick = std::max(1.5f, size * 0.13f);
		dl->AddLine(ImVec2(p.x + inset, p.y + inset), ImVec2(p.x + size - inset, p.y + size - inset), col, thick);
		dl->AddLine(ImVec2(p.x + size - inset, p.y + inset), ImVec2(p.x + inset, p.y + size - inset), col, thick);
	}

	if (hovered && !ImGui::IsPopupOpen(kSignInModal))
	{
		switch (s)
		{
		case TeamController::Status::SignedIn:
			ImGui::SetTooltip("Signed in to the team %s as %s. Click to open the Team window.",
			                  team.team().c_str(), team.member().c_str());
			break;
		case TeamController::Status::Connecting:
			ImGui::SetTooltip("Signing in to the team server\xe2\x80\xa6");
			break;
		case TeamController::Status::Reconnecting:
			ImGui::SetTooltip("The connection to the team server dropped; retrying on its own. "
			                  "Click to sign in again now.");
			break;
		case TeamController::Status::Failed:
			ImGui::SetTooltip("Not signed in to a team: %s\nClick to sign in.", team.lastError().c_str());
			break;
		default:
			ImGui::SetTooltip("Not signed in to a team. Click to sign in.");
			break;
		}
	}

	if (clicked && s != TeamController::Status::SignedIn)
		ImGui::OpenPopup(kSignInModal);
	drawSignInModal(team, settings, form);
	return clicked && s == TeamController::Status::SignedIn;
}

namespace
{
// Enough faces to see at a glance who is around, then a count: the same cap
// as the session's cluster, so the two together cannot run into the counters.
constexpr int   kOnlineShown = 4;
// How much of a face the next one leaves showing. Stacked, unlike the
// session's faces (CollabPresenceBar.cpp has why those are not): the seam
// here is a dark halo rather than a plug of the footer's colour, so it does
// not care what colour the footer is.
constexpr float kOnlineStep  = 0.6f;

// Split out so footerOnlineWidth() and drawFooterOnline() cannot disagree.
struct OnlineLayout
{
	int   shown  = 0;
	int   extra  = 0;
	float face   = 0.0f;
	float step   = 0.0f;   // left edge to left edge, whole pixels
	float width  = 0.0f;
	float extraW = 0.0f;
	char  extraLabel[16] = {};
};

OnlineLayout onlineLayout(std::size_t online)
{
	OnlineLayout l;
	if (online == 0) return l;
	l.face  = footerStatusWidth();
	l.step  = std::round(l.face * kOnlineStep);
	l.shown = std::min<int>(kOnlineShown, static_cast<int>(online));
	l.extra = static_cast<int>(online) - l.shown;
	if (l.extra > 0)
	{
		std::snprintf(l.extraLabel, sizeof(l.extraLabel), "+%d", l.extra);
		l.extraW = ImGui::CalcTextSize(l.extraLabel).x + 4.0f;
	}
	l.width = l.face + (l.shown - 1) * l.step + l.extraW;
	return l;
}

// ── The menu's state ──
// Written by drawFooterOnline, read by drawFooterOnlineMenu later in the same
// frame (see TeamView.h for why the two are apart).
ImVec2 s_onlineMin { 0.0f, 0.0f };
ImVec2 s_onlineMax { 0.0f, 0.0f };
bool   s_onlineHovered = false;
bool   s_onlineDrawn   = false;
// A click pins the menu, so its buttons can be reached without keeping the
// mouse in a corridor all the way there.
bool   s_onlinePinned   = false;
bool   s_onlineMenuOpen = false;
// Whom this editor invited from the menu, for the session it hosts then: the
// row says "invited" so a press visibly did something. Forgotten with the
// session.
std::string           s_invitedFor;
std::set<std::string> s_invited;

// ── The joining slide's state ──
// Where each member stood in the stack: this frame's, written by
// drawFooterOnline, and last frame's, which is what a slide starts from (by
// the time anyone can tell that Bob joined, the stack has already been laid
// out without him). Members behind the "+N" stand where the "+N" is.
struct StackFace
{
	std::string name;
	ImVec2      at;
	float       size = 0.0f;
};
std::vector<StackFace> s_stackNow;
std::vector<StackFace> s_stackLast;
// The faces on their way. Name, start and time only: the member and the
// target are looked up again every frame (members() pointers last one
// update(), and the session's cluster moves when someone else joins too).
struct JoinSlide
{
	std::string name;
	ImVec2      from;
	float       fromSize = 0.0f;
	double      start    = 0.0;
};
std::vector<JoinSlide> s_slides;
int                    s_joinFrame = -1;

const HE::Net::TeamMember* memberNamed(const TeamController& team, const std::string& name)
{
	for (const HE::Net::TeamMember& m : team.members())
		if (m.name == name) return &m;
	return nullptr;
}

// Once per frame, from whichever of the footer's calls comes first: the
// session cluster asks footerJoining() before the stack is drawn, and must
// already get the answer for this frame, or the face it should leave out is
// drawn once at its target before the slide starts.
void stepJoins(const TeamController& team)
{
	const int frame = ImGui::GetFrameCount();
	if (frame == s_joinFrame) return;
	s_joinFrame = frame;
	// Last frame's stack is complete now; this frame's is yet to be drawn.
	s_stackLast = std::move(s_stackNow);
	s_stackNow.clear();
	if (!team.signedIn())
	{
		s_slides.clear();
		s_stackLast.clear();
		return;
	}

	const std::vector<const HE::Net::TeamMember*> online = team.onlineOutsideSession();
	auto inStack = [&](const std::string& name) {
		return std::any_of(online.begin(), online.end(),
		                   [&](const HE::Net::TeamMember* m) { return m->name == name; });
	};
	// Still online, not you, and no longer outside the session: by the rule
	// that fills the stack, that is "in the session". Gone offline is not.
	auto joined = [&](const std::string& name) {
		const HE::Net::TeamMember* m = memberNamed(team, name);
		return m && m->online && !m->you && !inStack(name);
	};

	const double now = ImGui::GetTime();
	s_slides.erase(std::remove_if(s_slides.begin(), s_slides.end(),
	                              [&](const JoinSlide& s) {
		                              return now - s.start >= kJoinSlideSeconds || !joined(s.name);
	                              }),
	               s_slides.end());
	for (const StackFace& f : s_stackLast)
	{
		if (!joined(f.name)) continue;
		const bool sliding = std::any_of(s_slides.begin(), s_slides.end(),
		                                 [&](const JoinSlide& s) { return s.name == f.name; });
		if (!sliding) s_slides.push_back({ f.name, f.at, f.size, now });
	}
}
} // namespace

float footerOnlineWidth(const TeamController& team)
{
	return onlineLayout(team.onlineOutsideSession().size()).width;
}

void drawFooterOnline(TeamController& team, bool sessionBeside)
{
	s_onlineDrawn = false;
	stepJoins(team);
	const std::vector<const HE::Net::TeamMember*> online = team.onlineOutsideSession();
	const OnlineLayout l = onlineLayout(online.size());
	if (l.shown == 0) return;

	// One hit box for the whole cluster, as for the session's faces: the menu
	// belongs to the group, not to whichever face the mouse lands on.
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##teamOnline", ImVec2(l.width, l.face));
	probe("##teamOnline");
	if (ImGui::IsItemClicked()) s_onlinePinned = !s_onlinePinned;
	s_onlineHovered = ImGui::IsItemHovered();
	s_onlineMin     = origin;
	s_onlineMax     = ImVec2(origin.x + l.width, origin.y + l.face);
	s_onlineDrawn   = true;

	// Right to left, so the leftmost face is on top and every face keeps its
	// right-hand side, the one with the dot, in view.
	ImDrawList* dl = ImGui::GetWindowDrawList();
	for (int i = l.shown - 1; i >= 0; --i)
	{
		const HE::Net::TeamMember& m = *online[static_cast<std::size_t>(i)];
		const ImVec2 p(origin.x + i * l.step, origin.y);
		// The seam to the face underneath: dark, like the dot's ring below,
		// and only where it falls on that face, so the stack's outer edge
		// stays the faces' own.
		if (i + 1 < l.shown)
		{
			dl->PushClipRect(ImVec2(p.x + l.step, p.y), ImVec2(p.x + l.face + 2.0f, p.y + l.face), true);
			dl->AddRect(ImVec2(p.x - 1.0f, p.y - 1.0f), ImVec2(p.x + l.face + 1.0f, p.y + l.face + 1.0f),
			            IM_COL32(0, 0, 0, 170), l.face * 0.2f + 1.0f, 0, 2.0f);
			dl->PopClipRect();
		}
		faceAt(dl, p, &m, m.color, m.name, l.face);
		// The green dot in the corner says "online", and is the mark the
		// session's faces never carry: those people are not online somewhere,
		// they are here.
		const float  r = std::max(2.5f, l.face * 0.17f);
		const ImVec2 c(p.x + l.face - r * 0.8f, p.y + l.face - r * 0.8f);
		dl->AddCircleFilled(c, r + 1.0f, IM_COL32(0, 0, 0, 170));
		dl->AddCircleFilled(c, r, Theme::u32(Theme::Ok));
		probeRect("##teamOnlineFace", m.name, p, ImVec2(p.x + l.face, p.y + l.face));
		s_stackNow.push_back({ m.name, p, l.face });
	}
	if (l.extra > 0)
	{
		dl->AddText(ImVec2(origin.x + l.width - l.extraW + 4.0f,
		                   origin.y + (l.face - ImGui::GetTextLineHeight()) * 0.5f),
		            ImGui::GetColorU32(ImGuiCol_TextDisabled), l.extraLabel);
		for (std::size_t i = static_cast<std::size_t>(l.shown); i < online.size(); ++i)
			s_stackNow.push_back({ online[i]->name, ImVec2(origin.x + l.width - l.extraW, origin.y), l.face });
	}

	// The rule between "online in the team" and "in this session", in the
	// middle of the footer's 16 px gap.
	if (sessionBeside)
	{
		const float x = std::floor(origin.x + l.width + 8.0f) + 0.5f;
		dl->AddLine(ImVec2(x, origin.y + 1.0f), ImVec2(x, origin.y + l.face - 1.0f),
		            Theme::u32(Theme::TextDim), 1.0f);
	}

}

namespace
{
// The menu itself: everyone the stack stands for, one row each. Returns
// whether the mouse is inside it, which is what keeps it open on the way from
// the stack to a button.
bool drawOnlineMenuWindow(TeamController& team, bool& openTeamWindow)
{
	const std::vector<const HE::Net::TeamMember*> online = team.onlineOutsideSession();
	const std::string& hosted = team.hostedSession();
	if (hosted != s_invitedFor)
	{
		s_invitedFor = hosted;
		s_invited.clear();
	}

	// Sitting right on top of the stack, no gap, right edges together like
	// the session's menu (CollabPresenceBar::DrawOverlay), so the mouse can
	// cross from one to the other without leaving both.
	constexpr float kWidth = 260.0f;
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	const float x = std::max(vp->WorkPos.x + 4.0f,
	                         std::min(s_onlineMax.x - kWidth, vp->WorkPos.x + vp->WorkSize.x - kWidth - 4.0f));
	ImGui::SetNextWindowPos(ImVec2(x, s_onlineMin.y), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
	ImGui::SetNextWindowSize(ImVec2(kWidth, 0.0f), ImGuiCond_Always);
	ImGui::SetNextWindowViewport(vp->ID);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));

	bool hovered = false;
	if (ImGui::Begin("##teamOnlineMenu", nullptr,
	                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
	                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
	                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing |
	                 ImGuiWindowFlags_AlwaysAutoResize))
	{
		// AllowWhenBlockedByActiveItem: while a button here is held down it is
		// the active item, and without the flag the window reads as not
		// hovered on exactly that frame, so the menu closed under the press
		// and Invite never fired.
		hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup |
		                                 ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
		                                 ImGuiHoveredFlags_ChildWindows);
		ImGui::TextDisabled(team.sessionRunning() ? "%zu online in the team, not in this session"
		                                          : "%zu online in the team",
		                    online.size());
		ImGui::Separator();

		// Invite only: the same rule as the Team window's, which is where
		// Remove stays (behind its second press). Everyone listed here is
		// online, so hosting is the whole condition.
		const bool  hosting = !hosted.empty();
		const float rowH    = ImGui::GetFrameHeight();
		const float inviteW = ImGui::CalcTextSize("Invite").x + ImGui::GetStyle().FramePadding.x * 2.0f;
		for (const HE::Net::TeamMember* m : online)
		{
			ImGui::PushID(m->name.c_str());
			face(m, m->color, m->name, rowH);
			ImGui::SameLine();
			// The name is clipped short of the button rather than wrapped, so
			// every row is one row tall.
			const float  buttonX = ImGui::GetWindowContentRegionMax().x - inviteW;
			const ImVec2 at      = ImGui::GetCursorScreenPos();
			const float  clipX   = ImGui::GetWindowPos().x + buttonX - 6.0f;
			ImGui::PushClipRect(at, ImVec2(clipX, at.y + rowH), true);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(m->name.c_str());
			if (s_invited.count(m->name))
			{
				ImGui::SameLine();
				ImGui::TextDisabled("invited");
			}
			ImGui::PopClipRect();
			ImGui::SameLine(buttonX);
			ImGui::BeginDisabled(!hosting);
			if (EditorWidgets::smallButton("Invite") && team.invite(m->name)) s_invited.insert(m->name);
			probe("Invite", m->name);
			if (!hosting && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", team.sessionRunning()
				                            ? "Invitations come from the editor hosting a team session; "
				                              "this one is not."
				                            : "Host a session first (Window > Team).");
			ImGui::EndDisabled();
			ImGui::PopID();
		}

		ImGui::Separator();
		if (EditorWidgets::smallButton("Open Team Window")) openTeamWindow = true;
		probe("Open Team Window");
	}
	ImGui::End();
	ImGui::PopStyleVar();
	return hovered;
}
} // namespace

bool drawFooterOnlineMenu(TeamController& team)
{
	HE::Ed::Help::Scope helpScope("Team");
	// Consumed here: a frame whose footer left the stack out (nobody to show,
	// signed out) must not open the menu from last frame's leftovers.
	const bool drawn = s_onlineDrawn;
	s_onlineDrawn = false;
	if (!drawn)
	{
		s_onlineMenuOpen = false;
		s_onlinePinned   = false;
		s_onlineHovered  = false;
		return false;
	}

	if (s_onlineHovered) s_onlineMenuOpen = true;
	bool menuHovered    = false;
	bool openTeamWindow = false;
	if (s_onlineMenuOpen || s_onlinePinned) menuHovered = drawOnlineMenuWindow(team, openTeamWindow);

	if (!s_onlineHovered && !menuHovered && !s_onlinePinned) s_onlineMenuOpen = false;
	// A click anywhere else unpins, the way a menu behaves everywhere else;
	// so does leaving for the Team window.
	if ((s_onlinePinned && !s_onlineHovered && !menuHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ||
	    openTeamWindow)
	{
		s_onlinePinned   = false;
		s_onlineMenuOpen = false;
	}
	return openTeamWindow;
}

bool footerJoining(const TeamController& team, const std::string& name)
{
	stepJoins(team);
	return std::any_of(s_slides.begin(), s_slides.end(), [&](const JoinSlide& s) {
		return TeamController::sameMemberName(s.name, name);
	});
}

void drawFooterJoins(const TeamController& team, const std::vector<SessionFace>& sessionFaces)
{
	stepJoins(team);
	if (s_slides.empty()) return;

	const double now = ImGui::GetTime();
	ImDrawList*  dl  = ImGui::GetWindowDrawList();
	for (auto it = s_slides.begin(); it != s_slides.end();)
	{
		const auto to = std::find_if(sessionFaces.begin(), sessionFaces.end(), [&](const SessionFace& f) {
			return TeamController::sameMemberName(f.name, it->name);
		});
		const HE::Net::TeamMember* m = memberNamed(team, it->name);
		if (to == sessionFaces.end() || !m)
		{
			// Behind the session's "+N", or no session cluster at all: there
			// is no face to land on, and the cluster shows them as it would
			// have anyway.
			it = s_slides.erase(it);
			continue;
		}

		// Ease-out: off quickly, settling into place.
		const float t = std::clamp(static_cast<float>((now - it->start) / kJoinSlideSeconds), 0.0f, 1.0f);
		const float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
		auto lerp = [e](float a, float b) { return a + (b - a) * e; };
		const float  size = lerp(it->fromSize, to->size);
		const ImVec2 p(std::round(lerp(it->from.x, to->x)), std::round(lerp(it->from.y, to->y)));

		// The team's square rounds into the session's circle, and the green
		// "online" dot goes: where it lands, they are not online somewhere,
		// they are here.
		faceAt(dl, p, m, m->color, m->name, size, lerp(0.2f, 0.5f));
		const float  r = std::max(2.5f, size * 0.17f);
		const ImVec2 c(p.x + size - r * 0.8f, p.y + size - r * 0.8f);
		const float  a = 1.0f - e;
		dl->AddCircleFilled(c, r + 1.0f, IM_COL32(0, 0, 0, static_cast<int>(170 * a)));
		ImVec4 ok = Theme::Ok;
		ok.w *= a;
		dl->AddCircleFilled(c, r, Theme::u32(ok));
		probeRect("##teamJoining", m->name, p, ImVec2(p.x + size, p.y + size));
		++it;
	}
}

namespace
{

ImVec4 statusColour(TeamController::Status s)
{
	switch (s)
	{
	case TeamController::Status::SignedIn:     return Theme::Ok;
	case TeamController::Status::Connecting:
	case TeamController::Status::Reconnecting: return Theme::Warn;
	case TeamController::Status::Failed:       return Theme::Bad;
	default:                                   return Theme::TextDim;
	}
}

// A filled dot for "online", a ring for "offline": the shape says it as well
// as the colour, for whoever cannot tell the green from the grey.
void presenceDot(bool online, const ImVec4& colour)
{
	const float  h  = ImGui::GetFrameHeight();
	const ImVec2 p  = ImGui::GetCursorScreenPos();
	const ImVec2 c(p.x + h * 0.5f, p.y + h * 0.5f);
	ImDrawList*  dl = ImGui::GetWindowDrawList();
	if (online) dl->AddCircleFilled(c, h * 0.22f, Theme::u32(colour));
	else        dl->AddCircle(c, h * 0.20f, Theme::u32(colour), 0, 1.5f);
	ImGui::Dummy(ImVec2(h, h));
}

ImU32 colourU32(HE::Net::TeamColor c)
{
	return c.unset() ? Theme::u32(Theme::TextDim) : IM_COL32(c.r, c.g, c.b, 255);
}

// A member's face: their picture if there is one, else their colour with the
// initial on it, framed in their colour either way. Drawn from the pixels with
// rectangles rather than a texture: this file has no renderer (see TeamView.h),
// and a face is a few hundred rectangles at most.
void faceAt(ImDrawList* dl, ImVec2 p, const HE::Net::TeamMember* m, HE::Net::TeamColor colour,
            const std::string& name, float size, float roundness)
{
	const ImVec2 q(p.x + size, p.y + size);
	const ImU32  frame    = colourU32(colour);
	const float  rounding = size * roundness;

	if (m && m->avatarSize > 0 && m->avatarRgba.size() == std::size_t(m->avatarSize) * m->avatarSize * 4)
	{
		// One rectangle per cell, a cell no smaller than ~1.5 px: a 64 px picture
		// in a 19 px row is 12x12 cells, in the 48 px preview 32x32.
		const int   src   = m->avatarSize;
		const int   cells = std::max(1, std::min(src, static_cast<int>(size / 1.5f)));
		const float cell  = size / static_cast<float>(cells);
		dl->AddRectFilled(p, q, Theme::u32(Theme::TextDim), rounding);
		for (int y = 0; y < cells; ++y)
			for (int x = 0; x < cells; ++x)
			{
				const int sx = (x * src + src / 2) / cells;
				const int sy = (y * src + src / 2) / cells;
				const std::uint8_t* px = &m->avatarRgba[(std::size_t(sy) * src + sx) * 4];
				// Edges on whole pixels, shared by neighbours: no gaps between
				// cells, and no overlap that doubles a translucent pixel's alpha
				// into a visible grid.
				const ImVec2 a(std::floor(p.x + x * cell), std::floor(p.y + y * cell));
				const ImVec2 b(std::floor(p.x + (x + 1) * cell), std::floor(p.y + (y + 1) * cell));
				dl->AddRectFilled(a, b, IM_COL32(px[0], px[1], px[2], px[3]));
			}
	}
	else
	{
		dl->AddRectFilled(p, q, frame, rounding);
		if (!name.empty())
		{
			const char initial[2] = { static_cast<char>(std::toupper(static_cast<unsigned char>(name[0]))), 0 };
			const float  fontSize = size * 0.6f;
			const ImVec2 ts       = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, initial);
			// Dark text on a light colour, light on a dark one.
			const float lum = colour.unset() ? 0.4f
			                                 : (0.299f * colour.r + 0.587f * colour.g + 0.114f * colour.b) / 255.0f;
			dl->AddText(ImGui::GetFont(), fontSize,
			            ImVec2(p.x + (size - ts.x) * 0.5f, p.y + (size - ts.y) * 0.5f),
			            lum > 0.6f ? IM_COL32(20, 20, 20, 255) : IM_COL32(245, 245, 245, 255), initial);
		}
	}
	dl->AddRect(p, q, frame, rounding, 0, std::max(1.5f, size * 0.06f));
}

// The same, as a layout item at the cursor.
void face(const HE::Net::TeamMember* m, HE::Net::TeamColor colour, const std::string& name, float size)
{
	faceAt(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), m, colour, name, size);
	ImGui::Dummy(ImVec2(size, size));
}

// One line: a dot in the status colour, the status, and what the controller
// last refused (a wrong password, an address that does not parse).
void drawStatus(TeamController& team)
{
	const TeamController::Status s = team.status();
	presenceDot(s == TeamController::Status::SignedIn, statusColour(s));
	ImGui::SameLine(0.0f, 2.0f);
	ImGui::AlignTextToFramePadding();
	if (s == TeamController::Status::SignedIn)
		ImGui::TextColored(statusColour(s), "Signed in to %s as %s%s", team.team().c_str(),
		                   team.member().c_str(), team.isAdmin() ? " (admin)" : "");
	else
		ImGui::TextColored(statusColour(s), "%s", statusText(s));

	if (s == TeamController::Status::SignedIn || s == TeamController::Status::Reconnecting)
		EditorWidgets::hint("Team server %s", team.serverText().c_str());
	// The reason a sign-in failed is a notification (TeamController), not a red line
	// here that stays after the sign-in form is gone.
}

bool connectionUp(TeamController::Status s)
{
	return s != TeamController::Status::SignedOut && s != TeamController::Status::Failed;
}

void drawServerField(TeamController& team, const Settings& settings)
{
	// Locked while a connection exists: it, the sessions it announced and the
	// relay they run on all hang off this address, and a field that could
	// change under them would show something the editor is not doing.
	ImGui::BeginDisabled(connectionUp(team.status()));
	EditorWidgets::Row::inputText("Team Server (host:port)##teamserver", settings.server);
	probe("Team Server (host:port)##teamserver");
	ImGui::EndDisabled();
	// Said right under the field, while it is being typed: a malformed address
	// is otherwise only discovered at Sign In.
	if (!settings.server->empty())
	{
		HE::Net::RelayAddress parsed;
		std::string why;
		if (!HE::Net::parseRelayAddress(*settings.server, parsed, &why))
			ImGui::TextColored(Theme::Warn, "Not usable: %s", why.c_str());
	}
}

// Team, member, password and the two ways in. While a connection exists the
// names are what it signed in with, so the fields are locked and Sign Out is
// the one thing to press.
void drawSignIn(TeamController& team, const Settings& settings, Form& form)
{
	const bool busy = connectionUp(team.status());

	ImGui::BeginDisabled(busy);
	EditorWidgets::Row::inputText("Team##teamname", settings.team);
	probe("Team##teamname");
	EditorWidgets::Row::inputText("Member##teammember", settings.member);
	probe("Member##teammember");
	if (!busy)
	{
		EditorWidgets::Row::inputText("Password##teampassword", &form.password,
		                              ImGuiInputTextFlags_Password);
		probe("Password##teampassword");
	}
	ImGui::EndDisabled();

	ImGui::Spacing();
	if (busy)
	{
		const bool out = EditorWidgets::button("Sign Out");
		probe("Sign Out");
		if (out)
		{
			team.signOut();
			form.wipeSecrets();
		}
	}
	else
	{
		const bool ready = !settings.server->empty() && !settings.team->empty() &&
		                   !settings.member->empty() && !form.password.empty();
		ImGui::BeginDisabled(!ready);
		const bool in = EditorWidgets::primaryButton("Sign In");
		probe("Sign In");
		ImGui::SameLine();
		const bool create = EditorWidgets::button("Create Team");
		probe("Create Team");
		ImGui::EndDisabled();
		// The controller keeps its own copy for the reconnect; this one goes.
		if (in)     team.signIn(*settings.server, *settings.team, *settings.member, form.password);
		if (create) team.createTeam(*settings.server, *settings.team, *settings.member, form.password);
		if (in || create) form.wipeSecrets();
	}
	ImGui::Spacing();
	drawStatus(team);
}

// The footer's sign-in, opened from the X right of the bell: name and password,
// and Sign In tries again — after a refused password, a saved sign-in the
// server no longer takes, or a connection that will not come back. Server and
// team belong to Preferences ▸ Team; they are asked here too only when one was
// empty as the modal opened (decided once, or the rows would vanish with the
// first letter typed), since without them there is nothing to try.
void drawSignInModal(TeamController& team, const Settings& settings, Form& form)
{
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
	                        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	// Fixed width, height fitted: wrapped text and an auto-sized width would
	// chase each other.
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26.0f, 0.0f), ImGuiCond_Always);
	if (!ImGui::BeginPopupModal(kSignInModal, nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

	const TeamController::Status s = team.status();
	// Through: nothing left to ask. The face takes the X's place.
	if (s == TeamController::Status::SignedIn)
	{
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	const bool connecting = s == TeamController::Status::Connecting;

	static bool s_askWhere = false;
	if (ImGui::IsWindowAppearing())
		s_askWhere = settings.server->empty() || settings.team->empty();
	if (s_askWhere)
	{
		drawServerField(team, settings);
		ImGui::BeginDisabled(connectionUp(s));
		EditorWidgets::Row::inputText("Team##teamname", settings.team);
		probe("Team##teamname");
		ImGui::EndDisabled();
	}
	else
	{
		EditorWidgets::hint("Team %s on %s (Preferences \xc2\xbb Team).", settings.team->c_str(),
		                    settings.server->c_str());
	}

	ImGui::BeginDisabled(connecting);
	EditorWidgets::Row::inputText("Member##teammember", settings.member);
	probe("Member##teammember");
	if (ImGui::IsWindowAppearing() && !settings.member->empty()) ImGui::SetKeyboardFocusHere();
	const bool enter = EditorWidgets::Row::inputText("Password##teampassword", &form.password,
	                                                 ImGuiInputTextFlags_Password |
	                                                 ImGuiInputTextFlags_EnterReturnsTrue);
	probe("Password##teampassword");
	ImGui::EndDisabled();

	ImGui::Spacing();
	drawStatus(team);
	ImGui::Spacing();

	const bool ready = !connecting && !settings.server->empty() && !settings.team->empty() &&
	                   !settings.member->empty() && !form.password.empty();
	ImGui::BeginDisabled(!ready);
	const bool in = EditorWidgets::primaryButton("Sign In");
	probe("Sign In");
	ImGui::EndDisabled();
	ImGui::SameLine();
	const bool cancel = EditorWidgets::button("Cancel");
	probe("Cancel");

	// The modal stays open on the attempt: a refusal is read right here, and
	// success closes it (above).
	if (in || (enter && ready))
	{
		team.signIn(*settings.server, *settings.team, *settings.member, form.password);
		form.wipeSecrets();
	}
	if (cancel)
	{
		form.wipeSecrets();
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

// ── Signed in: the team ──────────────────────────────────────────────────────

void drawMembers(TeamController& team, Form& form)
{
	const auto& members = team.members();
	int online = 0;
	for (const auto& m : members) online += m.online ? 1 : 0;
	char heading[64];
	std::snprintf(heading, sizeof(heading), "Members (%d of %d online)", online,
	              static_cast<int>(members.size()));
	EditorWidgets::subHeading(heading);

	const bool hosting = !team.hostedSession().empty();
	const bool admin   = team.isAdmin();
	// Constant row height (a frame) so the rows stay a list and not a layout
	// that moves when somebody comes online and grows a button.
	if (ImGui::BeginTable("##teammembers", 4, ImGuiTableFlags_SizingFixedFit |
	                                              ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("##dot",  ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		ImGui::TableSetupColumn("##face", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("##act",  ImGuiTableColumnFlags_WidthFixed);
		for (const auto& m : members)
		{
			ImGui::PushID(m.name.c_str());
			ImGui::TableNextRow(0, ImGui::GetFrameHeight());
			ImGui::TableSetColumnIndex(0);
			presenceDot(m.online, m.online ? Theme::Ok : Theme::TextDim);

			// The face is the row's height, so a picture arriving later does
			// not move anything.
			ImGui::TableSetColumnIndex(1);
			face(&m, m.you ? team.ownColor() : m.color, m.name, ImGui::GetFrameHeight());

			ImGui::TableSetColumnIndex(2);
			ImGui::AlignTextToFramePadding();
			// A name and its "you, admin" tag are one line, never a paragraph. The
			// panel's wrap guard wraps at the edge of the CELL here, and the cell
			// is whatever the buttons column leaves (an armed "Confirm Remove" is
			// the widest it gets): the tag then broke letter by letter into a tall
			// column of single characters and the row grew to several times its
			// height. Unwrapped, and the tag shortens to what fits whole — "you,
			// admin", then "you" or "admin", then nothing — never half a word.
			ImGui::PushTextWrapPos(-1.0f);
			ImGui::TextColored(m.online ? Theme::Text : Theme::TextDim, "%s", m.name.c_str());
			if (m.you || m.admin)
			{
				const char* const tags[] = {
					(m.you && m.admin) ? "you, admin" : nullptr,
					m.you ? "you" : "admin",
				};
				// What is left of the cell after the name and the gap before the tag.
				const float room = ImGui::GetContentRegionAvail().x -
				                   ImGui::CalcTextSize(m.name.c_str()).x - ImGui::GetStyle().ItemSpacing.x;
				for (const char* tag : tags)
					if (tag && ImGui::CalcTextSize(tag).x <= room)
					{
						ImGui::SameLine();
						ImGui::TextDisabled("%s", tag);
						break;
					}
			}
			ImGui::PopTextWrapPos();

			ImGui::TableSetColumnIndex(3);
			if (!m.you)
			{
				// Only an online member can be invited: the server delivers the
				// invitation to their running editors and keeps none for later.
				const bool canInvite = hosting && m.online;
				ImGui::BeginDisabled(!canInvite);
				if (EditorWidgets::smallButton("Invite")) team.invite(m.name);
				probe("Invite", m.name);
				if (!canInvite && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip("%s", !hosting ? "Host a session first."
					                                 : "Only members who are online can be invited.");
				ImGui::EndDisabled();
				if (admin)
				{
					ImGui::SameLine();
					// Two presses: removing somebody signs out every editor of
					// theirs, and a misclick in a list is easy.
					if (form.removeArmed == m.name)
					{
						const bool confirm = EditorWidgets::dangerSmallButton("Confirm Remove");
						probe("Confirm Remove", m.name);
						if (confirm)
						{
							team.removeMember(m.name);
							form.removeArmed.clear();
						}
					}
					else
					{
						if (EditorWidgets::smallButton("Remove")) form.removeArmed = m.name;
						probe("Remove", m.name);
					}
				}
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	EditorWidgets::hint("A filled dot is online: at least one editor of theirs is signed in.");
}

void drawSessions(TeamController& team, Form& form)
{
	EditorWidgets::subHeading("Sessions");

	if (!team.hostedSession().empty())
	{
		ImGui::TextColored(Theme::Ok, "You are hosting a session.");
		bool open = team.hostedSessionOpen();
		if (EditorWidgets::checkbox("Anyone in the Team May Join", &open)) team.setSessionOpen(open);
		probe("Anyone in the Team May Join");
		EditorWidgets::hint(open ? "Members join from their Team window without an invitation."
		                         : "Invitation only: press Invite next to an online member.");
	}
	else
	{
		EditorWidgets::checkbox("Anyone in the Team May Join", &form.hostOpen);
		probe("Anyone in the Team May Join");
		ImGui::BeginDisabled(team.sessionRunning());
		if (EditorWidgets::primaryButton("Host Session")) team.hostSession(form.hostOpen);
		probe("Host Session");
		ImGui::EndDisabled();
		if (team.sessionRunning())
			EditorWidgets::hint("A collaboration session is already running; leave it in the "
			                    "Collaboration window first.");
		else
			EditorWidgets::hint("Starts a collaboration session through the team server and "
			                    "announces it to the team.");
	}

	// The others' sessions. The session id is never shown: for a team session
	// it is the key (TeamClient::joinCodeForSession), and a screenshot of this
	// window must not hand it out.
	ImGui::Spacing();
	int others = 0;
	for (const auto& s : team.sessions())
	{
		if (s.hosting) continue;
		++others;
		ImGui::PushID(s.session.c_str());
		const bool here = team.inSession(s.session);
		ImGui::AlignTextToFramePadding();
		ImGui::Text("%s's session", s.host.c_str());
		ImGui::SameLine();
		if (here)           ImGui::TextColored(Theme::Ok, "joined");
		else if (s.invited) ImGui::TextColored(Theme::AccentHi, "invited you");
		else if (s.open)    ImGui::TextDisabled("open to the team");
		else                ImGui::TextDisabled("invitation only");
		if (!here)
		{
			ImGui::SameLine();
			ImGui::BeginDisabled(!(s.open || s.invited) || team.sessionRunning());
			if (EditorWidgets::smallButton("Join")) team.join(s.session);
			probe("Join", s.host);
			ImGui::EndDisabled();
			if (s.invited)
			{
				ImGui::SameLine();
				if (EditorWidgets::smallButton("Decline")) team.decline(s.session);
				probe("Decline", s.host);
			}
		}
		ImGui::PopID();
	}
	if (others == 0) ImGui::TextDisabled("Nobody else in the team is hosting a session.");
}

void drawAdmin(TeamController& team, Form& form)
{
	EditorWidgets::subHeading("Add Member");
	EditorWidgets::Row::inputText("New Member", &form.newName);
	probe("New Member");
	EditorWidgets::Row::inputText("Their Password", &form.newPassword, ImGuiInputTextFlags_Password);
	probe("Their Password");
	EditorWidgets::checkbox("Admin", &form.newAdmin);
	probe("Admin");
	ImGui::BeginDisabled(form.newName.empty() || form.newPassword.empty());
	const bool add = EditorWidgets::button("Add Member");
	probe("Add Member");
	ImGui::EndDisabled();
	if (add)
	{
		team.addMember(form.newName, form.newPassword, form.newAdmin);
		form.newName.clear();
		wipe(form.newPassword);
		form.newAdmin = false;
	}
	EditorWidgets::hint("Tell them the password yourself; they can change it once signed in.");
}

// ── Your profile ─────────────────────────────────────────────────────────────

void openPictureDialog()
{
	if (s_chooser)
	{
		s_pickPath = s_chooser();
		s_pickReady.store(true, std::memory_order_release);
		return;
	}
#ifdef HE_TEAMVIEW_FILE_DIALOG
	static const SDL_DialogFileFilter kFilters[] = { { "Images", "png;jpg;jpeg;bmp;tga;gif" } };
	SDL_ShowOpenFileDialog(
		[](void* /*userdata*/, const char* const* filelist, int /*filter*/)
		{
			// Our own slot, not ctx.dialogBridge: that one is read as a scene or
			// project to open (see CollabPanel's picture picker).
			s_pickPath = (filelist && filelist[0]) ? filelist[0] : "";
			s_pickReady.store(true, std::memory_order_release);
		},
		nullptr, nullptr, kFilters, 1, nullptr, false);
#endif
}

void consumePicturePick(TeamController& team, Form& form)
{
	if (!s_pickReady.load(std::memory_order_acquire)) return;
	s_pickReady.store(false, std::memory_order_relaxed);
	const std::string path = s_pickPath;
	s_pickPath.clear();
	if (path.empty()) return;   // cancelled
	std::string error;
	if (!team.setAvatarFromFile(path, error))
		HE::Ed::notify(HE::Ed::NoteLevel::Important, "That picture cannot be used", error);
	(void)form;
}

// The presets the Collaboration window offers, None, and a free picker. A pick
// shows at once; the controller sends it once it has stopped changing.
void drawColourRow(TeamController& team, HE::Net::TeamColor mine)
{
	constexpr float kSwatch = 18.0f;
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("Colour");
	int index = 0;
	for (const HE::Net::ParticipantColor& preset : HE::Net::kParticipantPalette)
	{
		ImGui::SameLine(index == 0 ? ImGui::GetFontSize() * 4.0f : 0.0f, index == 0 ? -1.0f : 3.0f);
		ImGui::PushID(index);
		const HE::Net::TeamColor c{ preset.r, preset.g, preset.b };
		const ImVec4 col(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);
		const bool   chosen = !mine.unset() && mine == c;
		ImGui::PushStyleColor(ImGuiCol_Button,        col);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,  col);
		ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(1, 1, 1, chosen ? 1.0f : 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, chosen ? 2.0f : 0.0f);
		if (ImGui::Button("##swatch", ImVec2(kSwatch, kSwatch))) team.setColor(c);
		probe("##swatch", std::to_string(index));
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(4);
		ImGui::PopID();
		++index;
	}

	ImGui::SameLine();
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, mine.unset() ? 2.0f : 0.0f);
	ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, mine.unset() ? 1.0f : 0.0f));
	if (EditorWidgets::button("None##teamcolour", ImVec2(0.0f, kSwatch))) team.setColor({});
	probe("None##teamcolour");
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();

	ImGui::SameLine();
	float custom[3] = { mine.r / 255.0f, mine.g / 255.0f, mine.b / 255.0f };
	if (ImGui::ColorEdit3("Custom##teamcolour", custom,
	                      ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
	{
		HE::Net::TeamColor picked;
		picked.r = static_cast<std::uint8_t>(std::clamp(custom[0], 0.0f, 1.0f) * 255.0f + 0.5f);
		picked.g = static_cast<std::uint8_t>(std::clamp(custom[1], 0.0f, 1.0f) * 255.0f + 0.5f);
		picked.b = static_cast<std::uint8_t>(std::clamp(custom[2], 0.0f, 1.0f) * 255.0f + 0.5f);
		// Black is "no colour" on the wire; nudged, so picking it is not None.
		if (picked.unset()) picked.r = picked.g = picked.b = 1;
		team.setColor(picked);
	}
	EditorWidgets::helpForLabel("Custom");
	probe("Custom##teamcolour");
}

void drawProfile(TeamController& team, Form& form)
{
	EditorWidgets::subHeading("Your Profile");
	if (!team.profilesSupported())
	{
		EditorWidgets::hint("This team server keeps no colour or picture per member yet; it needs "
		                    "an update.");
		return;
	}
	consumePicturePick(team, form);

	const HE::Net::TeamMember* me     = team.self();
	const HE::Net::TeamColor   colour = team.ownColor();
	// Two rows tall, the height of the buttons beside it.
	const float faceSize = ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
	face(me, colour, team.member(), faceSize);
	ImGui::SameLine();
	ImGui::BeginGroup();
	const bool  hasPicture = me && me->avatarId != 0;
	const char* chooseLabel = hasPicture ? "Change Picture..." : "Choose Picture...";
	const bool  choose = EditorWidgets::button(chooseLabel);
	probe(chooseLabel);
	bool remove = false;
	if (hasPicture)
	{
		ImGui::SameLine();
		remove = EditorWidgets::dangerSmallButton("Remove Picture");
		probe("Remove Picture");
	}
	const bool useCollab = EditorWidgets::button("Use Collaboration Profile");
	probe("Use Collaboration Profile");
	ImGui::EndGroup();

	if (choose)    openPictureDialog();
	if (remove)    team.clearAvatar();
	if (useCollab) team.useCollabProfile();

	drawColourRow(team, colour);
	EditorWidgets::hint("The team sees this picture and colour next to your name, on every "
	                    "editor of theirs and yours. Use Collaboration Profile copies the ones "
	                    "from the Collaboration window. In a session the host may still give you "
	                    "another colour if yours is taken.");
}

void drawOwnPassword(TeamController& team, Form& form)
{
	if (!ImGui::CollapsingHeader("Your Password")) return;
	EditorWidgets::Row::inputText("Current Password", &form.oldPassword, ImGuiInputTextFlags_Password);
	probe("Current Password");
	EditorWidgets::Row::inputText("New Password", &form.changedPassword, ImGuiInputTextFlags_Password);
	probe("New Password");
	ImGui::BeginDisabled(form.oldPassword.empty() || form.changedPassword.empty());
	const bool change = EditorWidgets::button("Change Password");
	probe("Change Password");
	ImGui::EndDisabled();
	if (change)
	{
		team.changePassword(form.oldPassword, form.changedPassword);
		wipe(form.oldPassword);
		wipe(form.changedPassword);
	}
}

} // namespace
#endif

} // namespace TeamView
