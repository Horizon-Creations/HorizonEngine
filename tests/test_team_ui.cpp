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

	enum class Surface { Page, Panel };

	// One editor's worth of UI: the config strings, the form, and what the
	// last frame drew where.
	struct Ui
	{
		TeamController&  team;
		std::string      server, teamName, member;
		TeamView::Form   form;
		std::vector<TeamView::ProbedItem> items;

		TeamView::Settings settings() { return { &server, &teamName, &member }; }

		// One frame of `surface` filling the display. `pump` runs before it
		// (network and controllers), the way the editor's frame loop does.
		template <typename Pump>
		void frame(Surface surface, Pump&& pump, bool mouseDown = false, he_ui::Image* shot = nullptr)
		{
			pump();
			const int w = surface == Surface::Page ? kPageW : kPanelW;
			const int h = surface == Surface::Page ? kPageH : kPanelH;
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(float(w), float(h));
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown);
			ImGui::NewFrame();
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
