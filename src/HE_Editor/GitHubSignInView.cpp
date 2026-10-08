#include "GitHubSignInView.h"

#include <cstdio>

#if __has_include(<imgui.h>)
#include "EditorHelp.h"     // "Source Control/<label>" for the buttons
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include <imgui.h>
#endif

namespace GitHubSignInView
{

std::string formatCountdown(int seconds)
{
	if (seconds < 0) seconds = 0;
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%d:%02d", seconds / 60, seconds % 60);
	return buf;
}

#if __has_include(<imgui.h>)
namespace {

namespace Theme = HE::Ed::Theme;

void errorText(const std::string& s)
{
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
	ImGui::TextWrapped("%s", s.c_str());
	ImGui::PopStyleColor();
}

// The code, large, in a framed box across the width — the one thing on screen
// the user has to read off and type somewhere else, so it gets the space.
void drawCode(const std::string& code, const Fonts& fonts)
{
	const float width = ImGui::GetContentRegionAvail().x;
	if (fonts.heading) ImGui::PushFont(fonts.heading, 40.0f);
	const ImVec2 text = ImGui::CalcTextSize(code.c_str());
	const float  padY = 14.0f;
	const ImVec2 p0   = ImGui::GetCursorScreenPos();
	const ImVec2 p1(p0.x + width, p0.y + text.y + padY * 2.0f);

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p0, p1, ImGui::GetColorU32(Theme::warm(0.13f)), 6.0f);
	dl->AddRect(p0, p1, ImGui::GetColorU32(Theme::Accent), 6.0f, 0, 1.5f);
	dl->AddText(ImVec2(p0.x + (width - text.x) * 0.5f, p0.y + padY),
	            ImGui::GetColorU32(Theme::TextHeading), code.c_str());
	if (fonts.heading) ImGui::PopFont();

	// Reserve the box in the layout; the draw list above does not.
	ImGui::Dummy(ImVec2(width, p1.y - p0.y));
}

} // namespace

Action drawBody(const View& v, const Fonts& fonts)
{
	if (v.phase == Phase::Idle) return Action::None;

	HE::Ed::Help::Scope helpScope("Source Control");
	ImGui::PushID("github-signin");
	Action action = Action::None;

	switch (v.phase)
	{
	case Phase::Idle:
		break;

	case Phase::RequestingCode:
		ImGui::TextWrapped("Asking GitHub for a sign-in code\xe2\x80\xa6");
		ImGui::Spacing();
		if (EditorWidgets::button("Cancel", ImVec2(120.0f, 0.0f))) action = Action::Cancel;
		break;

	case Phase::WaitingForUser:
	{
		ImGui::TextWrapped("1. Open the GitHub page below and sign in there, if you "
		                   "are not already.");
		ImGui::TextWrapped("2. Enter this code when GitHub asks for it, then approve "
		                   "Horizon Engine:");
		ImGui::Spacing();
		drawCode(v.userCode, fonts);
		ImGui::Spacing();

		if (EditorWidgets::primaryButton("Copy code & open GitHub", ImVec2(220.0f, 0.0f)))
			action = Action::CopyAndOpen;
		ImGui::SameLine();
		if (EditorWidgets::button("Copy code", ImVec2(120.0f, 0.0f))) action = Action::Copy;
		if (v.copied)
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f), "Copied.");
		}
		ImGui::TextDisabled("%s", v.verificationUri.c_str());

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		ImGui::TextWrapped("Waiting for you to approve on GitHub \xe2\x80\x94 the code "
		                   "expires in %s.", formatCountdown(v.secondsLeft).c_str());
		ImGui::TextDisabled("Access asked for: your repositories (repo) and gists (gist). "
		                    "Your password is only ever typed on github.com.");
		ImGui::Spacing();
		if (EditorWidgets::button("Cancel", ImVec2(120.0f, 0.0f))) action = Action::Cancel;
		break;
	}

	case Phase::Saving:
		ImGui::TextWrapped("Approved. Saving the sign-in and asking GitHub who you "
		                   "are\xe2\x80\xa6");
		break;

	case Phase::SignedIn:
		ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f), "Signed in to GitHub as %s.",
		                   v.login.c_str());
		ImGui::TextWrapped("The token is in git's credential helper (the system "
		                   "keychain) and nowhere else. Repository lists, clones, "
		                   "pushes and issue reports use it from now on.");
		ImGui::Spacing();
		if (EditorWidgets::primaryButton("Close", ImVec2(120.0f, 0.0f))) action = Action::Close;
		break;

	case Phase::Denied:
	case Phase::Expired:
	case Phase::Failed:
	{
		const char* what = v.phase == Phase::Denied
		                 ? "The sign-in was declined on GitHub."
		                 : v.phase == Phase::Expired
		                 ? "The code expired before it was entered."
		                 : "Signing in to GitHub did not work.";
		ImGui::TextWrapped("%s", what);
		if (!v.error.empty()) errorText(v.error);
		ImGui::Spacing();
		// Two literal labels rather than one variable, so the help audit sees both.
		const bool again = v.phase == Phase::Expired
		                 ? EditorWidgets::primaryButton("Get a new code", ImVec2(160.0f, 0.0f))
		                 : EditorWidgets::primaryButton("Try again", ImVec2(160.0f, 0.0f));
		if (again) action = Action::Retry;
		ImGui::SameLine();
		if (EditorWidgets::button("Close", ImVec2(120.0f, 0.0f))) action = Action::Close;
		break;
	}
	}

	ImGui::PopID();
	return action;
}
#else
Action drawBody(const View&, const Fonts&) { return Action::None; }
#endif

} // namespace GitHubSignInView
