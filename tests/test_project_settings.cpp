// The project settings file (Config/ProjectSettings.json): the format's three
// promises — a missing file is the defaults, a damaged file is refused rather
// than reset, and a project that never touched its settings never grows one —
// plus the round trip through a REAL ProjectManager, because "did the file
// actually keep it" is answered by loading the project again, not by reading
// back the struct that was just written.
#include "doctest.h"
#include "TestFsUtil.h"

#include <Project/ProjectSettings.h>
#include "ProjectManager.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace
{

struct TempRoot
{
	fs::path root;
	explicit TempRoot(const char* name)
	{
		root = fs::temp_directory_path() /
		       ("he_test_project_settings_" + std::string(name) + "_" + std::to_string(::rand()));
		fs::create_directories(root);
	}
	~TempRoot() { he_test::removeAllQuiet(root); }
};

std::string readAll(const fs::path& p)
{
	std::ifstream in(p);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

// ─── Defaults ────────────────────────────────────────────────────────────────

TEST_CASE("ProjectSettings: default construction is today's behaviour, and says so")
{
	HE::ProjectSettings s;
	CHECK(s.isDefault());
	// The numbers RenderExtractor / PhysicsWorld have always used.
	CHECK(s.shadows.distance == doctest::Approx(250.0f));
	CHECK(s.shadows.cascadeCount == 3);
	CHECK(s.shadows.resolution == 2048);
	CHECK(s.physics.fixedHz == 60);
	CHECK(s.physics.gravity.y == doctest::Approx(-9.81f));
	// The packaged build keeps taking the editor's Preferences until a project
	// says otherwise.
	CHECK(s.renderDefaults.useEditorSettings);
	CHECK(s.game.title.empty());

	s.physics.fixedHz = 120;
	CHECK_FALSE(s.isDefault());
}

// The step both applications hand to advanceFixedSteps: the default IS the old
// constant, a tuned rate is its reciprocal, and a hand-edited nonsense value is
// clamped rather than becoming a zero or infinite step.
TEST_CASE("ProjectPhysicsSettings::fixedDt is the rate's reciprocal, clamped")
{
	HE::ProjectPhysicsSettings ph;
	CHECK(ph.fixedDt() == doctest::Approx(1.0f / 60.0f));
	ph.fixedHz = 120;
	CHECK(ph.fixedDt() == doctest::Approx(1.0f / 120.0f));
	ph.fixedHz = 0;
	CHECK(ph.fixedDt() == doctest::Approx(1.0f / HE::ProjectPhysicsSettings::kMinHz));
	ph.fixedHz = 100000;
	CHECK(ph.fixedDt() == doctest::Approx(1.0f / HE::ProjectPhysicsSettings::kMaxHz));
}

// ─── JSON ────────────────────────────────────────────────────────────────────

TEST_CASE("ProjectSettings: toJson/fromJson round-trips every field")
{
	HE::ProjectSettings a;
	a.game.title                       = "Sky Harbour";
	a.shadows.distance                 = 400.0f;
	a.shadows.cascadeCount             = 2;
	a.shadows.resolution               = 4096;
	a.shadows.splitLambda              = 0.7f;
	a.shadows.slopeBias                = 0.002f;
	a.shadows.minBias                  = 0.0006f;
	a.physics.fixedHz                  = 120;
	a.physics.gravity                  = glm::vec3(0.5f, -3.7f, 0.0f);
	a.renderDefaults.useEditorSettings = false;
	a.renderDefaults.windowWidth       = 1920;
	a.renderDefaults.windowHeight      = 1080;
	a.renderDefaults.windowMode        = "Borderless";
	a.renderDefaults.vsync             = false;
	a.renderDefaults.backend           = "OpenGL";

	json j;
	a.toJson(j);
	CHECK(j["version"] == HE::ProjectSettings::kVersion);
	CHECK(j["shadows"]["cascadeCount"] == 2);
	CHECK(j["physics"]["gravity"].size() == 3);

	HE::ProjectSettings b;
	b.fromJson(j);
	CHECK(a == b);
	CHECK(b.game.title == "Sky Harbour");
	CHECK(b.renderDefaults.windowMode == "Borderless");
	CHECK(b.physics.gravity.x == doctest::Approx(0.5f));
}

TEST_CASE("ProjectSettings: a missing key is its default, an unknown key is ignored")
{
	const json j = json::parse(R"({
		"version": 1,
		"shadows": { "distance": 120 },
		"physics": { "somethingFromTheFuture": true },
		"newSection": { "x": 1 }
	})");
	HE::ProjectSettings s;
	s.physics.fixedHz = 30;   // fromJson starts from a fresh default, not from this
	s.fromJson(j);
	CHECK(s.shadows.distance == doctest::Approx(120.0f));
	CHECK(s.shadows.cascadeCount == 3);
	CHECK(s.physics.fixedHz == 60);
	CHECK(s.renderDefaults.useEditorSettings);
}

TEST_CASE("ProjectSettings: wrong types and out-of-range numbers are corrected, never trusted")
{
	const json j = json::parse(R"({
		"game": { "title": 42 },
		"shadows": { "distance": "far", "cascadeCount": 9, "resolution": 3500, "splitLambda": 5 },
		"physics": { "fixedHz": 1, "gravity": [0, "down", 0] },
		"renderDefaults": { "windowWidth": 10, "windowMode": "Sideways" }
	})");
	HE::ProjectSettings s;
	s.fromJson(j);
	CHECK(s.game.title.empty());
	CHECK(s.shadows.distance == doctest::Approx(250.0f));
	CHECK(s.shadows.cascadeCount == HE::ProjectShadowSettings::kMaxCascades);
	CHECK(s.shadows.resolution == 4096);            // nearest power of two
	CHECK(s.shadows.splitLambda == doctest::Approx(1.0f));
	CHECK(s.physics.fixedHz == HE::ProjectPhysicsSettings::kMinHz);
	CHECK(s.physics.gravity.y == doctest::Approx(-9.81f));   // a half-valid array is dropped whole
	CHECK(s.renderDefaults.windowWidth == HE::ProjectRenderDefaults::kMinWindowEdge);
	CHECK(s.renderDefaults.windowMode == "Fullscreen");
}

TEST_CASE("ProjectSettings: fromJson on a non-object is a full reset")
{
	HE::ProjectSettings s;
	s.physics.fixedHz = 90;
	s.fromJson(json::array());
	CHECK(s.isDefault());
}

// ─── The file ────────────────────────────────────────────────────────────────

TEST_CASE("ProjectSettings file: absent = defaults, default = no file written")
{
	TempRoot t("absent");
	HE::ProjectSettings s;
	s.physics.fixedHz = 90;
	CHECK(HE::loadProjectSettings(t.root, s));
	CHECK(s.isDefault());                  // a missing file resets to the defaults

	CHECK(HE::saveProjectSettings(t.root, HE::ProjectSettings{}));
	CHECK_FALSE(fs::exists(HE::projectSettingsPath(t.root)));   // …and writes nothing
}

TEST_CASE("ProjectSettings file: written once changed, rewritten even when back to default")
{
	TempRoot t("roundtrip");
	HE::ProjectSettings s;
	s.shadows.cascadeCount = 2;
	s.game.title           = "Title";
	REQUIRE(HE::saveProjectSettings(t.root, s));
	const fs::path file = HE::projectSettingsPath(t.root);
	REQUIRE(fs::exists(file));
	CHECK(file.parent_path().filename() == "Config");
	CHECK_FALSE(fs::exists(file.string() + ".tmp"));

	HE::ProjectSettings back;
	REQUIRE(HE::loadProjectSettings(t.root, back));
	CHECK(back == s);

	// Once the file exists, "back to default" is a change and the file says so.
	REQUIRE(HE::saveProjectSettings(t.root, HE::ProjectSettings{}));
	CHECK(fs::exists(file));
	const json j = json::parse(readAll(file));
	CHECK(j["shadows"]["cascadeCount"] == 3);
	CHECK(j["game"]["title"] == "");
}

TEST_CASE("ProjectSettings file: a damaged file is refused and left untouched")
{
	TempRoot t("damaged");
	const fs::path file = HE::projectSettingsPath(t.root);
	fs::create_directories(file.parent_path());
	{
		std::ofstream out(file);
		out << "{ this is not json";
	}
	HE::ProjectSettings s;
	s.physics.fixedHz = 90;
	CHECK_FALSE(HE::loadProjectSettings(t.root, s));
	CHECK(s.physics.fixedHz == 90);                 // `out` untouched on failure
	CHECK(readAll(file) == "{ this is not json");   // and so is the file
}

// ─── Through the ProjectManager ──────────────────────────────────────────────

TEST_CASE("ProjectManager: settings load beside the .heproj and are saved by saveProjectSettings only")
{
	TempRoot t("manager");
	ProjectManager pm;
	REQUIRE(pm.createNewProject((t.root / "Demo").string(), "Demo"));
	const std::string heproj = pm.currentProject().path;
	const std::string root   = pm.projectRoot();
	CHECK(fs::path(root).filename() == "Demo");

	// A fresh project has no settings file, and saving the manifest does not
	// conjure one.
	CHECK(pm.currentProject().settings.isDefault());
	REQUIRE(pm.saveProject(heproj));
	CHECK_FALSE(fs::exists(HE::projectSettingsPath(root)));

	pm.currentProject().settings.physics.gravity = glm::vec3(0.0f, -1.62f, 0.0f);
	pm.currentProject().settings.renderDefaults.useEditorSettings = false;
	pm.currentProject().settings.renderDefaults.windowMode = "Windowed";
	REQUIRE(pm.saveProjectSettings());
	CHECK(fs::exists(HE::projectSettingsPath(root)));

	// Load the project again: the values come back from the FILE.
	ProjectManager again;
	REQUIRE(again.loadProject(heproj));
	CHECK(again.currentProject().settings.physics.gravity.y == doctest::Approx(-1.62f));
	CHECK_FALSE(again.currentProject().settings.renderDefaults.useEditorSettings);
	CHECK(again.currentProject().settings.renderDefaults.windowMode == "Windowed");

	// Closing forgets them; the next project starts from its own file (none).
	again.closeProject();
	CHECK(again.currentProject().settings.isDefault());
	CHECK_FALSE(again.saveProjectSettings());   // no project open
}

TEST_CASE("ProjectSettings: the splash rides in the game section and reads back")
{
	HE::ProjectSettings a;
	CHECK_FALSE(a.game.splashEnabled);          // off, so no shipped game grows one unasked
	a.game.splashEnabled  = true;
	a.game.splashImage    = "Content/Splash.png";
	a.game.splashSubtitle = "Version 0.4";
	CHECK_FALSE(a.isDefault());

	json j;
	a.toJson(j);
	CHECK(j["game"]["splashEnabled"] == true);
	CHECK(j["game"]["splashImage"] == "Content/Splash.png");

	HE::ProjectSettings b;
	b.fromJson(j);
	CHECK(a == b);
	CHECK(b.game.splashEnabled);
	CHECK(b.game.splashSubtitle == "Version 0.4");

	// A file written before the splash existed: absent keys are the defaults.
	HE::ProjectSettings c;
	c.fromJson(json::parse(R"({"version":1,"game":{"title":"Old"}})"));
	CHECK(c.game.title == "Old");
	CHECK_FALSE(c.game.splashEnabled);
	CHECK(c.game.splashImage.empty());
}

// ─── Anti-cheat (docs/anti-cheat-plan.md §4.4) ───────────────────────────────

TEST_CASE("ProjectAntiCheatSettings: default-constructed is off, with the plan's policy")
{
	using AC = HE::ProjectAntiCheatSettings;
	HE::ProjectSettings s;
	CHECK(s.isDefault());
	CHECK_FALSE(s.antiCheat.enabled);              // the host as it always was
	CHECK(s.antiCheat.integrityCheck);             // what ticking `enabled` should give
	CHECK(s.antiCheat.tolerance == doctest::Approx(0.15f));
	CHECK(s.antiCheat.windowSec == doctest::Approx(3.0f));
	CHECK(s.antiCheat.maxInputsPerSecond == 240);
	CHECK(s.antiCheat.scoreHalfLifeSec == doctest::Approx(30.0f));
	CHECK(s.antiCheat.scoreSuspect == doctest::Approx(5.0f));
	CHECK(s.antiCheat.scoreConfirmed == doctest::Approx(20.0f));
	// No kick on the score levels, a kick on Hard (§5.3).
	CHECK(s.antiCheat.policySuspect   == (AC::Log | AC::Event | AC::Telemetry));
	CHECK(s.antiCheat.policyConfirmed == (AC::Log | AC::Event | AC::Telemetry));
	CHECK(s.antiCheat.policyHard      == (AC::Log | AC::Event | AC::Telemetry | AC::Kick));
	CHECK(s.antiCheat.telemetryUrl.empty());
	CHECK(s.antiCheat.rules.empty());

	s.antiCheat.enabled = true;
	CHECK_FALSE(s.isDefault());
}

TEST_CASE("ProjectAntiCheatSettings: toJson writes the plan's shape and fromJson reads it back")
{
	using AC = HE::ProjectAntiCheatSettings;
	HE::ProjectSettings a;
	a.antiCheat.enabled            = true;
	a.antiCheat.integrityCheck     = false;
	a.antiCheat.tolerance          = 0.25f;
	a.antiCheat.windowSec          = 5.0f;
	a.antiCheat.maxInputsPerSecond = 120;
	a.antiCheat.scoreHalfLifeSec   = 45.0f;
	a.antiCheat.scoreSuspect       = 8.0f;
	a.antiCheat.scoreConfirmed     = 30.0f;
	a.antiCheat.policySuspect      = AC::Log;                         // observation mode
	a.antiCheat.policyConfirmed    = AC::Log | AC::Event | AC::Flag;
	a.antiCheat.policyHard         = AC::Log | AC::Event | AC::Telemetry | AC::Kick | AC::Ban;
	a.antiCheat.telemetryUrl       = "https://example.test/anticheat";
	a.antiCheat.rules = {
		{ "Damage", 0.0f, 100.0f, 300.0f, "suspect" },
		{ "Pickup", 1.0f, 1.0f,   5.0f,   "hard" },
	};

	json j;
	a.toJson(j);
	const json& ac = j["anticheat"];
	// Key for key what §4.4 shows.
	CHECK(ac["enabled"] == true);
	CHECK(ac["integrityCheck"] == false);
	CHECK(ac["tolerance"] == doctest::Approx(0.25));
	CHECK(ac["windowSec"] == doctest::Approx(5.0));
	CHECK(ac["maxInputsPerSecond"] == 120);
	CHECK(ac["score"]["halfLifeSec"] == doctest::Approx(45.0));
	CHECK(ac["score"]["suspect"] == doctest::Approx(8.0));
	CHECK(ac["score"]["confirmed"] == doctest::Approx(30.0));
	CHECK(ac["policy"]["suspect"]   == json::array({ "log" }));
	CHECK(ac["policy"]["confirmed"] == json::array({ "log", "event", "flag" }));
	CHECK(ac["policy"]["hard"]      == json::array({ "log", "event", "telemetry", "kick", "ban" }));
	CHECK(ac["telemetryUrl"] == "https://example.test/anticheat");
	REQUIRE(ac["rules"].size() == 2);
	CHECK(ac["rules"][0]["name"] == "Damage");
	CHECK(ac["rules"][0]["max"] == doctest::Approx(100.0));
	CHECK(ac["rules"][0]["maxPerSecond"] == doctest::Approx(300.0));
	CHECK(ac["rules"][1]["level"] == "hard");

	HE::ProjectSettings b;
	b.fromJson(j);
	CHECK(a == b);
	CHECK(b.antiCheat.policySuspect == AC::Log);
	CHECK(b.antiCheat.policyHard == (AC::Log | AC::Event | AC::Telemetry | AC::Kick | AC::Ban));
	REQUIRE(b.antiCheat.rules.size() == 2);
	CHECK(b.antiCheat.rules[1].name == "Pickup");
	CHECK(b.antiCheat.rules[1].levelIndex() == 2);

	// Two saves of the same settings are the same text — a policy is written
	// in bit order, not in whatever order the file happened to list it.
	json j2;
	b.toJson(j2);
	CHECK(j.dump() == j2.dump());
}

TEST_CASE("ProjectAntiCheatSettings: a file from before it existed is the default, and unknown keys are ignored")
{
	HE::ProjectSettings s;
	s.fromJson(json::parse(R"({"version":1,"physics":{"fixedHz":90}})"));
	CHECK(s.physics.fixedHz == 90);
	CHECK_FALSE(s.antiCheat.enabled);
	CHECK(s.antiCheat.rules.empty());
	CHECK(s.antiCheat.policyHard == HE::ProjectSettings{}.antiCheat.policyHard);

	// Keys from a newer engine, a policy naming a response this one does not
	// know, a rule with an extra field: all read past, nothing thrown.
	s.fromJson(json::parse(R"({
		"anticheat": {
			"enabled": true,
			"futureKnob": 12,
			"policy": { "hard": ["log", "quarantine", "kick"], "purgatory": ["kick"] },
			"rules": [ { "name": "Loot", "max": 3, "weight": 9 }, "not a rule", 42 ]
		}
	})"));
	using AC = HE::ProjectAntiCheatSettings;
	CHECK(s.antiCheat.enabled);
	CHECK(s.antiCheat.policyHard == (AC::Log | AC::Kick));
	// The unknown level keeps its default: only "hard" was in the file.
	CHECK(s.antiCheat.policySuspect == AC{}.policySuspect);
	REQUIRE(s.antiCheat.rules.size() == 1);           // the two non-objects are skipped
	CHECK(s.antiCheat.rules[0].name == "Loot");
	CHECK(s.antiCheat.rules[0].min == doctest::Approx(0.0f));
	CHECK(s.antiCheat.rules[0].max == doctest::Approx(3.0f));
	CHECK(s.antiCheat.rules[0].level == "suspect");
}

TEST_CASE("ProjectAntiCheatSettings: clamp holds the thresholds, keeps Log, and does not eat a nameless rule")
{
	using AC = HE::ProjectAntiCheatSettings;
	HE::ProjectSettings s;
	AC& a = s.antiCheat;

	// Out of range in both directions, plus a NaN a hand edit could not even
	// write but a struct can hold.
	a.tolerance          = -1.0f;
	a.windowSec          = 1000.0f;
	a.maxInputsPerSecond = 0;
	a.scoreHalfLifeSec   = std::nanf("");
	a.scoreSuspect       = 50.0f;
	a.scoreConfirmed     = 10.0f;          // below Suspect: lifted, not the other way round
	a.policySuspect      = 0;              // "[]" — log only, and Log is put back
	a.policyConfirmed    = 0xFFFFFFFFu;    // bits the engine does not have
	a.policyHard         = AC::Kick;       // a policy written without "log"
	a.rules = {
		{ "",       5.0f,  2.0f,  -3.0f, "suspect" },   // nameless, max < min, negative rate
		{ "Coins",  0.0f,  1.0f,   0.0f, "severe"  },   // a level the engine does not spell
	};
	s.clamp();

	CHECK(a.tolerance == doctest::Approx(0.0f));
	CHECK(a.windowSec == doctest::Approx(AC::kMaxWindowSec));
	CHECK(a.maxInputsPerSecond == AC::kMinInputsPerSecond);
	CHECK(a.scoreHalfLifeSec == doctest::Approx(AC{}.scoreHalfLifeSec));
	CHECK(a.scoreSuspect == doctest::Approx(50.0f));
	CHECK(a.scoreConfirmed == doctest::Approx(50.0f));
	CHECK(a.policySuspect == AC::Log);
	CHECK(a.policyConfirmed == AC::AllResponses);
	CHECK(a.policyHard == (AC::Log | AC::Kick));
	REQUIRE(a.rules.size() == 2);           // the empty name stays: the page adds rows before naming them
	CHECK(a.rules[0].name.empty());
	CHECK(a.rules[0].min == doctest::Approx(5.0f));
	CHECK(a.rules[0].max == doctest::Approx(5.0f));
	CHECK(a.rules[0].maxPerSecond == doctest::Approx(0.0f));
	CHECK(a.rules[1].level == "suspect");

	// The same through the file: fromJson clamps too, so a file that says
	// confirmed < suspect reads back consistent.
	json j;
	s.toJson(j);
	j["anticheat"]["score"]["confirmed"] = 1;
	j["anticheat"]["policy"]["suspect"]  = json::array();
	HE::ProjectSettings t;
	t.fromJson(j);
	CHECK(t.antiCheat.scoreConfirmed == doctest::Approx(50.0f));
	CHECK(t.antiCheat.policySuspect == AC::Log);

	// More rules than the ceiling are cut, not refused.
	a.rules.assign(static_cast<std::size_t>(AC::kMaxRules) + 10, HE::ProjectAntiCheatRule{ "R", 0, 1, 0, "suspect" });
	s.clamp();
	CHECK(a.rules.size() == static_cast<std::size_t>(AC::kMaxRules));
}

TEST_CASE("ProjectAntiCheatSettings: a project keeps its rules across a real save and load")
{
	TempRoot tmp("anticheat");
	HE::ProjectSettings a;
	a.antiCheat.enabled = true;
	a.antiCheat.rules   = { { "Damage", 0.0f, 100.0f, 300.0f, "suspect" } };
	REQUIRE(HE::saveProjectSettings(tmp.root, a));

	HE::ProjectSettings b;
	REQUIRE(HE::loadProjectSettings(tmp.root, b));
	CHECK(a == b);
	CHECK(b.antiCheat.enabled);
	REQUIRE(b.antiCheat.rules.size() == 1);
	CHECK(b.antiCheat.rules[0].name == "Damage");
	// And it is in the text a reviewer would read, under the plan's key.
	CHECK(readAll(HE::projectSettingsPath(tmp.root)).find("\"anticheat\"") != std::string::npos);
}
