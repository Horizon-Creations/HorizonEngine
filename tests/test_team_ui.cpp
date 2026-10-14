#include "doctest.h"
#include "FakeTeamServer.h"
#include "ImGuiSoftwareRaster.h"

#include "CollabController.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "TeamController.h"
#include "TeamView.h"

#include <HorizonScene/HorizonWorld.h>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

// ── Preferences ▸ Team and Window ▸ Team, driven headless (topic 197) ───────
// TeamView is the drawing half of the team subsystem; here it is drawn over a
// real TeamController signed in to the in-process fake team server
// (FakeTeamServer.h), and pressed the way a person would: the pointer goes to
// where the control was drawn (TeamView::setProbe records that), the button
// goes down and up, and the request has to arrive at the server as its op.
//
// With HE_UI_DUMP_DIR set every state worth looking at is written out as a BMP
// (scripts/he_uishot.py turns them into PNGs) — the screenshots the step's
// report links. The last case does the same against a real collab_server when
// HE_TEST_TEAM_SERVER=host:port names one: two editors, two members, an
// invitation that travels through the real server.

using namespace HE::Ed;
using HE::Net::TeamClient;

namespace
{
	constexpr int kPanelW = 520, kPanelH = 900;
	constexpr int kPageW  = 760, kPageH  = 600;
	constexpr std::uint8_t kBgR = 20, kBgG = 18, kBgB = 15;

	struct Harness
	{
		Harness()
		{
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(float(kPanelW), float(kPanelH));
			io.DeltaTime   = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
#ifdef HE_EDITOR_DEPS_DIR
			const std::string font = std::string(HE_EDITOR_DEPS_DIR) + "/Fonts/Roboto_Condensed-Bold.ttf";
			if (std::FILE* f = std::fopen(font.c_str(), "rb"))
			{
				std::fclose(f);
				ImFontConfig cfg;
				cfg.OversampleH = 2;
				cfg.OversampleV = 2;
				io.FontDefault = io.Fonts->AddFontFromFileTTF(font.c_str(), 15.0f, &cfg);
			}
#endif
			applyHorizonDarkTheme();
		}
		~Harness()
		{
			TeamView::setProbe(nullptr);
			ImGui::DestroyContext();
		}
	};

	void dump(const he_ui::Image& img, const char* name)
	{
		if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
			he_ui::writeBmp(img, std::string(dir) + "/" + name);
	}

	// Footer: the editor's 24 px bar along the bottom of a Page-sized display,
	// holding only the team status (EditorUI.cpp draws the rest of it).
	enum class Surface { Page, Panel, Footer };
	constexpr float kFooterH = 24.0f;

	// One editor's worth of UI: the config strings, the form, and what the
	// last frame drew where.
	struct Ui
	{
		TeamController&  team;
		std::string      server, teamName, member;
		TeamView::Form   form;
		std::vector<TeamView::ProbedItem> items;
		// drawFooterStatus said "open the Team window" since this was last reset.
		bool openTeamWindow = false;

		TeamView::Settings settings() { return { &server, &teamName, &member }; }

		// The footer the way EditorUI.cpp opens it (24 px, 8/4 padding, no
		// focus, no nav), the status right-aligned with the FPS gap beside it.
		void footer(int w, int h)
		{
			items.clear();
			ImGui::SetNextWindowPos(ImVec2(0.0f, float(h) - kFooterH));
			ImGui::SetNextWindowSize(ImVec2(float(w), kFooterH));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
			ImGui::Begin("##EditorFooter", nullptr,
			             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
			             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
			             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration);
			ImGui::PopStyleVar();
			TeamView::setProbe(&items);
			ImGui::TextUnformatted("Ready");
			const float statusX = float(w) - TeamView::footerStatusWidth() - 8.0f - 60.0f;
			ImGui::SameLine(statusX);
			if (TeamView::drawFooterStatus(team, settings(), form)) openTeamWindow = true;
			// Who else is online, left of it with the editor's 16 px gap (the
			// bell and the session's own faces between them are EditorUI's).
			if (const float onlineW = TeamView::footerOnlineWidth(team); onlineW > 0.0f)
			{
				ImGui::SameLine(statusX - 16.0f - onlineW);
				TeamView::drawFooterOnline(team, team.sessionRunning());
			}
			ImGui::End();
			// The overlay pass, after the footer window, as EditorUI.cpp runs it.
			if (TeamView::drawFooterOnlineMenu(team)) openTeamWindow = true;
			TeamView::setProbe(nullptr);
		}

		// One frame of `surface` filling the display. `pump` runs before it
		// (network and controllers), the way the editor's frame loop does.
		template <typename Pump>
		void frame(Surface surface, Pump&& pump, bool mouseDown = false, he_ui::Image* shot = nullptr)
		{
			pump();
			const int w = surface == Surface::Panel ? kPanelW : kPageW;
			const int h = surface == Surface::Panel ? kPanelH : kPageH;
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(float(w), float(h));
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
			ImGui::NewFrame();
			if (surface == Surface::Footer)
			{
				footer(w, h);
				EditorWidgets::drawQueuedHelp();
				ImGui::Render();
				if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), w, h);
				return;
			}
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(float(w), float(h)));
			TeamView::setProbe(&items);
			// The Preferences page is drawn the way EditorSettingsPanel draws it
			// (heading, separator, TeamView::drawSettings), minus the rail and
			// the catalog's session rows, which are not TeamView's.
			ImGui::Begin(surface == Surface::Page ? "Preferences##teampage" : "Team", nullptr,
			             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
			{
				EditorWidgets::WrapText wrap;
				if (surface == Surface::Page)
				{
					ImGui::TextUnformatted("Team");
					ImGui::Separator();
					ImGui::Spacing();
					TeamView::drawSettings(team, settings(), form);
				}
				else
				{
					TeamView::drawPanel(team, settings(), form);
				}
			}
			ImGui::End();
			TeamView::setProbe(nullptr);
			EditorWidgets::drawQueuedHelp();
			ImGui::Render();
			if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), w, h);
		}

		const TeamView::ProbedItem* find(const std::string& label, const std::string& row = {}) const
		{
			for (const auto& it : items)
				if (it.label == label && it.row == row) return &it;
			return nullptr;
		}
		int count(const std::string& label) const
		{
			return static_cast<int>(std::count_if(items.begin(), items.end(),
			                                      [&](const auto& it) { return it.label == label; }));
		}

		// Pointer onto the control, a hover frame, down, up — what a click is to
		// ImGui. False when the control was not drawn (or drawn disabled).
		template <typename Pump>
		bool click(Surface surface, Pump&& pump, const std::string& label, const std::string& row = {})
		{
			const TeamView::ProbedItem* it = find(label, row);
			if (!it || it->disabled) return false;
			const float x = (it->x0 + it->x1) * 0.5f, y = (it->y0 + it->y1) * 0.5f;
			ImGui::GetIO().AddMousePosEvent(x, y);
			frame(surface, pump);
			frame(surface, pump);
			frame(surface, pump, true);
			frame(surface, pump, false);
			frame(surface, pump, false);
			return true;
		}

		// Click into a field and type.
		template <typename Pump>
		bool type(Surface surface, Pump&& pump, const std::string& label, const char* text)
		{
			if (!click(surface, pump, label)) return false;
			ImGui::GetIO().AddInputCharactersUTF8(text);
			frame(surface, pump);
			frame(surface, pump);
			return true;
		}
	};

	std::uint64_t wallMs()
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	// Draws frames (each pumping everything) until `done`, bounded by wall time.
	template <typename Frame, typename Done>
	bool frameUntil(Frame&& frame, Done&& done, std::chrono::seconds limit = std::chrono::seconds(20))
	{
		const auto deadline = std::chrono::steady_clock::now() + limit;
		while (!done())
		{
			if (std::chrono::steady_clock::now() >= deadline) return false;
			frame();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return true;
	}

	bool sawOp(const FakeTeamServer& s, std::uint8_t op)
	{
		return std::find(s.ops.begin(), s.ops.end(), op) != s.ops.end();
	}

	// Pixels inside a probed control's rect that `match` says yes to.
	template <typename Match>
	int pixelsIn(const he_ui::Image& img, const TeamView::ProbedItem& it, Match&& match)
	{
		int n = 0;
		for (int y = int(it.y0); y < int(it.y1); ++y)
			for (int x = int(it.x0); x < int(it.x1); ++x)
			{
				std::uint8_t r, g, b, a;
				img.pixel(x, y, r, g, b, a);
				if (match(r, g, b)) ++n;
			}
		return n;
	}
	// Theme::Bad (242, 117, 102), the X's red, and a blue that is nothing but
	// the face's colour in the case below.
	bool isRed(std::uint8_t r, std::uint8_t g, std::uint8_t b) { return r > 180 && g < 150 && b < 140 && r > g + 60; }
	bool isBlue(std::uint8_t r, std::uint8_t g, std::uint8_t b) { return b > 180 && r < 90 && g < 150; }
}

TEST_CASE("Team UI: the Preferences page signs in with what was typed and keeps no password")
{
	Harness h;
	FakeTeamServer server;
	REQUIRE(server.t);
	CollabController collab;
	TeamController   team(collab);
	Ui ui{ team };
	ui.server   = "127.0.0.1:" + std::to_string(server.t->boundPort());
	ui.teamName = "studio";
	ui.member   = "anna";   // the server's spelling comes back from the roster

	std::uint64_t now = 1;
	auto pump = [&] { server.pump(); collab.update(now); team.update(now); now += 16; };
	auto page = [&] { ui.frame(Surface::Page, pump); };

	ImGui::GetIO().AddMousePosEvent(float(kPageW) - 2.0f, float(kPageH) - 2.0f);
	page();
	page();
	// Nothing to sign in with yet: no password typed.
	REQUIRE(ui.find("Sign In"));
	CHECK(ui.find("Sign In")->disabled);
	CHECK(ui.find("Create Team")->disabled);
	REQUIRE(ui.find("Password##teampassword"));

	REQUIRE(ui.type(Surface::Page, pump, "Password##teampassword", "correct-horse"));
	CHECK(ui.form.password == "correct-horse");
	CHECK_FALSE(ui.find("Sign In")->disabled);
	{
		he_ui::Image img;
		// Pointer off the controls so no tooltip covers the shot.
		ImGui::GetIO().AddMousePosEvent(float(kPageW) - 2.0f, float(kPageH) - 2.0f);
		page();
		ui.frame(Surface::Page, pump, false, &img);
		REQUIRE(img.valid());
		CHECK(img.inkedPixels(kBgR, kBgG, kBgB) > 8000);
		dump(img, "team-page-signed-out.bmp");
	}

	REQUIRE(ui.click(Surface::Page, pump, "Sign In"));
	// Handed to the controller and gone from the form at once — not kept until
	// the server has answered.
	CHECK(ui.form.password.empty());
	REQUIRE(frameUntil(page, [&] { return team.signedIn() && team.members().size() == 2; }));
	CHECK(server.logins == 1);
	CHECK(team.member() == "Anna");
	CHECK(team.isAdmin());

	ImGui::GetIO().AddMousePosEvent(float(kPageW) - 2.0f, float(kPageH) - 2.0f);
	page();
	// Signed in: the names are locked, the password field is gone, Sign Out
	// is the one thing to press.
	CHECK(ui.find("Team##teamname")->disabled);
	CHECK(ui.find("Team Server (host:port)##teamserver")->disabled);
	CHECK(ui.find("Password##teampassword") == nullptr);
	REQUIRE(ui.find("Sign Out"));
	{
		he_ui::Image img;
		ui.frame(Surface::Page, pump, false, &img);
		dump(img, "team-page-signed-in.bmp");
	}

	// Out, and in again with the wrong password: refused, and said so.
	REQUIRE(ui.click(Surface::Page, pump, "Sign Out"));
	CHECK(team.status() == TeamController::Status::SignedOut);
	REQUIRE(ui.type(Surface::Page, pump, "Password##teampassword", "wrong-horse"));
	REQUIRE(ui.click(Surface::Page, pump, "Sign In"));
	REQUIRE(frameUntil(page, [&] { return team.status() == TeamController::Status::Failed; }));
	CHECK_FALSE(team.lastError().empty());
	CHECK(server.logins == 2);
	{
		he_ui::Image img;
		ImGui::GetIO().AddMousePosEvent(float(kPageW) - 2.0f, float(kPageH) - 2.0f);
		page();
		ui.frame(Surface::Page, pump, false, &img);
		dump(img, "team-page-refused.bmp");
	}

	// The refused password was not kept for another try either.
	CHECK(ui.form.password.empty());
	CHECK(ui.find("Sign In")->disabled);
}

TEST_CASE("Team UI: the window shows members, presence and sessions, and its buttons reach the server")
{
	Harness h;
	FakeTeamServer server;
	REQUIRE(server.t);
	server.roster          = { "Anna", "Bob", "Cleo", "Dana" };
	server.alsoOnline      = { "Bob", "Dana" };
	server.ignoreStrangers = true;   // Host Session's relay link lands here too
	server.sessions.push_back({ "s-bob", "Bob", "Bob", true });
	FakeTeamServer::Session dana{ "s-dana", "Dana", "Dana", false };
	dana.invited.insert("Anna");
	server.sessions.push_back(dana);

	CollabController collab;
	TeamController   team(collab);
	Ui ui{ team };
	ui.server   = "127.0.0.1:" + std::to_string(server.t->boundPort());
	ui.teamName = "studio";
	ui.member   = "Anna";

	std::uint64_t now = 1;
	auto pump  = [&] { server.pump(); collab.update(now); team.update(now); now += 16; };
	auto panel = [&] { ui.frame(Surface::Panel, pump); };

	// Signed out, the window is the sign-in form.
	ImGui::GetIO().AddMousePosEvent(float(kPanelW) - 2.0f, float(kPanelH) - 2.0f);
	panel();
	REQUIRE(ui.find("Sign In"));
	CHECK(ui.find("Invite", "Bob") == nullptr);

	REQUIRE(team.signIn(ui.server, ui.teamName, ui.member, "correct-horse"));
	REQUIRE(frameUntil(panel, [&] {
		return team.signedIn() && team.members().size() == 4 && team.sessions().size() == 2;
	}));
	panel();

	// Every other member has an Invite (greyed: nothing hosted yet) and, for
	// an admin, a Remove. Not on our own row.
	CHECK(ui.count("Invite") == 3);
	CHECK(ui.find("Invite", "Anna") == nullptr);
	REQUIRE(ui.find("Invite", "Bob"));
	CHECK(ui.find("Invite", "Bob")->disabled);
	CHECK(ui.count("Remove") == 3);
	// Bob's session is open to the team, Dana's is invitation-only but invites
	// us: both can be joined, only Dana's declined.
	REQUIRE(ui.find("Join", "Bob"));
	CHECK_FALSE(ui.find("Join", "Bob")->disabled);
	REQUIRE(ui.find("Join", "Dana"));
	CHECK_FALSE(ui.find("Join", "Dana")->disabled);
	CHECK(ui.find("Decline", "Bob") == nullptr);
	REQUIRE(ui.find("Decline", "Dana"));
	REQUIRE(ui.find("Host Session"));
	REQUIRE(ui.find("Add Member"));
	CHECK(ui.find("Add Member")->disabled);   // nothing typed
	{
		he_ui::Image img;
		ui.frame(Surface::Panel, pump, false, &img);
		REQUIRE(img.valid());
		CHECK(img.inkedPixels(kBgR, kBgG, kBgB) > 20000);
		dump(img, "team-panel-signed-in.bmp");
	}

	// Decline: op 11 at the server.
	REQUIRE(ui.click(Surface::Panel, pump, "Decline", "Dana"));
	REQUIRE(frameUntil(panel, [&] { return sawOp(server, 11); }));
	// The list is the server's again: no invitation, so Dana's invitation-only
	// session can no longer be joined or declined.
	REQUIRE(frameUntil(panel, [&] { return ui.find("Decline", "Dana") == nullptr; }));
	REQUIRE(ui.find("Join", "Dana"));
	CHECK(ui.find("Join", "Dana")->disabled);

	// Remove takes two presses.
	REQUIRE(ui.click(Surface::Panel, pump, "Remove", "Cleo"));
	CHECK_FALSE(sawOp(server, 3));
	REQUIRE(ui.find("Confirm Remove", "Cleo"));
	CHECK(ui.find("Confirm Remove", "Bob") == nullptr);
	REQUIRE(ui.click(Surface::Panel, pump, "Confirm Remove", "Cleo"));
	REQUIRE(frameUntil(panel, [&] { return sawOp(server, 3); }));
	// The window lists the server's roster again, one row shorter. Waited for
	// before typing below: a row vanishing mid-click moves every field up.
	REQUIRE(frameUntil(panel, [&] { return ui.find("Invite", "Bob") && !ui.find("Invite", "Cleo"); }));

	// Add Member: typed name and password, one press, op 2; the form empties.
	REQUIRE(ui.type(Surface::Panel, pump, "New Member", "Eli"));
	REQUIRE(ui.type(Surface::Panel, pump, "Their Password", "first-pass"));
	REQUIRE(ui.click(Surface::Panel, pump, "Add Member"));
	REQUIRE(frameUntil(panel, [&] { return sawOp(server, 2); }));
	CHECK(ui.form.newName.empty());
	CHECK(ui.form.newPassword.empty());
	REQUIRE(frameUntil(panel, [&] { return ui.find("Invite", "Eli") != nullptr; }));

	// Host: the session is announced (op 6), and now the ONLINE members can be
	// invited — Bob and Dana, not Eli, who has never signed in.
	REQUIRE(ui.click(Surface::Panel, pump, "Host Session"));
	REQUIRE(frameUntil(panel, [&] { return sawOp(server, 6) && !team.hostedSession().empty(); }));
	panel();
	CHECK(ui.find("Host Session") == nullptr);
	REQUIRE(ui.find("Invite", "Bob"));
	CHECK_FALSE(ui.find("Invite", "Bob")->disabled);
	CHECK_FALSE(ui.find("Invite", "Dana")->disabled);
	REQUIRE(ui.find("Invite", "Eli"));
	CHECK(ui.find("Invite", "Eli")->disabled);
	// A session is running here, so the others' cannot be joined meanwhile.
	CHECK(ui.find("Join", "Bob")->disabled);

	REQUIRE(ui.click(Surface::Panel, pump, "Invite", "Bob"));
	REQUIRE(frameUntil(panel, [&] { return sawOp(server, 10); }));
	bool invitedBob = false;
	for (const auto& s : server.sessions)
		if (s.id == team.hostedSession()) invitedBob = s.invited.count("Bob") > 0;
	CHECK(invitedBob);
	{
		he_ui::Image img;
		ImGui::GetIO().AddMousePosEvent(float(kPanelW) - 2.0f, float(kPanelH) - 2.0f);
		panel();
		ui.frame(Surface::Panel, pump, false, &img);
		dump(img, "team-panel-hosting.bmp");
	}

	team.signOut();
}

TEST_CASE("Team UI: your profile — a colour swatch, a picture file, Remove Picture (topic 202)")
{
	Harness h;
	FakeTeamServer server;
	REQUIRE(server.t);
	server.alsoOnline = { "Bob" };
	server.setProfile("Bob", 0x30, 0x90, 0xf0, 8, 0xc0);   // a picture, so the roster shows one
	const std::string picture = (std::filesystem::temp_directory_path() /
	                             ("he_team_ui_face_" + std::to_string(wallMs()) + ".bmp")).string();
	writeBmp(picture, 5, 5, 0xd0, 0x60, 0x20);

	CollabController collab;
	TeamController   team(collab);
	Ui ui{ team };
	ui.server   = "127.0.0.1:" + std::to_string(server.t->boundPort());
	ui.teamName = "studio";
	ui.member   = "Anna";

	std::uint64_t now = 1;
	auto pump  = [&] { server.pump(); collab.update(now); team.update(now); now += 16; };
	auto panel = [&] { ui.frame(Surface::Panel, pump); };

	ImGui::GetIO().AddMousePosEvent(float(kPanelW) - 2.0f, float(kPanelH) - 2.0f);
	REQUIRE(team.signIn(ui.server, ui.teamName, ui.member, "correct-horse"));
	REQUIRE(frameUntil(panel, [&] {
		const HE::Net::TeamMember* bob = nullptr;
		for (const auto& m : team.members()) if (m.name == "Bob") bob = &m;
		return team.signedIn() && bob && bob->avatarSize == 8;
	}));
	panel();
	// No picture of ours yet: Choose, no Remove; the eight preset swatches.
	REQUIRE(ui.find("Choose Picture..."));
	CHECK(ui.find("Remove Picture") == nullptr);
	CHECK(ui.count("##swatch") == 8);
	REQUIRE(ui.find("Use Collaboration Profile"));

	// A swatch: shown at once, sent once it has settled (op 16).
	REQUIRE(ui.click(Surface::Panel, pump, "##swatch", "2"));
	const HE::Net::ParticipantColor want = HE::Net::kParticipantPalette[2];
	CHECK(team.ownColor() == HE::Net::TeamColor{ want.r, want.g, want.b });
	REQUIRE(frameUntil(panel, [&] {
		const auto& p = server.profiles["anna"];
		return p.r == want.r && p.g == want.g && p.b == want.b;
	}));
	CHECK(std::count(server.ops.begin(), server.ops.end(), std::uint8_t(16)) == 1);

	// Choose Picture: the file the chooser hands over is decoded, scaled to
	// 64 px and uploaded (op 17); then Change and Remove are offered.
	TeamView::setPictureChooser([&] { return picture; });
	REQUIRE(ui.click(Surface::Panel, pump, "Choose Picture..."));
	REQUIRE(frameUntil(panel, [&] { return team.self() && team.self()->avatarSize == 64; }));
	CHECK(server.profiles["anna"].size == 64);
	CHECK(server.profiles["anna"].rgba[0] == 0xd0);
	panel();
	REQUIRE(ui.find("Change Picture..."));
	REQUIRE(ui.find("Remove Picture"));
	CHECK(ui.form.profileError.empty());
	{
		he_ui::Image img;
		ImGui::GetIO().AddMousePosEvent(float(kPanelW) - 2.0f, float(kPanelH) - 2.0f);
		panel();
		ui.frame(Surface::Panel, pump, false, &img);
		REQUIRE(img.valid());
		dump(img, "team-panel-profile.bmp");
	}

	// A file that is not an image: said under the picture, nothing sent.
	const int uploads = static_cast<int>(std::count(server.ops.begin(), server.ops.end(), std::uint8_t(17)));
	TeamView::setPictureChooser([&] { return picture + ".missing"; });
	REQUIRE(ui.click(Surface::Panel, pump, "Change Picture..."));
	CHECK(ui.form.profileError.find("could not be read as an image") != std::string::npos);
	CHECK(std::count(server.ops.begin(), server.ops.end(), std::uint8_t(17)) == uploads);

	REQUIRE(ui.click(Surface::Panel, pump, "Remove Picture"));
	REQUIRE(frameUntil(panel, [&] { return team.self()->avatarId == 0; }));
	panel();
	REQUIRE(ui.find("Choose Picture..."));
	CHECK(ui.find("Remove Picture") == nullptr);
	CHECK(ui.form.profileError.empty());

	TeamView::setPictureChooser({});
	std::filesystem::remove(picture);
	team.signOut();
}

TEST_CASE("Team UI: against a server without profiles the section says so (topic 202)")
{
	Harness h;
	FakeTeamServer server;
	REQUIRE(server.t);
	server.noProfiles = true;
	CollabController collab;
	TeamController   team(collab);
	Ui ui{ team };
	std::uint64_t now = 1;
	auto pump  = [&] { server.pump(); collab.update(now); team.update(now); now += 16; };
	auto panel = [&] { ui.frame(Surface::Panel, pump); };
	REQUIRE(team.signIn("127.0.0.1:" + std::to_string(server.t->boundPort()), "studio", "Anna", "correct-horse"));
	REQUIRE(frameUntil(panel, [&] { return team.signedIn() && !team.profilesSupported(); }));
	panel();
	CHECK(ui.find("Choose Picture...") == nullptr);
	CHECK(ui.count("##swatch") == 0);
	REQUIRE(ui.find("Host Session"));   // the rest of the window as before
	team.signOut();
}

TEST_CASE("Team UI: the footer status — a red X opens the sign-in modal, signed in it is your face (topic 202)")
{
	Harness h;
	FakeTeamServer server;
	REQUIRE(server.t);
	server.setProfile("Anna", 40, 110, 240);   // blue, no picture: the face is a blue square
	CollabController collab;
	TeamController   team(collab);
	Ui ui{ team };
	ui.server   = "127.0.0.1:" + std::to_string(server.t->boundPort());
	ui.teamName = "studio";
	ui.member   = "anna";

	std::uint64_t now = 1;
	auto pump   = [&] { server.pump(); collab.update(now); team.update(now); now += 16; };
	auto footer = [&] { ui.frame(Surface::Footer, pump); };
	auto away   = [&] { ImGui::GetIO().AddMousePosEvent(2.0f, 2.0f); };
	auto shot   = [&](const char* name) {
		he_ui::Image img;
		away();
		footer();
		ui.frame(Surface::Footer, pump, false, &img);
		REQUIRE(img.valid());
		dump(img, name);
		return img;
	};
	auto modalOpen = [&] { return ui.find("Password##teampassword") != nullptr; };

	// Signed out: the X, red, in the footer, and no modal until it is pressed.
	away();
	footer();
	footer();
	REQUIRE(ui.find("##teamStatus"));
	CHECK(ui.find("##teamStatus")->y0 >= float(kPageH) - kFooterH);
	CHECK_FALSE(modalOpen());
	{
		const he_ui::Image img = shot("team-footer-signed-out.bmp");
		CHECK(pixelsIn(img, *ui.find("##teamStatus"), isRed) > 10);
	}

	// Pressed: the modal, with name and password; server and team are set, so
	// they are not asked again.
	REQUIRE(ui.click(Surface::Footer, pump, "##teamStatus"));
	REQUIRE(modalOpen());
	CHECK(ui.find("Member##teammember"));
	CHECK(ui.find("Team##teamname") == nullptr);
	REQUIRE(ui.find("Sign In"));
	CHECK(ui.find("Sign In")->disabled);
	CHECK_FALSE(ui.openTeamWindow);

	// A wrong password: refused, the modal stays and says so, the X stays.
	REQUIRE(ui.type(Surface::Footer, pump, "Password##teampassword", "wrong-horse"));
	CHECK_FALSE(ui.find("Sign In")->disabled);
	REQUIRE(ui.click(Surface::Footer, pump, "Sign In"));
	CHECK(ui.form.password.empty());
	REQUIRE(frameUntil(footer, [&] { return team.status() == TeamController::Status::Failed; }));
	CHECK(server.logins == 1);
	CHECK_FALSE(team.lastError().empty());
	footer();
	CHECK(modalOpen());
	shot("team-footer-modal-refused.bmp");

	// The right one, confirmed with Enter: signed in, the modal closes itself
	// and the X becomes Anna's face in her colour.
	REQUIRE(ui.type(Surface::Footer, pump, "Password##teampassword", "correct-horse"));
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
	footer();
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
	footer();
	CHECK(ui.form.password.empty());
	REQUIRE(frameUntil(footer, [&] { return team.signedIn() && team.self() && !team.ownColor().unset(); }));
	CHECK(server.logins == 2);
	footer();
	CHECK_FALSE(modalOpen());
	{
		const he_ui::Image img = shot("team-footer-signed-in.bmp");
		CHECK(pixelsIn(img, *ui.find("##teamStatus"), isBlue) > 20);
		CHECK(pixelsIn(img, *ui.find("##teamStatus"), isRed) == 0);
	}

	// The face asks for the Team window, and opens no modal.
	REQUIRE(ui.click(Surface::Footer, pump, "##teamStatus"));
	CHECK(ui.openTeamWindow);
	CHECK_FALSE(modalOpen());

	// Signed out again: the X is back, and Cancel closes the modal without
	// trying anything.
	team.signOut();
	ui.openTeamWindow = false;
	REQUIRE(ui.click(Surface::Footer, pump, "##teamStatus"));
	REQUIRE(modalOpen());
	REQUIRE(ui.click(Surface::Footer, pump, "Cancel"));
	CHECK_FALSE(modalOpen());
	CHECK(server.logins == 2);
	CHECK_FALSE(ui.openTeamWindow);

	// No team configured yet: the modal asks for it (and the server) too, and
	// keeps asking while it is typed.
	ui.teamName.clear();
	REQUIRE(ui.click(Surface::Footer, pump, "##teamStatus"));
	REQUIRE(modalOpen());
	REQUIRE(ui.find("Team##teamname"));
	CHECK(ui.find("Team Server (host:port)##teamserver"));
	REQUIRE(ui.type(Surface::Footer, pump, "Team##teamname", "studio"));
	CHECK(ui.teamName == "studio");
	CHECK(ui.find("Team##teamname"));
	shot("team-footer-modal-ask-team.bmp");
}

TEST_CASE("Team presence: online, not you, not in the session — matched by name, any case (topic 202)")
{
	std::vector<HE::Net::TeamMember> members(4);
	members[0].name = "Anna"; members[0].online = true; members[0].you = true;
	members[1].name = "Bob";  members[1].online = true;
	members[2].name = "Cleo"; members[2].online = true;
	members[3].name = "Dana";

	auto names = [](const std::vector<const HE::Net::TeamMember*>& v) {
		std::vector<std::string> out;
		for (const auto* m : v) out.push_back(m->name);
		return out;
	};
	CHECK(names(TeamController::onlineOutside(members, {})) == std::vector<std::string>{ "Bob", "Cleo" });
	CHECK(names(TeamController::onlineOutside(members, { "Anna", "bob" })) == std::vector<std::string>{ "Cleo" });
	CHECK(TeamController::onlineOutside(members, { "BOB", "Cleo" }).empty());
	// Someone in the session who is not in the team changes nothing.
	CHECK(names(TeamController::onlineOutside(members, { "Guest" })) == std::vector<std::string>{ "Bob", "Cleo" });
}

TEST_CASE("Team UI: the footer shows who else is online; joining the session takes them out of it (topic 202)")
{
	Harness h;
	FakeTeamServer server;
	REQUIRE(server.t);
	server.roster     = { "Anna", "Bob", "Cleo", "Dana" };
	server.alsoOnline = { "Bob", "Cleo" };      // Dana is offline
	server.setProfile("Bob", 40, 110, 240);    // blue, no picture: a blue square

	HorizonWorld     world, bobWorld;
	CollabController collab, bobCollab;        // Anna's editor, and Bob's for the session
	collab.setWorld(&world);
	bobCollab.setWorld(&bobWorld);
	TeamController team(collab);
	Ui ui{ team };
	ui.server   = "127.0.0.1:" + std::to_string(server.t->boundPort());
	ui.teamName = "studio";
	ui.member   = "anna";

	std::uint64_t now = 1;
	auto pump   = [&] { server.pump(); collab.update(now); bobCollab.update(now); team.update(now); now += 16; };
	auto footer = [&] { ui.frame(Surface::Footer, pump); };
	auto away   = [&] { ImGui::GetIO().AddMousePosEvent(2.0f, 2.0f); };
	auto shot   = [&](const char* name) {
		he_ui::Image img;
		away();
		footer();
		ui.frame(Surface::Footer, pump, false, &img);
		REQUIRE(img.valid());
		dump(img, name);
		return img;
	};
	auto face = [&](const char* who) { return ui.find("##teamOnlineFace", who); };

	// Signed out: nobody is shown, whatever the server knows.
	away();
	footer();
	CHECK(TeamView::footerOnlineWidth(team) == 0.0f);
	CHECK(ui.find("##teamOnline") == nullptr);

	// Signed in as Anna: Bob and Cleo, left of her own status, in the footer;
	// not Anna (that is the status) and not Dana (offline).
	REQUIRE(team.signIn(ui.server, ui.teamName, ui.member, "correct-horse"));
	REQUIRE(frameUntil(footer, [&] { return team.signedIn() && face("Bob") && face("Cleo"); }));
	footer();
	CHECK(face("Anna") == nullptr);
	CHECK(face("Dana") == nullptr);
	REQUIRE(ui.find("##teamOnline"));
	REQUIRE(ui.find("##teamStatus"));
	CHECK(ui.find("##teamOnline")->x1 <= ui.find("##teamStatus")->x0 - 16.0f);
	CHECK(ui.find("##teamOnline")->y0 >= float(kPageH) - kFooterH);
	CHECK(face("Bob")->x0 < face("Cleo")->x0);    // roster order
	{
		const he_ui::Image img = shot("team-footer-online.bmp");
		CHECK(pixelsIn(img, *face("Bob"), isBlue) > 20);
		// The green "online" dot in the face's corner (Theme::Ok).
		CHECK(pixelsIn(img, *face("Cleo"), [](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
			return g > 150 && g > r + 40 && g > b + 40;
		}) > 3);
	}

	// A click pins the menu (step 4); its Open Team Window asks for the window.
	REQUIRE(ui.click(Surface::Footer, pump, "##teamOnline"));
	REQUIRE(ui.click(Surface::Footer, pump, "Open Team Window"));
	CHECK(ui.openTeamWindow);
	ui.openTeamWindow = false;
	away();
	footer();
	CHECK(ui.find("Open Team Window") == nullptr);

	// Anna hosts, Bob joins: he is in the session now, so he leaves the online
	// cluster (the session's own faces show him) and Cleo stays.
	REQUIRE(collab.startHosting(0, "Anna"));
	REQUIRE(bobCollab.joinSession("127.0.0.1", collab.port(), collab.joinCode(), "Bob"));
	REQUIRE(frameUntil(footer, [&] { return collab.participants().size() == 2; }));
	footer();
	CHECK(face("Bob") == nullptr);
	REQUIRE(face("Cleo"));
	{
		const he_ui::Image img = shot("team-footer-online-bob-in-session.bmp");
		CHECK(pixelsIn(img, *ui.find("##teamOnline"), isBlue) == 0);
	}

	// Bob leaves the session: back on the online side.
	bobCollab.leave();
	REQUIRE(frameUntil(footer, [&] { return face("Bob") != nullptr; }));

	// Everyone in the session: the slot is gone, not an empty cluster.
	collab.leave();
	REQUIRE(collab.startHosting(0, "Anna"));
	HorizonWorld cleoWorld;
	CollabController cleoCollab;
	cleoCollab.setWorld(&cleoWorld);
	REQUIRE(bobCollab.joinSession("127.0.0.1", collab.port(), collab.joinCode(), "Bob"));
	REQUIRE(cleoCollab.joinSession("127.0.0.1", collab.port(), collab.joinCode(), "cleo"));
	REQUIRE(frameUntil(footer, [&] {
		cleoCollab.update(now);
		return collab.participants().size() == 3;
	}));
	footer();
	CHECK(TeamView::footerOnlineWidth(team) == 0.0f);
	CHECK(ui.find("##teamOnline") == nullptr);
	cleoCollab.leave();
	bobCollab.leave();
	collab.leave();

	// Signed out: gone too.
	team.signOut();
	footer();
	CHECK(ui.find("##teamOnline") == nullptr);
}

TEST_CASE("Team UI: the online faces stack; hovering lists every one with Invite, never Remove (topic 202)")
{
	Harness h;
	FakeTeamServer server;
	REQUIRE(server.t);
	server.roster          = { "Anna", "Bob", "Cleo", "Dana", "Eli", "Fay", "Gus" };
	server.alsoOnline      = { "Bob", "Cleo", "Dana", "Eli", "Fay", "Gus" };
	server.ignoreStrangers = true;   // Host Session's relay link lands here too
	server.setProfile("Bob", 40, 110, 240);

	CollabController collab;
	TeamController   team(collab);
	Ui ui{ team };
	ui.server   = "127.0.0.1:" + std::to_string(server.t->boundPort());
	ui.teamName = "studio";
	ui.member   = "Anna";   // an admin: the Team window would offer her Remove

	std::uint64_t now = 1;
	auto pump   = [&] { server.pump(); collab.update(now); team.update(now); now += 16; };
	auto footer = [&] { ui.frame(Surface::Footer, pump); };
	auto away   = [&] { ImGui::GetIO().AddMousePosEvent(2.0f, 2.0f); };
	auto face   = [&](const char* who) { return ui.find("##teamOnlineFace", who); };
	auto hover  = [&] {
		const TeamView::ProbedItem* c = ui.find("##teamOnline");
		REQUIRE(c);
		ImGui::GetIO().AddMousePosEvent((c->x0 + c->x1) * 0.5f, (c->y0 + c->y1) * 0.5f);
		footer();
		footer();
	};
	const char* everyone[] = { "Bob", "Cleo", "Dana", "Eli", "Fay", "Gus" };

	away();
	REQUIRE(team.signIn(ui.server, ui.teamName, ui.member, "correct-horse"));
	REQUIRE(frameUntil(footer, [&] { return team.signedIn() && team.members().size() == 7 && face("Bob"); }));
	footer();

	// The stack: four faces, overlapping in roster order, the rest a count.
	CHECK(ui.count("##teamOnlineFace") == 4);
	REQUIRE(face("Eli"));
	CHECK(face("Fay") == nullptr);
	const float faceW = face("Bob")->x1 - face("Bob")->x0;
	CHECK(face("Bob")->x0 < face("Cleo")->x0);
	CHECK(face("Bob")->x1 > face("Cleo")->x0);
	CHECK(face("Eli")->x1 - face("Bob")->x0 < 3.0f * faceW);
	// No menu while the mouse is elsewhere.
	CHECK(ui.find("Invite", "Bob") == nullptr);
	{
		he_ui::Image img;
		ui.frame(Surface::Footer, pump, false, &img);
		REQUIRE(img.valid());
		dump(img, "team-footer-online-stack.bmp");
		// Bob is on top: his whole face is his blue, not cut by Cleo's.
		CHECK(pixelsIn(img, *face("Bob"), isBlue) > int(faceW * faceW * 0.4f));
	}

	// Hovering opens the menu above the stack: all six on their own row, each
	// with an Invite (greyed, nothing hosted), and no Remove anywhere.
	hover();
	for (const char* who : everyone)
	{
		INFO(who);
		REQUIRE(ui.find("Invite", who));
		CHECK(ui.find("Invite", who)->disabled);
	}
	CHECK(ui.count("Invite") == 6);
	CHECK(ui.find("Invite", "Anna") == nullptr);
	CHECK(ui.count("Remove") == 0);
	CHECK(ui.count("Confirm Remove") == 0);
	REQUIRE(ui.find("Open Team Window"));
	CHECK(ui.find("Invite", "Gus")->y1 <= ui.find("##teamOnline")->y0);
	{
		he_ui::Image img;
		ui.frame(Surface::Footer, pump, false, &img);
		REQUIRE(img.valid());
		dump(img, "team-footer-online-menu.bmp");
	}

	// Onto the menu: it stays. Away from both: it closes.
	{
		const TeamView::ProbedItem* gus = ui.find("Invite", "Gus");
		ImGui::GetIO().AddMousePosEvent(gus->x0 - 40.0f, (gus->y0 + gus->y1) * 0.5f);
		footer();
		footer();
		CHECK(ui.find("Invite", "Gus") != nullptr);
	}
	away();
	footer();
	footer();
	CHECK(ui.find("Invite", "Bob") == nullptr);

	// Host a team session: now the menu's Invite reaches the server (op 10).
	REQUIRE(team.hostSession(false));
	REQUIRE(frameUntil(footer, [&] { return sawOp(server, 6) && !team.hostedSession().empty(); }));
	footer();
	REQUIRE(ui.find("##teamOnline"));   // Anna alone in the session: everyone else still here
	hover();
	REQUIRE(ui.find("Invite", "Eli"));
	CHECK_FALSE(ui.find("Invite", "Eli")->disabled);
	REQUIRE(ui.click(Surface::Footer, pump, "Invite", "Eli"));
	REQUIRE(frameUntil(footer, [&] { return sawOp(server, 10); }));
	// The menu stayed open under the press (the button held down is the
	// active item; the menu must still count as hovered then).
	CHECK(ui.find("Invite", "Eli") != nullptr);
	bool invitedEli = false;
	for (const auto& s : server.sessions)
		if (s.id == team.hostedSession()) invitedEli = s.invited.count("Eli") > 0;
	CHECK(invitedEli);
	{
		he_ui::Image img;
		ui.frame(Surface::Footer, pump, false, &img);
		dump(img, "team-footer-online-menu-hosting.bmp");
	}

	// A click on the stack pins the menu: it stays with the mouse gone, and a
	// click anywhere else lets it go.
	away();
	footer();
	REQUIRE(ui.click(Surface::Footer, pump, "##teamOnline"));
	away();
	footer();
	footer();
	CHECK(ui.find("Invite", "Bob") != nullptr);
	ui.frame(Surface::Footer, pump, true);
	ui.frame(Surface::Footer, pump, false);
	footer();
	CHECK(ui.find("Invite", "Bob") == nullptr);

	// Signed out, stack and menu are gone, pinned or not.
	REQUIRE(ui.click(Surface::Footer, pump, "##teamOnline"));
	collab.leave();
	team.signOut();
	footer();
	footer();
	CHECK(ui.find("##teamOnline") == nullptr);
	CHECK(ui.find("Invite", "Bob") == nullptr);
}

TEST_CASE("Team UI: two editors through a real collab_server (HE_TEST_TEAM_SERVER=host:port)")
{
	const char* env = std::getenv("HE_TEST_TEAM_SERVER");
	if (!env || !*env)
	{
		MESSAGE("HE_TEST_TEAM_SERVER not set — skipped (start collab_server and point it there)");
		return;
	}
	Harness h;

	struct Editor
	{
		HorizonWorld     world;
		CollabController collab;
		TeamController   team{ collab };
		Ui               ui{ team };
		Editor() { collab.setWorld(&world); }
		void pump()
		{
			const std::uint64_t now = wallMs();
			collab.update(now);
			team.update(now);
		}
	};
	Editor anna, bob;
	// A fresh team per run: the server keeps teams on disk.
	const std::string teamName = "uishot" + std::to_string(wallMs() % 1000000);
	for (Editor* e : { &anna, &bob })
	{
		e->ui.server   = env;
		e->ui.teamName = teamName;
	}
	anna.ui.member = "Anna";
	bob.ui.member  = "Bob";

	auto pumpBoth = [&] { anna.pump(); bob.pump(); };
	auto annaPanel = [&] { anna.ui.frame(Surface::Panel, pumpBoth); };
	auto bobPanel  = [&] { bob.ui.frame(Surface::Panel, pumpBoth); };

	// Anna creates the team through the window's own Create Team.
	ImGui::GetIO().AddMousePosEvent(float(kPanelW) - 2.0f, float(kPanelH) - 2.0f);
	annaPanel();
	REQUIRE(anna.ui.type(Surface::Panel, pumpBoth, "Password##teampassword", "anna-secret-1"));
	REQUIRE(anna.ui.click(Surface::Panel, pumpBoth, "Create Team"));
	REQUIRE(frameUntil(annaPanel, [&] { return anna.team.signedIn() && anna.team.isAdmin(); }));

	// …adds Bob with the Add Member form…
	REQUIRE(anna.ui.type(Surface::Panel, pumpBoth, "New Member", "Bob"));
	REQUIRE(anna.ui.type(Surface::Panel, pumpBoth, "Their Password", "bob-secret-1"));
	REQUIRE(anna.ui.click(Surface::Panel, pumpBoth, "Add Member"));
	REQUIRE(frameUntil(annaPanel, [&] { return anna.team.members().size() == 2; }));

	// …Bob signs in from his window and both see each other online.
	bobPanel();
	REQUIRE(bob.ui.type(Surface::Panel, pumpBoth, "Password##teampassword", "bob-secret-1"));
	REQUIRE(bob.ui.click(Surface::Panel, pumpBoth, "Sign In"));
	auto online = [](const TeamController& t, const char* who) {
		for (const auto& m : t.members())
			if (m.name == who) return m.online;
		return false;
	};
	REQUIRE(frameUntil(bobPanel, [&] { return bob.team.signedIn() && online(bob.team, "Anna"); }));
	REQUIRE(frameUntil(annaPanel, [&] { return online(anna.team, "Bob"); }));

	// Anna hosts (invitation only) and invites Bob from his row.
	annaPanel();
	REQUIRE(anna.ui.click(Surface::Panel, pumpBoth, "Host Session"));
	REQUIRE(frameUntil(annaPanel, [&] { return !anna.team.hostedSession().empty(); }));
	annaPanel();
	REQUIRE(anna.ui.click(Surface::Panel, pumpBoth, "Invite", "Bob"));
	REQUIRE(frameUntil(bobPanel, [&] {
		for (const auto& s : bob.team.sessions())
			if (s.invited) return true;
		return false;
	}));
	{
		he_ui::Image img;
		ImGui::GetIO().AddMousePosEvent(float(kPanelW) - 2.0f, float(kPanelH) - 2.0f);
		annaPanel();
		anna.ui.frame(Surface::Panel, pumpBoth, false, &img);
		dump(img, "team-live-anna-hosting.bmp");
		bobPanel();
		bob.ui.frame(Surface::Panel, pumpBoth, false, &img);
		dump(img, "team-live-bob-invited.bmp");
	}

	// Bob presses Join: the team server answers and the collab session starts.
	bobPanel();
	REQUIRE(bob.ui.find("Join", "Anna"));
	CHECK(bob.ui.find("Decline", "Anna"));
	REQUIRE(bob.ui.click(Surface::Panel, pumpBoth, "Join", "Anna"));
	REQUIRE(frameUntil(bobPanel, [&] { return bob.collab.inSession(); }, std::chrono::seconds(30)));
	{
		he_ui::Image img;
		ImGui::GetIO().AddMousePosEvent(float(kPanelW) - 2.0f, float(kPanelH) - 2.0f);
		bobPanel();
		bob.ui.frame(Surface::Panel, pumpBoth, false, &img);
		dump(img, "team-live-bob-joined.bmp");
	}
	CHECK(bob.team.inSession(anna.team.hostedSession()));

	bob.collab.leave();
	anna.collab.leave();
	bob.team.signOut();
	anna.team.signOut();
}
