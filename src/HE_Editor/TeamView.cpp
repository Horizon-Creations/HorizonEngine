#include "TeamView.h"

#include <algorithm>
#include <cstdio>

#if __has_include(<imgui.h>)
#include "EditorHelp.h"      // "Team/<label>" for every control below
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include <Net/RelayTransport.h>   // the server field's parse check
#include <imgui.h>
#endif

namespace TeamView
{

namespace
{
std::vector<ProbedItem>* s_probe = nullptr;

void wipe(std::string& secret)
{
	std::fill(secret.begin(), secret.end(), '\0');
	secret.clear();
}
} // namespace

void setProbe(std::vector<ProbedItem>* out) { s_probe = out; }

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
	TeamController::Notice n;
	while (team.pollNotice(n))
	{
		form.recent.push_back(std::move(n));
		if (form.recent.size() > Form::kRecent) form.recent.erase(form.recent.begin());
	}
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

// Defined below the two entry points. The entry points come first because they
// open the "Team" help scope every control in this file is looked up under,
// and scripts/editor_help_audit.py reads a file top to bottom.
void drawStatus(TeamController& team);
void drawRecent(const Form& form);
void drawServerField(TeamController& team, const Settings& settings);
void drawSignIn(TeamController& team, const Settings& settings, Form& form);
void drawMembers(TeamController& team, Form& form);
void drawSessions(TeamController& team, Form& form);
void drawAdmin(TeamController& team, Form& form);
void drawOwnPassword(TeamController& team, Form& form);

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
	drawRecent(form);
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
		drawRecent(form);
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
	drawOwnPassword(team, form);
	drawRecent(form);
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
	if (!team.lastError().empty() && s != TeamController::Status::SignedIn)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, Theme::Bad);
		ImGui::TextWrapped("%s", team.lastError().c_str());
		ImGui::PopStyleColor();
	}
}

void drawRecent(const Form& form)
{
	if (form.recent.empty()) return;
	EditorWidgets::subHeading("Recent");
	for (auto it = form.recent.rbegin(); it != form.recent.rend(); ++it)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, it->error ? Theme::Warn : Theme::TextDim);
		ImGui::TextWrapped("%s", it->text.c_str());
		ImGui::PopStyleColor();
	}
}

bool connectionUp(TeamController::Status s)
{
	return s == TeamController::Status::Connecting || s == TeamController::Status::SignedIn ||
	       s == TeamController::Status::Reconnecting;
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
	if (ImGui::BeginTable("##teammembers", 3, ImGuiTableFlags_SizingFixedFit |
	                                              ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("##dot",  ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("##act",  ImGuiTableColumnFlags_WidthFixed);
		for (const auto& m : members)
		{
			ImGui::PushID(m.name.c_str());
			ImGui::TableNextRow(0, ImGui::GetFrameHeight());
			ImGui::TableSetColumnIndex(0);
			presenceDot(m.online, m.online ? Theme::Ok : Theme::TextDim);

			ImGui::TableSetColumnIndex(1);
			ImGui::AlignTextToFramePadding();
			ImGui::TextColored(m.online ? Theme::Text : Theme::TextDim, "%s", m.name.c_str());
			if (m.you || m.admin)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("%s%s%s", m.you ? "you" : "", (m.you && m.admin) ? ", " : "",
				                    m.admin ? "admin" : "");
			}

			ImGui::TableSetColumnIndex(2);
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
