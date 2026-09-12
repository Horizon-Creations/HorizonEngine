#include "doctest.h"

#include "EditorConfig.h"
#include "EditorSettingsCatalog.h"
#include "McpToolRegistry.h"
#include "ProjectManager.h"
#include "TestFsUtil.h"

#include <Physics/CollisionLayers.h>
#include <Renderer/UIFont.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ─── Reading and writing the knobs from outside the editor ───────────────────
// Two claims, and the first one is the only one that can lie convincingly:
//
//   1. A PROJECT WRITE OUTLIVES THE SESSION. The tool sets a field on a struct
//      the editor holds in memory; whether that reaches the .heproj manifest is
//      a separate act, and a test that read the struct back would pass either
//      way. So the project tests run against a REAL ProjectManager over a
//      temporary directory and check by LOADING THE PROJECT AGAIN through a
//      second manager. That is the only question worth asking here.
//
//   2. AN EDITOR WRITE LANDS IN THE FIELD IT NAMES. Sixty fields and one
//      hand-written table between them, so the test walks the WHOLE catalogue
//      and round-trips every writable key rather than spot-checking three: a
//      row whose member pointer names the neighbouring field is invisible until
//      somebody wonders why Bloom Threshold moves the intensity.
//
// The rest are the refusals, and they matter for different reasons:
//   • the Remote Control rows are the switch this bridge itself came through,
//   • a wrong type or an out-of-range number must be refused rather than
//     written and clamped somewhere the caller cannot see,
//   • `scriptLanguage` and `appProject` say what the project IS,
//   • a startup scene that is not there gives a packaged game that cannot boot,
//     reported as a successful export,
//   • the two values that reach their owner through a callback have to actually
//     call it — a write that only set the struct would be a number in a file
//     that changes nothing.

using HE::Ed::McpSettingsHooks;
using HE::Ed::McpTool;
using HE::Ed::McpToolRegistry;
using HE::Ed::SettingStorage;
using HE::Ed::SettingType;
using HE::Ed::ToolResult;
using HE::Ed::editorSettingCatalog;
using HE::Ed::kRemoteControlCategory;
using nlohmann::json;

namespace fs = std::filesystem;

namespace {

std::string codeOf(const ToolResult& r)
{
	return r.isError ? r.errorCode : std::string("<ok>");
}

// The whole surface, over a real project on disk.
struct Fixture
{
	fs::path        root;
	ProjectManager  pm;
	EditorConfig    cfg;
	McpToolRegistry registry;

	bool        haveProject   = true;
	int         saveCount     = 0;
	bool        saveFails     = false;
	int         layersApplied = 0;
	int         persistCount  = 0;
	std::vector<std::string> applied;   // the `apply` names that were honoured
	bool        vsync = true;

	explicit Fixture(const std::string& name, bool withProject = true)
	{
		root = fs::temp_directory_path() /
		       ("he_test_mcp_settings_" + name + "_" + std::to_string(::rand()));
		fs::create_directories(root);
		if (withProject)
			REQUIRE(pm.createNewProject((root / "Demo").string(), "Demo"));
		else
			haveProject = false;

		McpSettingsHooks h;
		h.project = [this]() -> ProjectData* {
			return haveProject ? &pm.currentProject() : nullptr;
		};
		h.saveProject = [this] {
			++saveCount;
			if (saveFails) return false;
			return pm.saveProject(pm.currentProject().path);
		};
		h.applyCollisionLayers = [this] { ++layersApplied; };
		h.editorConfig         = [this] { return &cfg; };
		h.persistEditorConfig  = [this] { ++persistCount; return true; };
		h.readExternalSetting  = [this](const std::string& key, json& out) {
			if (key == "display.vsync")   { out = vsync;               return true; }
			if (key == "display.backend") { out = std::string("Metal"); return true; }
			return false;
		};
		h.writeExternalSetting = [this](const std::string& key, const json& in,
		                                std::string& err) {
			if (key == "display.vsync")
			{
				if (!in.is_boolean()) { err = "expected a boolean"; return false; }
				vsync = in.get<bool>();
				return true;
			}
			err = "'" + key + "' is not writable";
			return false;
		};
		h.applySetting = [this](const std::string& what, const EditorConfig&) {
			applied.push_back(what);
		};
		HE::Ed::registerSettingsTools(registry, std::move(h));
	}

	~Fixture() { he_test::removeAllQuiet(root); }

	ToolResult call(const std::string& name, const json& args = json::object())
	{
		const McpTool* t = registry.find(name);
		REQUIRE_MESSAGE(t != nullptr, "no such tool registered: " << name);
		return t->handler(args);
	}

	ToolResult set(const std::string& key, const json& value)
	{
		return call("settings_set", json{ { "key", key }, { "value", value } });
	}

	// What the MANIFEST says, read through a second manager. The only honest
	// answer to "did that survive".
	ProjectData reload()
	{
		ProjectManager fresh;
		REQUIRE(fresh.loadProject(pm.currentProject().path));
		return fresh.currentProject();
	}
};

// A value of the right shape for a descriptor, different from what is there now.
json otherValue(const HE::Ed::SettingDesc& d, const EditorConfig& cfg)
{
	switch (d.type)
	{
	case SettingType::Bool:   return json(!(cfg.*(d.pb)));
	case SettingType::Int:
	{
		const int cur = cfg.*(d.pi);
		const int lo  = static_cast<int>(d.minValue), hi = static_cast<int>(d.maxValue);
		return json(cur == lo ? std::min(hi, cur + 1) : cur - 1);
	}
	case SettingType::Float:
	{
		const double cur = static_cast<double>(cfg.*(d.pf));
		const double mid = (d.minValue + d.maxValue) * 0.5;
		return json(cur == mid ? d.minValue : mid);
	}
	case SettingType::String: return json(std::string("he-test-value"));
	case SettingType::Enum:
	{
		const int cur = cfg.*(d.pi);
		const int next = (cur + 1) % static_cast<int>(d.options.size());
		return json(d.options[static_cast<std::size_t>(next)]);
	}
	}
	return json();
}

} // namespace

TEST_CASE("settings tools: both register with legal names and schemas")
{
	Fixture f("reg");
	for (const char* n : { "settings_get", "settings_set" })
	{
		const McpTool* t = f.registry.find(n);
		REQUIRE_MESSAGE(t != nullptr, "missing tool: " << n);
		CHECK(McpToolRegistry::enforceNameRule(t->name));
		CHECK(t->inputSchema["type"] == "object");
	}
	CHECK_FALSE(f.registry.find("settings_get")->mutates);
	CHECK(f.registry.find("settings_set")->mutates);
}

TEST_CASE("the editor catalogue is internally consistent")
{
	// Every row has to be addressable, typed and — unless it lives elsewhere —
	// actually bound to a field. A descriptor with no member pointer would read
	// and write nothing while reporting success.
	std::vector<std::string> seen;
	for (const HE::Ed::SettingDesc& d : editorSettingCatalog())
	{
		CAPTURE(d.key);
		CHECK_FALSE(d.key.empty());
		CHECK_FALSE(d.label.empty());
		CHECK_FALSE(d.category.empty());
		for (const std::string& s : seen) CHECK(s != d.key);
		seen.push_back(d.key);

		if (d.storage == SettingStorage::Config)
		{
			const int bound = (d.pb ? 1 : 0) + (d.pi ? 1 : 0) + (d.pf ? 1 : 0) + (d.ps ? 1 : 0);
			CHECK(bound == 1);
			if (d.type == SettingType::Bool)   CHECK(d.pb != nullptr);
			if (d.type == SettingType::Int)    CHECK(d.pi != nullptr);
			if (d.type == SettingType::Float)  CHECK(d.pf != nullptr);
			if (d.type == SettingType::String) CHECK(d.ps != nullptr);
			if (d.type == SettingType::Enum) { CHECK(d.pi != nullptr); CHECK(d.options.size() >= 2); }
		}
		else
			CHECK(d.pb == nullptr);

		// A row that cannot be written has to say why; an unexplained refusal is
		// one a caller can only retry.
		if (!d.writable) CHECK_FALSE(d.readOnlyReason.empty());
	}
	CHECK(seen.size() > 20);
}

TEST_CASE("settings_set: every writable editor row lands in the field it names")
{
	Fixture f("roundtrip");
	int written = 0;
	for (const HE::Ed::SettingDesc& d : editorSettingCatalog())
	{
		if (d.storage != SettingStorage::Config || !d.writable) continue;
		CAPTURE(d.key);
		const json want = otherValue(d, f.cfg);
		const ToolResult w = f.set(d.key, want);
		REQUIRE_MESSAGE(codeOf(w) == "<ok>", d.key << ": " << w.errorMessage);
		CHECK(w.content["changed"] == true);

		// Read it back through settings_get, not through the struct: the read
		// path is the one a client actually uses, and a broken one would make the
		// write untestable from outside.
		const ToolResult g = f.call("settings_get", json{ { "key", d.key } });
		REQUIRE(codeOf(g) == "<ok>");
		// A Float row is a `float` field, so what comes back is the double the
		// caller sent rounded to single precision. Comparing exactly would fail
		// on every non-representable value and say nothing about the binding.
		if (d.type == SettingType::Float)
			CHECK(g.content["value"].get<double>() ==
			      doctest::Approx(want.get<double>()).epsilon(1e-6));
		else
			CHECK(g.content["value"] == want);
		++written;
	}
	CHECK(written > 20);
}

TEST_CASE("settings_set: the Remote Control rows are never written from here")
{
	Fixture f("remote");
	int rows = 0;
	for (const HE::Ed::SettingDesc& d : editorSettingCatalog())
	{
		if (d.category != kRemoteControlCategory) continue;
		++rows;
		CAPTURE(d.key);
		// Readable — a client may see the state it is living in…
		const ToolResult g = f.call("settings_get", json{ { "key", d.key } });
		CHECK(codeOf(g) == "<ok>");
		CHECK(g.content["writable"] == false);
		// …and not writable, whatever it sends.
		const ToolResult w = f.set(d.key, d.type == SettingType::Bool ? json(false) : json(1234));
		CHECK(codeOf(w) == "read_only");
		CHECK(w.errorMessage.find("Remote Control") != std::string::npos);
	}
	REQUIRE(rows >= 2);
	// And nothing moved.
	CHECK_FALSE(f.cfg.McpServerEnabled);
	CHECK(f.cfg.McpPort == 0);
}

TEST_CASE("settings_set: a wrong type or an out-of-range number is refused")
{
	Fixture f("types");
	const float before = f.cfg.BloomIntensity;

	CHECK(codeOf(f.set("postProcess.bloomIntensity", json("a lot")))  == "invalid_argument");
	CHECK(codeOf(f.set("postProcess.bloomIntensity", json(999.0)))    == "invalid_argument");
	CHECK(codeOf(f.set("postProcess.bloomIntensity", json(-1.0)))     == "invalid_argument");
	CHECK(f.cfg.BloomIntensity == before);

	CHECK(codeOf(f.set("postProcess.bloomEnabled", json(1)))          == "invalid_argument");
	CHECK(codeOf(f.set("collab.maxAssetMB",        json(1.5)))        == "invalid_argument");
	CHECK(codeOf(f.set("collab.maxAssetMB",        json(99999)))      == "invalid_argument");
	CHECK(codeOf(f.set("nonsense.key",             json(true)))       == "not_found");
}

TEST_CASE("settings_set: an enum takes its NAME or its index, and nothing else")
{
	Fixture f("enums");
	CHECK(codeOf(f.set("postProcess.antiAliasing", json("SMAA"))) == "<ok>");
	CHECK(f.cfg.AntiAliasing == 2);
	CHECK(codeOf(f.set("postProcess.antiAliasing", json("taa")))  == "<ok>");   // case-insensitive
	CHECK(f.cfg.AntiAliasing == 3);
	CHECK(codeOf(f.set("postProcess.antiAliasing", json(1)))      == "<ok>");
	CHECK(f.cfg.AntiAliasing == 1);

	const ToolResult bad = f.set("postProcess.antiAliasing", json("MLAA"));
	CHECK(codeOf(bad) == "invalid_argument");
	// The refusal has to be actionable: it names what would have worked.
	CHECK(bad.errorMessage.find("SMAA") != std::string::npos);
	CHECK(codeOf(f.set("postProcess.antiAliasing", json(99))) == "invalid_argument");
	CHECK(f.cfg.AntiAliasing == 1);

	// And the read answers with the name, with the index alongside it.
	const ToolResult g = f.call("settings_get", json{ { "key", "postProcess.antiAliasing" } });
	REQUIRE(codeOf(g) == "<ok>");
	CHECK(g.content["value"] == "FXAA");
	CHECK(g.content["index"] == 1);
	CHECK(g.content["options"].size() == 5);
}

TEST_CASE("settings_set: the values that travel through a callback actually do")
{
	Fixture f("callbacks");
	// A field that IS in the config and additionally has to reach the frame
	// pacer. Setting only the struct would be a number in a file.
	REQUIRE(codeOf(f.set("display.maxFps", json(120.0))) == "<ok>");
	CHECK(f.cfg.MaxFps == doctest::Approx(120.0));
	REQUIRE(f.applied.size() == 1);
	CHECK(f.applied[0] == "maxfps");

	// A value that is not in the config at all: it goes out through the hook and
	// comes back through the other one.
	REQUIRE(f.vsync);
	const ToolResult w = f.set("display.vsync", json(false));
	REQUIRE(codeOf(w) == "<ok>");
	CHECK_FALSE(f.vsync);
	CHECK(w.content["value"] == false);
	// It does not live in config.json, so it is not "waiting to be written".
	CHECK(w.content["persisted"] == false);

	// A row that does neither does not call the apply hook.
	const std::size_t before = f.applied.size();
	REQUIRE(codeOf(f.set("postProcess.bloomThreshold", json(2.0))) == "<ok>");
	CHECK(f.applied.size() == before);
}

TEST_CASE("settings_set: the backend is readable and will not be changed")
{
	// The memory this guards: a dialog field that remembered "Software" across
	// projects turned every later packaged game into a dark window with no
	// scene. The backend is chosen at startup; this interface does not move it.
	Fixture f("backend");
	const ToolResult g = f.call("settings_get", json{ { "key", "display.backend" } });
	REQUIRE(codeOf(g) == "<ok>");
	CHECK(g.content["value"] == "Metal");
	CHECK(g.content["writable"] == false);
	CHECK(codeOf(f.set("display.backend", json("Software"))) == "read_only");
}

TEST_CASE("settings_set: an editor write reports honestly where it landed")
{
	Fixture f("persist");
	const ToolResult w = f.set("viewport.cameraSpeed", json(12.0));
	REQUIRE(codeOf(w) == "<ok>");
	CHECK(w.content["persisted"] == true);
	CHECK(f.persistCount == 1);

	// Without the hook, the truthful answer is "in effect now, on disk later" —
	// never a persisted:true nobody honoured.
	Fixture g("nopersist");
	McpSettingsHooks h;
	h.editorConfig = [&g] { return &g.cfg; };
	McpToolRegistry reg;
	HE::Ed::registerSettingsTools(reg, std::move(h));
	const ToolResult r = reg.find("settings_set")->handler(
		json{ { "key", "viewport.cameraSpeed" }, { "value", 12.0 } });
	REQUIRE(codeOf(r) == "<ok>");
	CHECK(r.content["persisted"] == false);
	CHECK(r.content["note"].get<std::string>().find("closes") != std::string::npos);
	CHECK(reg.find("settings_get")->handler(json::object())
	         .content["editor"]["persistsImmediately"] == false);
}

// ─── The project half ────────────────────────────────────────────────────────

TEST_CASE("settings_get: no project open is a state, not an error")
{
	Fixture f("noproj", /*withProject=*/false);
	const ToolResult all = f.call("settings_get");
	REQUIRE(codeOf(all) == "<ok>");
	CHECK(all.content["project"]["open"] == false);
	// …but the editor half is still there, because it does not need one.
	CHECK(all.content["editor"]["settings"].size() > 20);

	CHECK(codeOf(f.call("settings_get", json{ { "scope", "project" } })) == "no_project");
	CHECK(codeOf(f.set("project.allowFiles", json(true)))                == "no_project");
}

TEST_CASE("settings_set: a project write reaches the MANIFEST, not just the struct")
{
	Fixture f("projwrite");
	REQUIRE_FALSE(f.pm.currentProject().allowFiles);

	const ToolResult w = f.set("project.allowFiles", json(true));
	REQUIRE(codeOf(w) == "<ok>");
	CHECK(w.content["changed"] == true);
	CHECK(w.content["persisted"] == true);
	CHECK(w.content["value"] == true);

	// The only question worth asking: a SECOND manager, loading the file.
	CHECK(f.reload().allowFiles);

	// Several more, including the ones whose shapes differ.
	REQUIRE(codeOf(f.set("project.allowProcesses", json(true)))            == "<ok>");
	REQUIRE(codeOf(f.set("project.appVersion",     json("2.3")))           == "<ok>");
	REQUIRE(codeOf(f.set("project.themeMode",      json("dark")))          == "<ok>");
	REQUIRE(codeOf(f.set("project.fontScriptGreek", json(true)))           == "<ok>");
	REQUIRE(codeOf(f.set("project.advancedShaderEffects", json(false)))    == "<ok>");

	const ProjectData back = f.reload();
	CHECK(back.allowProcesses);
	CHECK(back.appVersion == "2.3");
	CHECK(back.themeMode  == "Dark");           // canonicalised, not echoed
	CHECK((back.fontScripts & HE::UIFontScriptGreek) != 0u);
	CHECK_FALSE(back.advancedShaderEffects);
}

TEST_CASE("settings_set: a manifest that cannot be written is not reported as a success")
{
	Fixture f("savefail");
	f.saveFails = true;
	const ToolResult w = f.set("project.allowFiles", json(true));
	CHECK(codeOf(w) == "write_failed");
	// The struct did move — the message says so rather than pretending nothing
	// happened, because pretending is what makes the next read confusing.
	CHECK(w.errorMessage.find("memory") != std::string::npos);
}

TEST_CASE("settings_set: the five fields that say what the project IS are read-only")
{
	Fixture f("readonly");
	for (const char* key : { "project.name", "project.path", "project.id",
	                         "project.scriptLanguage", "project.appProject" })
	{
		CAPTURE(key);
		const ToolResult g = f.call("settings_get", json{ { "key", key } });
		REQUIRE(codeOf(g) == "<ok>");
		CHECK(g.content["writable"] == false);
		CHECK_FALSE(g.content["readOnlyReason"].get<std::string>().empty());
		const ToolResult w = f.set(key, json("Lua"));
		CHECK(codeOf(w) == "read_only");
	}
	CHECK(f.saveCount == 0);
}

TEST_CASE("settings_get: the startup scene is answered project-relative, and not written")
{
	// Not a policy: ProjectManager::saveProject is a read-modify-write that keeps
	// the manifest's own "startupScene" key and never writes this field back. A
	// setter here would move the struct, report persisted:true because the file
	// WAS written, and lose the value on the next load. The editor has no surface
	// for it either — what a BUILD boots into is the export profile's own
	// startupScene, and that one project_package takes as an argument.
	Fixture f("startup");
	const std::string had = f.pm.currentProject().startupScene;
	REQUIRE_FALSE(had.empty());          // the preset seeds one

	const ToolResult g = f.call("settings_get", json{ { "key", "project.startupScene" } });
	REQUIRE(codeOf(g) == "<ok>");
	CHECK(g.content["writable"] == false);
	// Absolute in the struct (that is the field), project-relative on the wire —
	// an absolute path off this machine means nothing to anybody else.
	CHECK(fs::path(had).is_absolute());
	CHECK(g.content["value"] == "Content/StartupScene.hescene");
	CHECK(g.content["readOnlyReason"].get<std::string>().find("project_package")
	      != std::string::npos);

	const ToolResult w = f.set("project.startupScene", json("Content/Other.hescene"));
	CHECK(codeOf(w) == "read_only");
	CHECK(f.pm.currentProject().startupScene == had);
	CHECK(f.saveCount == 0);
}

TEST_CASE("settings_set: the active export profile has to name one that exists")
{
	Fixture f("profile");
	auto& proj = f.pm.currentProject();
	REQUIRE(proj.exportProfiles.size() >= 2);

	const ToolResult bad = f.set("project.activeExportProfile", json("Release"));
	CHECK(codeOf(bad) == "invalid_argument");
	CHECK(bad.errorMessage.find(proj.exportProfiles[0].name) != std::string::npos);

	const std::string want = proj.exportProfiles[0].name;
	REQUIRE(codeOf(f.set("project.activeExportProfile", json(want))) == "<ok>");
	CHECK(f.reload().activeExportProfile == want);
}

TEST_CASE("settings_get: the project scope carries the export profiles and the matrix")
{
	Fixture f("projread");
	const ToolResult r = f.call("settings_get", json{ { "scope", "project" } });
	REQUIRE(codeOf(r) == "<ok>");
	const json& p = r.content["project"];
	CHECK(p["settings"].size() > 15);
	CHECK(p["exportProfiles"].size() >= 2);
	CHECK(p["collisionLayers"]["layers"].size() == HE::CollisionLayerConfig::kCount);
	CHECK(p["collisionLayers"]["isDefault"] == true);
	// A fresh project collides with everything, so there is nothing blocked yet.
	CHECK(p["collisionLayers"]["blockedPairs"].empty());
	CHECK_FALSE(r.content.contains("editor"));
}

TEST_CASE("settings_set: the collision matrix stays symmetric and reaches the simulation")
{
	Fixture f("layers");
	REQUIRE(codeOf(f.set("project.collisionLayers.name.1", json("Player"))) == "<ok>");
	REQUIRE(codeOf(f.set("project.collisionLayers.collides.1.2", json(false))) == "<ok>");

	const ProjectData back = f.reload();
	CHECK(back.collisionLayers.layerName(1) == "Player");
	// Both cells, because Jolt does not promise which order it asks in.
	CHECK_FALSE(back.collisionLayers.collides(1, 2));
	CHECK_FALSE(back.collisionLayers.collides(2, 1));
	// The matrix only reaches a RUNNING simulation through this callback.
	CHECK(f.layersApplied == 2);

	// The pair now shows up in the read, with the key that set it.
	const ToolResult r = f.call("settings_get", json{ { "scope", "project" } });
	REQUIRE(codeOf(r) == "<ok>");
	REQUIRE(r.content["project"]["collisionLayers"]["blockedPairs"].size() == 1);
	CHECK(r.content["project"]["collisionLayers"]["blockedPairs"][0]["key"]
	      == "project.collisionLayers.collides.1.2");

	// Out of range is a refusal rather than a write past the end of the array.
	CHECK(codeOf(f.set("project.collisionLayers.name.99", json("X")))       == "invalid_argument");
	CHECK(codeOf(f.set("project.collisionLayers.collides.1", json(false)))  == "invalid_argument");
	CHECK(codeOf(f.set("project.collisionLayers.collides.1.99", json(false))) == "invalid_argument");
	CHECK(codeOf(f.set("project.collisionLayers.name.1", json(7)))          == "invalid_argument");
}

TEST_CASE("settings_get: a category filter answers only that page")
{
	Fixture f("filter");
	const ToolResult r = f.call("settings_get", json{ { "category", "Permissions" } });
	REQUIRE(codeOf(r) == "<ok>");
	const json& rows = r.content["project"]["settings"];
	REQUIRE(rows.size() == 3);
	for (const json& row : rows) CHECK(row["category"] == "Permissions");
	// A filtered read answers only what it asked for — no profiles, no matrix.
	CHECK_FALSE(r.content["project"].contains("exportProfiles"));
	// …and the editor half of the same category is simply empty.
	CHECK(r.content["editor"]["settings"].empty());

	const ToolResult gi = f.call("settings_get", json{ { "category", "Global Illumination" } });
	REQUIRE(codeOf(gi) == "<ok>");
	CHECK(gi.content["editor"]["settings"].size() >= 8);
}

TEST_CASE("settings_get/set: bad arguments are named, not guessed at")
{
	Fixture f("args");
	CHECK(codeOf(f.call("settings_get", json{ { "scope", "everything" } })) == "invalid_argument");
	CHECK(codeOf(f.call("settings_get", json{ { "key", "no.such.thing" } })) == "not_found");
	CHECK(codeOf(f.call("settings_set", json{ { "key", "display.maxFps" } })) == "invalid_argument");
	CHECK(codeOf(f.call("settings_set", json{ { "value", 1 } }))              == "invalid_argument");
}

TEST_CASE("settings_get: every row a client is told it may write, it may write")
{
	// The promise the whole pair rests on: `writable: true` and a refusal are not
	// both allowed to be true of the same key.
	Fixture f("promise");
	const ToolResult all = f.call("settings_get");
	REQUIRE(codeOf(all) == "<ok>");
	for (const json& row : all.content["project"]["settings"])
	{
		if (!row["writable"].get<bool>()) continue;
		CAPTURE(row["key"].get<std::string>());
		// Setting a row to the value it already has must succeed: that is the
		// cheapest possible write and it exercises the same path.
		const ToolResult w = f.set(row["key"].get<std::string>(), row["value"]);
		CHECK_MESSAGE(codeOf(w) == "<ok>", w.errorMessage);
		CHECK(w.content["changed"] == false);
	}
}
