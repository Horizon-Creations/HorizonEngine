// The project settings file (Config/ProjectSettings.json): the format's three
// promises — a missing file is the defaults, a damaged file is refused rather
// than reset, and a project that never touched its settings never grows one —
// plus the round trip through a REAL ProjectManager, because "did the file
// actually keep it" is answered by loading the project again, not by reading
// back the struct that was just written.
#include "doctest.h"
#include "TestFsUtil.h"

#include <Project/ProjectSettings.h>
#include <HorizonScene/PhysicsTuning.h>   // kMaxThreads, pinned against the project's
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
	a.physics.threads                  = 6;
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
	CHECK(j["physics"]["threads"] == 6);

	HE::ProjectSettings b;
	b.fromJson(j);
	CHECK(a == b);
	CHECK(b.physics.threads == 6);
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
	CHECK(s.physics.threads == 0);   // a project from before the setting: automatic
	CHECK(s.renderDefaults.useEditorSettings);
}

// Thema 178: the thread count of a physics step. 0 is "pick for me", the number is
// clamped on the way in, and an edit makes the settings non-default.
TEST_CASE("ProjectPhysicsSettings::threads: automatic by default, clamped, and part of equality")
{
	HE::ProjectSettings s;
	CHECK(s.physics.threads == 0);
	CHECK(s.isDefault());

	s.physics.threads = 4;
	CHECK_FALSE(s.isDefault());

	s.physics.threads = -7;
	s.clamp();
	CHECK(s.physics.threads == 0);
	s.physics.threads = 100000;
	s.clamp();
	CHECK(s.physics.threads == HE::ProjectPhysicsSettings::kMaxThreads);

	// A hand-edited file: a string is ignored (default), a huge number is clamped.
	HE::ProjectSettings fromText;
	fromText.fromJson(json::parse(R"({ "physics": { "threads": "many" } })"));
	CHECK(fromText.physics.threads == 0);
	fromText.fromJson(json::parse(R"({ "physics": { "threads": 9999 } })"));
	CHECK(fromText.physics.threads == HE::ProjectPhysicsSettings::kMaxThreads);

	// The editor's setting and the engine's tuning name the same ceiling.
	static_assert(HE::ProjectPhysicsSettings::kMaxThreads == HE::PhysicsTuning::kMaxThreads,
	              "ProjectPhysicsSettings::kMaxThreads and PhysicsTuning::kMaxThreads must agree");
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

// ─── Multiplayer (docs/gameplay-replication-plan.md §8.4) ────────────────────

TEST_CASE("ProjectMultiplayerSettings: default-constructed is what a session already did")
{
	HE::ProjectSettings s;
	CHECK(s.isDefault());
	// One above LanBeacon::kPort (47823), so the announcement and the session
	// sit next to each other and one firewall rule names the pair.
	CHECK(s.multiplayer.defaultPort == 47824);
	CHECK(s.multiplayer.maxPlayers  == 8);
	CHECK(s.multiplayer.timeoutSec  == doctest::Approx(5.0f));
	// The numbers GameReplication::Config has carried since it existed — this
	// page must not change what a session does merely by appearing.
	CHECK(s.multiplayer.tickHz      == doctest::Approx(30.0f));
	CHECK(s.multiplayer.worldExtent == doctest::Approx(4096.0f));
	CHECK(s.multiplayer.interpolationDelaySec == doctest::Approx(0.1f));
	CHECK(s.multiplayer.reconcileSnapDistance == doctest::Approx(2.0f));
	CHECK(s.multiplayer.reconcileSmoothing    == doctest::Approx(12.0f));
	CHECK(s.multiplayer.maxPendingInputs      == 64);
	CHECK(s.multiplayer.discoverLan);
	CHECK(s.multiplayer.rpcPerSecond == 60);
	// No relay: peer-to-peer, as every project hosted before the relay existed.
	CHECK(s.multiplayer.relayAddress.empty());

	s.multiplayer.tickHz = 60.0f;
	CHECK_FALSE(s.isDefault());
}

TEST_CASE("ProjectMultiplayerSettings: toJson writes the plan's shape and fromJson reads it back")
{
	HE::ProjectSettings a;
	a.multiplayer.defaultPort           = 40000;
	a.multiplayer.maxPlayers            = 16;
	a.multiplayer.timeoutSec            = 9.5f;
	a.multiplayer.tickHz                = 60.0f;
	a.multiplayer.worldExtent           = 8192.0f;
	a.multiplayer.interpolationDelaySec = 0.05f;
	a.multiplayer.reconcileSnapDistance = 3.5f;
	a.multiplayer.reconcileSmoothing    = 20.0f;
	a.multiplayer.maxPendingInputs      = 128;
	a.multiplayer.discoverLan           = false;
	a.multiplayer.discoverDirectory     = false;
	a.multiplayer.portMapping           = false;
	a.multiplayer.rpcPerSecond          = 200;
	a.multiplayer.relayAddress          = "relay.example.com:7777";

	json j;
	a.toJson(j);
	const json& m = j["multiplayer"];
	CHECK(m["defaultPort"] == 40000);
	CHECK(m["maxPlayers"]  == 16);
	CHECK(m["timeoutSec"]  == doctest::Approx(9.5));
	CHECK(m["tickHz"]      == doctest::Approx(60.0));
	CHECK(m["worldExtent"] == doctest::Approx(8192.0));
	CHECK(m["prediction"]["interpolationDelaySec"] == doctest::Approx(0.05));
	CHECK(m["prediction"]["reconcileSnapDistance"] == doctest::Approx(3.5));
	CHECK(m["prediction"]["reconcileSmoothing"]    == doctest::Approx(20.0));
	CHECK(m["prediction"]["maxPendingInputs"]      == 128);
	CHECK(m["discovery"]["lan"]         == false);
	CHECK(m["discovery"]["directory"]   == false);
	CHECK(m["discovery"]["portMapping"] == false);
	CHECK(m["rpcPerSecond"] == 200);
	CHECK(m["relayAddress"] == "relay.example.com:7777");

	HE::ProjectSettings b;
	b.fromJson(j);
	CHECK(a == b);
}

TEST_CASE("ProjectMultiplayerSettings: a file from before it existed is the default")
{
	// Exactly what every ProjectSettings.json on disk looks like today: no
	// "multiplayer" key at all. It must read as the defaults and not as zeroes,
	// or an existing project would host at 0 Hz.
	const json j = json::parse(R"({ "version": 1, "physics": { "fixedHz": 30 } })");
	HE::ProjectSettings s;
	s.fromJson(j);
	CHECK(s.physics.fixedHz == 30);
	CHECK(s.multiplayer.defaultPort == 47824);
	CHECK(s.multiplayer.tickHz      == doctest::Approx(30.0f));
	CHECK(s.multiplayer.maxPendingInputs == 64);
	CHECK(s.multiplayer.discoverLan);
	CHECK(s.multiplayer.relayAddress.empty());
}

TEST_CASE("ProjectMultiplayerSettings: clamp corrects a hand-edited file but keeps port 0")
{
	using MP = HE::ProjectMultiplayerSettings;
	HE::ProjectSettings s;
	s.multiplayer.defaultPort           = 999999;
	s.multiplayer.maxPlayers            = 0;
	s.multiplayer.timeoutSec            = -3.0f;
	s.multiplayer.tickHz                = 0.0f;
	s.multiplayer.worldExtent           = -1.0f;
	s.multiplayer.interpolationDelaySec = 99.0f;
	s.multiplayer.maxPendingInputs      = 0;
	s.multiplayer.rpcPerSecond          = 0;
	s.clamp();
	CHECK(s.multiplayer.defaultPort == MP::kMaxPort);
	CHECK(s.multiplayer.maxPlayers  == MP::kMinPlayers);
	CHECK(s.multiplayer.timeoutSec  == doctest::Approx(MP::kMinTimeoutSec));
	CHECK(s.multiplayer.tickHz      == doctest::Approx(MP::kMinTickHz));
	CHECK(s.multiplayer.worldExtent == doctest::Approx(MP::kMinWorldExtent));
	CHECK(s.multiplayer.interpolationDelaySec == doctest::Approx(MP::kMaxInterpolationSec));
	CHECK(s.multiplayer.maxPendingInputs == MP::kMinPendingInputs);
	CHECK(s.multiplayer.rpcPerSecond == MP::kMinRpcPerSecond);

	// 0 is a REAL answer for the port ("let the system pick"), so it survives
	// where every other zero above was corrected.
	HE::ProjectSettings z;
	z.multiplayer.defaultPort = 0;
	z.clamp();
	CHECK(z.multiplayer.defaultPort == 0);
}

TEST_CASE("ProjectMultiplayerSettings: a project keeps its session numbers across a real save and load")
{
	TempRoot tmp("multiplayer");
	HE::ProjectSettings a;
	a.multiplayer.defaultPort = 40001;
	a.multiplayer.tickHz      = 45.0f;
	a.multiplayer.discoverLan = false;
	REQUIRE(HE::saveProjectSettings(tmp.root, a));

	HE::ProjectSettings b;
	REQUIRE(HE::loadProjectSettings(tmp.root, b));
	CHECK(a == b);
	CHECK(b.multiplayer.defaultPort == 40001);
	CHECK(b.multiplayer.tickHz == doctest::Approx(45.0f));
	CHECK_FALSE(b.multiplayer.discoverLan);
	CHECK(readAll(HE::projectSettingsPath(tmp.root)).find("\"multiplayer\"") != std::string::npos);
}

// ── Source control (topic 198 step 9, the team-server design §6.1) ─────────

TEST_CASE("ProjectSourceControlSettings: every project before it is git, and stays fileless")
{
	// No "sourceControl" key: every ProjectSettings.json on disk today. It must
	// read as direct git, or opening an old project would switch its commits off.
	const json j = json::parse(R"({ "version": 1, "physics": { "fixedHz": 30 } })");
	HE::ProjectSettings s;
	s.fromJson(j);
	CHECK(s.sourceControl.mode == HE::ProjectSourceControlSettings::Mode::Git);
	CHECK_FALSE(s.sourceControl.teamServer());
	CHECK(s.sourceControl.team.empty());

	// The default is part of isDefault(): a project that never touched its
	// settings still writes no file.
	CHECK(HE::ProjectSettings{}.isDefault());
	HE::ProjectSettings t;
	t.sourceControl.mode = HE::ProjectSourceControlSettings::Mode::TeamServer;
	CHECK_FALSE(t.isDefault());
}

TEST_CASE("ProjectSourceControlSettings: toJson writes the design's shape and fromJson reads it back")
{
	HE::ProjectSettings a;
	a.sourceControl.mode    = HE::ProjectSourceControlSettings::Mode::TeamServer;
	a.sourceControl.team    = "Harbour Crew";
	a.sourceControl.project = "sky-harbour";
	json j;
	a.toJson(j);
	CHECK(j["sourceControl"]["mode"] == "teamServer");
	CHECK(j["sourceControl"]["team"] == "Harbour Crew");
	CHECK(j["sourceControl"]["project"] == "sky-harbour");

	HE::ProjectSettings b;
	b.fromJson(j);
	CHECK(a == b);
	CHECK(b.sourceControl.teamServer());

	// The default writes "git" explicitly, like every other value in the file.
	json d;
	HE::ProjectSettings{}.toJson(d);
	CHECK(d["sourceControl"]["mode"] == "git");

	// A different team or project is a different setting.
	HE::ProjectSettings c = a;
	c.sourceControl.project = "other";
	CHECK(a != c);
}

TEST_CASE("ProjectSourceControlSettings: an unknown mode is git, not the team server")
{
	// A newer editor's mode, or a typo, must never be what turns direct git off.
	const json j = json::parse(R"({ "version": 1,
		"sourceControl": { "mode": "TeamServer", "team": "t", "project": "p" } })");
	HE::ProjectSettings s;
	s.fromJson(j);
	CHECK(s.sourceControl.mode == HE::ProjectSourceControlSettings::Mode::Git);
	CHECK(s.sourceControl.team == "t");

	const json w = json::parse(R"({ "version": 1, "sourceControl": { "mode": 3 } })");
	HE::ProjectSettings n;
	n.fromJson(w);
	CHECK_FALSE(n.sourceControl.teamServer());
}

TEST_CASE("ProjectSourceControlSettings: the team-server choice survives a real save and load")
{
	TempRoot tmp("sourcecontrol");
	HE::ProjectSettings a;
	a.sourceControl.mode    = HE::ProjectSourceControlSettings::Mode::TeamServer;
	a.sourceControl.team    = "crew";
	a.sourceControl.project = "game";
	REQUIRE(HE::saveProjectSettings(tmp.root, a));

	HE::ProjectSettings b;
	REQUIRE(HE::loadProjectSettings(tmp.root, b));
	CHECK(a == b);
	CHECK(readAll(HE::projectSettingsPath(tmp.root)).find("\"teamServer\"") != std::string::npos);
}

// ── Graphics and Input ───────────────────────────────────────────────────────
// The graphics half of a build's config.json used to be the exporting editor's
// own preferences; the project can now own it. Off by default — which IS the old
// behaviour — and every number held to the range the editor's page enforces.

TEST_CASE("ProjectGraphics: default-constructed is off and equals today's editor defaults")
{
	const HE::ProjectSettings s;
	CHECK_FALSE(s.graphics.enabled);
	CHECK_FALSE(s.input.enabled);
	CHECK(s.isDefault());
	// A few of the values the editor's config has always started with.
	CHECK(s.graphics.bloomEnabled);
	CHECK(s.graphics.bloomIntensity == doctest::Approx(0.6f));
	CHECK(s.graphics.antiAliasing == 1);
	CHECK_FALSE(s.graphics.giEnabled);
	CHECK(s.input.stickDeadzone == doctest::Approx(0.15f));
	CHECK(s.input.triggerDeadzone == doctest::Approx(0.05f));
}

TEST_CASE("ProjectGraphics: toJson/fromJson round-trips every field, and a change breaks equality")
{
	HE::ProjectSettings a;
	a.graphics.enabled              = true;
	a.graphics.renderPath           = 1;
	a.graphics.antiAliasing         = 3;
	a.graphics.renderScale          = 0.75f;
	a.graphics.bloomEnabled         = false;
	a.graphics.bloomThreshold       = 2.5f;
	a.graphics.ssaoMethod           = 2;
	a.graphics.dofEnabled           = true;
	a.graphics.dofAperture          = 4.0f;
	a.graphics.motionBlurMax        = 48.0f;
	a.graphics.ssrEnabled           = true;
	a.graphics.ssrQuality           = 2;
	a.graphics.giEnabled            = true;
	a.graphics.giReflBounces        = 3;
	a.graphics.giReflBlur           = true;
	a.graphics.occlusionCulling     = true;
	a.graphics.gpuParticles         = false;
	a.input.enabled                 = true;
	a.input.stickDeadzone           = 0.3f;
	a.input.triggerDeadzone         = 0.2f;
	CHECK_FALSE(a.isDefault());
	CHECK(a != HE::ProjectSettings{});

	nlohmann::json j;
	a.toJson(j);
	REQUIRE(j.contains("graphics"));
	CHECK(j["graphics"]["bloomThreshold"].get<float>() == doctest::Approx(2.5f));
	CHECK(j["graphics"]["giEnabled"].get<bool>());
	CHECK(j["input"]["stickDeadzone"].get<float>() == doctest::Approx(0.3f));

	HE::ProjectSettings b;
	b.fromJson(j);
	CHECK(a == b);

	// One field apart is one field unequal.
	b.graphics.dofFocusRange += 1.0f;
	CHECK(a != b);
}

TEST_CASE("ProjectGraphics: a file from before it existed is the default, and a partial block fills the rest")
{
	HE::ProjectSettings s;
	s.fromJson(nlohmann::json{ { "version", 1 }, { "game", { { "title", "Old" } } } });
	CHECK_FALSE(s.graphics.enabled);
	CHECK(s.isDefault() == false);   // the title

	s.fromJson(nlohmann::json{ { "graphics", { { "enabled", true }, { "bloomEnabled", false } } } });
	CHECK(s.graphics.enabled);
	CHECK_FALSE(s.graphics.bloomEnabled);
	CHECK(s.graphics.ssaoEnabled);                                     // not in the file: default
	CHECK(s.graphics.renderScale == doctest::Approx(1.0f));
}

TEST_CASE("ProjectGraphics: wrong types and out-of-range numbers are corrected, never trusted")
{
	HE::ProjectSettings s;
	s.fromJson(nlohmann::json{ { "graphics", {
		{ "enabled", true },
		{ "renderScale", 99.0 },         // above 2
		{ "bloomIntensity", -4.0 },      // below 0
		{ "ssaoMethod", 17 },            // beyond GTAO
		{ "giReflBounces", 0 },          // below 1
		{ "dofAperture", "wide" },       // not a number: the default stays
		{ "bloomEnabled", "yes" },       // not a bool: the default stays
	} }, { "input", { { "enabled", true }, { "stickDeadzone", 5.0 }, { "triggerDeadzone", -1.0 } } } });
	CHECK(s.graphics.renderScale == doctest::Approx(2.0f));
	CHECK(s.graphics.bloomIntensity == doctest::Approx(0.0f));
	CHECK(s.graphics.ssaoMethod == 2);
	CHECK(s.graphics.giReflBounces == 1);
	CHECK(s.graphics.dofAperture == doctest::Approx(2.8f));
	CHECK(s.graphics.bloomEnabled);
	CHECK(s.input.stickDeadzone == doctest::Approx(HE::ProjectInputSettings::kMaxDeadzone));
	CHECK(s.input.triggerDeadzone == doctest::Approx(0.0f));

	// And clamp() on a struct set directly holds a NaN too.
	s.graphics.renderScale = std::nanf("");
	s.clamp();
	CHECK(s.graphics.renderScale == doctest::Approx(1.0f));
}

// ── operator== sees every field ──────────────────────────────────────────────
// operator== is a hand-kept field list, and it drives isDefault() and with it
// the "never grew a file" promise. A member added to a struct and forgotten
// there would be saved nowhere and compared as equal, with no error: this case
// flips each member of a default ProjectSettings on its own and wants the
// comparison to notice. A new member needs a row here as well.

namespace
{

// Far enough from any default that nearlyEqual (relative 1e-6) cannot hide it.
void bump(bool& v)         { v = !v; }
void bump(int& v)          { v += 1; }
void bump(float& v)        { v += 1.0f; }
void bump(std::string& v)  { v += "x"; }

template <class Mutate>
void expectFieldSeen(const char* field, Mutate mutate)
{
	INFO("field: " << field);
	const HE::ProjectSettings base;
	HE::ProjectSettings changed;
	mutate(changed);
	CHECK_FALSE(base == changed);
	CHECK(base != changed);
	CHECK_FALSE(changed.isDefault());
}

} // namespace

TEST_CASE("ProjectSettings: operator== and isDefault see every field")
{
	using S = HE::ProjectSettings;
#define HE_FIELD(path) expectFieldSeen(#path, [](S& s) { bump(s.path); })

	HE_FIELD(game.title);
	HE_FIELD(game.splashEnabled);
	HE_FIELD(game.splashImage);
	HE_FIELD(game.splashSubtitle);

	HE_FIELD(shadows.distance);
	HE_FIELD(shadows.cascadeCount);
	HE_FIELD(shadows.resolution);
	HE_FIELD(shadows.splitLambda);
	HE_FIELD(shadows.slopeBias);
	HE_FIELD(shadows.minBias);

	HE_FIELD(physics.fixedHz);
	HE_FIELD(physics.threads);
	HE_FIELD(physics.budgetMs);
	HE_FIELD(physics.eventsPerPoll);
	HE_FIELD(physics.gravity.x);
	HE_FIELD(physics.gravity.y);
	HE_FIELD(physics.gravity.z);
	HE_FIELD(physics.floatingOriginRadius);

	HE_FIELD(renderDefaults.useEditorSettings);
	HE_FIELD(renderDefaults.windowWidth);
	HE_FIELD(renderDefaults.windowHeight);
	HE_FIELD(renderDefaults.windowMode);
	HE_FIELD(renderDefaults.vsync);
	HE_FIELD(renderDefaults.backend);

	HE_FIELD(graphics.enabled);
#define HE_X(T, name, def, editorName, lo, hi) HE_FIELD(graphics.name);
	HE_PROJECT_GRAPHICS_FIELDS(HE_X)
#undef HE_X

	HE_FIELD(input.enabled);
	HE_FIELD(input.stickDeadzone);
	HE_FIELD(input.triggerDeadzone);

	HE_FIELD(antiCheat.enabled);
	HE_FIELD(antiCheat.integrityCheck);
	HE_FIELD(antiCheat.tolerance);
	HE_FIELD(antiCheat.windowSec);
	HE_FIELD(antiCheat.maxInputsPerSecond);
	HE_FIELD(antiCheat.scoreHalfLifeSec);
	HE_FIELD(antiCheat.scoreSuspect);
	HE_FIELD(antiCheat.scoreConfirmed);
	HE_FIELD(antiCheat.telemetryUrl);

	HE_FIELD(multiplayer.defaultPort);
	HE_FIELD(multiplayer.maxPlayers);
	HE_FIELD(multiplayer.timeoutSec);
	HE_FIELD(multiplayer.tickHz);
	HE_FIELD(multiplayer.worldExtent);
	HE_FIELD(multiplayer.interpolationDelaySec);
	HE_FIELD(multiplayer.reconcileSnapDistance);
	HE_FIELD(multiplayer.reconcileSmoothing);
	HE_FIELD(multiplayer.maxPendingInputs);
	HE_FIELD(multiplayer.discoverLan);
	HE_FIELD(multiplayer.discoverDirectory);
	HE_FIELD(multiplayer.portMapping);
	HE_FIELD(multiplayer.relayAddress);
	HE_FIELD(multiplayer.rpcPerSecond);

	HE_FIELD(sourceControl.team);
	HE_FIELD(sourceControl.project);
#undef HE_FIELD

	// The members a plain bump() does not fit.
	expectFieldSeen("antiCheat.policySuspect",   [](S& s) { s.antiCheat.policySuspect   ^= HE::ProjectAntiCheatSettings::Flag; });
	expectFieldSeen("antiCheat.policyConfirmed", [](S& s) { s.antiCheat.policyConfirmed ^= HE::ProjectAntiCheatSettings::Flag; });
	expectFieldSeen("antiCheat.policyHard",      [](S& s) { s.antiCheat.policyHard      ^= HE::ProjectAntiCheatSettings::Flag; });
	expectFieldSeen("sourceControl.mode", [](S& s) { s.sourceControl.mode = HE::ProjectSourceControlSettings::Mode::TeamServer; });

	// A rule is part of equality: its presence, and each of its members.
	expectFieldSeen("antiCheat.rules (added)", [](S& s) { s.antiCheat.rules.push_back({}); });
	// Each member of a rule is compared against the same rule, not against a
	// default ProjectSettings, which has no rule to differ from.
	HE::ProjectSettings withRule, other;
	HE::ProjectAntiCheatRule rule;
	rule.name = "speed";
	withRule.antiCheat.rules.push_back(rule);
	other.antiCheat.rules.push_back(rule);
	CHECK(withRule == other);
	other.antiCheat.rules[0].name = "speedy";
	CHECK(withRule != other);
	other.antiCheat.rules[0] = rule;
	other.antiCheat.rules[0].level = "hard";
	CHECK(withRule != other);
	other.antiCheat.rules[0] = rule;
	other.antiCheat.rules[0].min += 1.0f;
	CHECK(withRule != other);
	other.antiCheat.rules[0] = rule;
	other.antiCheat.rules[0].max += 1.0f;
	CHECK(withRule != other);
	other.antiCheat.rules[0] = rule;
	other.antiCheat.rules[0].maxPerSecond += 1.0f;
	CHECK(withRule != other);
}
