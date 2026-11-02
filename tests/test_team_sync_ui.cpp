#include "doctest.h"
#include "FakeScServer.h"
#include "ImGuiSoftwareRaster.h"

#include "CollabController.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "GitController.h"
#include "TeamController.h"
#include "TeamSyncController.h"
#include "TeamSyncView.h"

#include <HorizonScene/HorizonWorld.h>

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

// ── Saving through the team server, the editor's half (topic 199, step 4) ───
// TeamSyncController over a real TeamController and GitController, signed in
// to the in-process fake team server whose port also serves the sync
// connection (FakeScServer.h): a save arms the debounce and goes up, the other
// editor hears sc-head through its TeamController and pulls, a file an editor
// holds unsaved edits for is not overwritten (its save is merged on the
// server instead), and the Source Control window's team-sync view
// (TeamSyncView) is drawn over the result. With HE_UI_DUMP_DIR set the
// frames are written as BMPs (scripts/he_uishot.py turns them into PNGs).

namespace fs = std::filesystem;

namespace
{
	constexpr int kW = 620, kH = 760;
	constexpr auto kDeadline = std::chrono::seconds(30);

	std::uint64_t steadyMs()
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	std::string readText(const fs::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		if (!in) return "<none>";
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	void writeText(const fs::path& p, const std::string& text)
	{
		fs::create_directories(p.parent_path());
		std::ofstream out(p, std::ios::binary | std::ios::trunc);
		out << text;
	}

	fs::path scratchDir(const std::string& who)
	{
		static std::atomic<int> n{ 0 };
		fs::path p = fs::temp_directory_path() /
		             ("he-team-sync-ui-" + who + "-" +
		              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
		              std::to_string(n++));
		fs::create_directories(p);
		return p;
	}

	// One editor: the controllers EditorApplication owns, and a project folder
	// in team mode (team "studio", project "game").
	struct Editor
	{
		std::string           name;
		fs::path              root = scratchDir(name);
		HorizonWorld          world;
		CollabController      collab;
		TeamController        team{ collab };
		GitController         git;
		TeamSyncController    sync{ team, git, collab };
		std::set<std::string> unsaved;     // what the "tabs" hold unsaved edits for
		std::vector<std::string> written;  // what came from the server
		std::vector<fs::path> opened;      // folders "shown" by Open My Version
		std::string           projectTeam = "studio";

		explicit Editor(std::string n) : name(std::move(n))
		{
			collab.setWorld(&world);
			sync.setKeepLocal([this](const std::string& p) { return unsaved.count(p) > 0; });
			sync.setOpenFolder([this](const fs::path& dir) { opened.push_back(dir); });
		}
		~Editor()
		{
			sync.close();
			std::error_code ec;
			fs::remove_all(root, ec);
		}
		void update()
		{
			const std::uint64_t now = steadyMs();
			collab.update(now);
			team.update(now);
			TeamSyncController::Project p;
			p.root       = root;
			p.teamServer = true;
			p.team       = projectTeam;
			p.project    = "game";
			for (const std::string& w : sync.update(now, p, /*playing=*/false)) written.push_back(w);
		}
		std::string file(const std::string& rel) const { return readText(root / rel); }
		void put(const std::string& rel, const std::string& text) const { writeText(root / rel, text); }
		bool settled(std::uint64_t rev) const
		{
			const auto s = sync.service().status();
			return sync.running() && s.connected && s.rev == rev &&
			       s.phase == HE::Sc::TeamSyncService::Phase::Idle && s.activity.empty();
		}
	};

	template <typename Fn>
	bool pump(FakeTeamServer& server, std::vector<Editor*> editors, Fn done)
	{
		const auto deadline = std::chrono::steady_clock::now() + kDeadline;
		for (;;)
		{
			server.pump();
			for (Editor* e : editors) e->update();
			if (done()) return true;
			if (std::chrono::steady_clock::now() >= deadline) return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	std::string addressOf(const FakeTeamServer& server)
	{
		return "127.0.0.1:" + std::to_string(server.address().port);
	}

	struct Harness
	{
		Harness()
		{
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(float(kW), float(kH));
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
			HE::Ed::applyHorizonDarkTheme();
		}
		~Harness() { ImGui::DestroyContext(); }

		// The Source Control window, team mode, as the panel draws it. Two
		// frames: the first one lays out (wrapping), the second is the picture.
		he_ui::Image shot(TeamSyncController& sync)
		{
			he_ui::Image img;
			for (int frame = 0; frame < 2; ++frame)
			{
				ImGui::NewFrame();
				ImGui::SetNextWindowPos(ImVec2(0, 0));
				ImGui::SetNextWindowSize(ImVec2(float(kW), float(kH)));
				ImGui::Begin("Source Control", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
				TeamSyncView::draw(sync);
				ImGui::End();
				ImGui::Render();
				img = he_ui::rasterize(ImGui::GetDrawData(), kW, kH);
			}
			return img;
		}
	};

	void dump(const he_ui::Image& img, const char* name)
	{
		if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
			he_ui::writeBmp(img, std::string(dir) + "/" + name);
	}

	// Pixels that are not the window background: the view drew something.
	int inkPixels(const he_ui::Image& img)
	{
		int n = 0;
		for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4)
			if (img.rgba[i] > 120 || img.rgba[i + 1] > 120 || img.rgba[i + 2] > 120) ++n;
		return n;
	}
} // namespace

TEST_CASE("Team sync UI: a save syncs, the other editor pulls, unsaved edits are merged instead of overwritten")
{
	FakeTeamServer server;
	FakeScServer   sc(server);
	Editor anna("Anna"), bob("Bob");
	anna.put("Content/door.txt", "closed\n");

	// Not signed in yet: nothing runs, and the view says why.
	pump(server, { &anna }, [] { return true; });
	CHECK(anna.sync.blocker() == TeamSyncController::Blocker::SignedOut);
	CHECK(anna.sync.stateLine().find("Sign in to team \"studio\"") == 0);
	CHECK(anna.git.hasTeamServerUpload());   // the slot is filled from the start

	REQUIRE(anna.team.signIn(addressOf(server), "studio", "Anna", "correct-horse"));
	REQUIRE(bob.team.signIn(addressOf(server), "studio", "Bob", "correct-horse"));
	REQUIRE(pump(server, { &anna, &bob }, [&] { return anna.settled(0) && bob.settled(0); }));
	CHECK(anna.sync.blocker() == TeamSyncController::Blocker::None);
	CHECK(anna.sync.stateLine() == "1 change not on the team server yet (r0).");

	// A save arms the debounce: nothing goes up before it has passed.
	const std::uint64_t saved = steadyMs();
	anna.sync.noteSaved();
	REQUIRE(pump(server, { &anna, &bob }, [&] { return sc.head("game") == 1; }));
	CHECK(steadyMs() - saved >= TeamSyncController::kDebounceMs - 50);
	// Bob hears sc-head through his TeamController and pulls.
	REQUIRE(pump(server, { &anna, &bob }, [&] {
		return bob.settled(1) && bob.file("Content/door.txt") == "closed\n";
	}));
	CHECK(bob.written == std::vector<std::string>{ "Content/door.txt" });
	REQUIRE(pump(server, { &anna, &bob }, [&] { return anna.settled(1); }));
	CHECK(anna.sync.stateLine() == "In sync with the team at r1.");

	// Bob has unsaved edits of the door in a tab. Anna's save arrives: his file
	// is not overwritten (and not reloaded)...
	bob.unsaved.insert("Content/door.txt");
	anna.put("Content/door.txt", "open\n");
	anna.sync.syncNow();
	REQUIRE(pump(server, { &anna, &bob }, [&] { return anna.settled(2) && bob.settled(2); }));
	CHECK(bob.file("Content/door.txt") == "closed\n");
	CHECK(bob.written.size() == 1);
	// ...and when he saves, the server sees his edit against the version he
	// edited from: a recorded conflict with Anna's kept, not a silent overwrite.
	bob.put("Content/door.txt", "bob's door\n");
	bob.unsaved.clear();
	bob.sync.noteSaved();
	REQUIRE(pump(server, { &anna, &bob }, [&] {
		return bob.settled(3) && anna.settled(3) && anna.file("Content/door.txt") == "bob's door\n";
	}));
	const auto results = bob.sync.service().lastResults();
	REQUIRE(results.size() == 1);
	CHECK(results[0].outcome == "replaced");
	CHECK(results[0].detail.find("against r2 by Anna") == 0);

	// Anna is told; Open My Version fetches hers and shows its folder.
	REQUIRE(pump(server, { &anna, &bob }, [&] { return !anna.sync.service().lost().empty(); }));
	anna.sync.service().fetchLost(anna.sync.service().lost()[0]);
	REQUIRE(pump(server, { &anna, &bob }, [&] { return !anna.opened.empty(); }));
	CHECK(anna.opened[0] == anna.root / "Saved" / "TeamSync" / "Lost" / "r3" / "Content");
	CHECK(readText(anna.opened[0] / "door.txt") == "open\n");

	// The views, for the record.
	Harness h;
	const he_ui::Image annaShot = h.shot(anna.sync);
	const he_ui::Image bobShot  = h.shot(bob.sync);
	dump(annaShot, "team-sync-anna-lost.bmp");
	dump(bobShot, "team-sync-bob-replaced.bmp");
	CHECK(inkPixels(annaShot) > 2000);
	CHECK(inkPixels(bobShot) > 2000);

	// Signed in to another team than the project names: nothing syncs.
	anna.projectTeam = "elsewhere";
	pump(server, { &anna }, [] { return true; });
	CHECK(anna.sync.blocker() == TeamSyncController::Blocker::OtherTeam);
	CHECK_FALSE(anna.sync.running());
	dump(h.shot(anna.sync), "team-sync-other-team.bmp");
}

TEST_CASE("Team sync UI: the project's team name matches the sign-in whatever its case")
{
	FakeTeamServer server;
	FakeScServer   sc(server);
	Editor anna("Anna");
	anna.projectTeam = "STUDIO";
	REQUIRE(anna.team.signIn(addressOf(server), "studio", "Anna", "correct-horse"));
	REQUIRE(pump(server, { &anna }, [&] { return anna.settled(0); }));
	CHECK(anna.sync.blocker() == TeamSyncController::Blocker::None);
	CHECK(anna.sync.running());

	// A different name stays blocked, also when it only differs in length.
	anna.projectTeam = "studios";
	pump(server, { &anna }, [] { return true; });
	CHECK(anna.sync.blocker() == TeamSyncController::Blocker::OtherTeam);
	CHECK_FALSE(anna.sync.running());
}

TEST_CASE("Team sync UI: a first sync that differs asks before anything moves")
{
	FakeTeamServer server;
	FakeScServer   sc(server);
	Editor anna("Anna");
	anna.put("Content/door.txt", "closed\n");
	REQUIRE(anna.team.signIn(addressOf(server), "studio", "Anna", "correct-horse"));
	REQUIRE(pump(server, { &anna }, [&] { return anna.settled(0); }));
	anna.sync.syncNow();
	REQUIRE(pump(server, { &anna }, [&] { return anna.settled(1); }));

	Editor other("Anna");   // a second machine of Anna's, with an old copy
	other.put("Content/door.txt", "very old\n");
	REQUIRE(other.team.signIn(addressOf(server), "studio", "Anna", "correct-horse"));
	REQUIRE(pump(server, { &anna, &other }, [&] {
		return !other.sync.service().status().firstSyncDiffers.empty();
	}));
	CHECK(other.sync.stateLine() == "First sync: 1 file differs from the team's version.");
	Harness h;
	const he_ui::Image shot = h.shot(other.sync);
	dump(shot, "team-sync-first-sync.bmp");
	CHECK(inkPixels(shot) > 1000);
	CHECK(other.file("Content/door.txt") == "very old\n");
}
