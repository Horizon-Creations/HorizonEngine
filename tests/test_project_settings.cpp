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
