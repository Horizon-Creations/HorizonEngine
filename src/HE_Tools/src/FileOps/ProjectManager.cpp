#include "ProjectManager.h"
#include "CppScaffoldTemplates.h"
#include <ContentManager/DefaultAssets.h> // well-known UUIDs seeded into the tutorial scene
#include <Types/Enums.h>                  // HE::LightType
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <Diagnostics/Log.h>

namespace fs = std::filesystem;
using json   = nlohmann::json;

// ─── Export profiles ──────────────────────────────────────────────────────────

std::vector<ExportProfile> defaultExportProfiles()
{
	ExportProfile dev;
	dev.name             = "Development";
	dev.compress         = false;  // fast iteration: store, no crypto
	dev.encrypt          = false;
	dev.enableModSupport = true;

	ExportProfile ship;
	ship.name             = "Shipping";
	ship.compress         = true;  // zstd + AES for distribution
	ship.encrypt          = true;
	ship.enableModSupport = false;

	return { dev, ship };
}

// Type-checked json getters. .heproj manifests are user-editable text; nlohmann's
// value()/get<> throw type_error on a wrong-typed value, which would escape
// loadProject and (because the editor auto-loads the last project on startup)
// crash-loop the editor until the file is hand-fixed. Wrong types → default.
static std::string jsonString(const json& j, const char* key, const std::string& def = {})
{
	auto it = j.find(key);
	return (it != j.end() && it->is_string()) ? it->get<std::string>() : def;
}
static bool jsonBool(const json& j, const char* key, bool def)
{
	auto it = j.find(key);
	return (it != j.end() && it->is_boolean()) ? it->get<bool>() : def;
}

static json profileToJson(const ExportProfile& p)
{
	json j;
	j["name"]             = p.name;
	j["compress"]         = p.compress;
	j["encrypt"]          = p.encrypt;
	j["enableModSupport"] = p.enableModSupport;
	j["startupScene"]     = p.startupScene;
	j["outputDir"]        = p.outputDir;
	j["excludePatterns"]  = p.excludePatterns;
	j["incremental"]      = p.incremental;
	j["targetPlatform"]   = p.targetPlatform;
	j["appBundle"]        = p.appBundle;
	j["shaderBackends"]   = p.shaderBackends;
	j["compileHorizonCode"] = p.compileHorizonCode;
	return j;
}

static ExportProfile profileFromJson(const json& j)
{
	ExportProfile p;
	p.name             = jsonString(j, "name");
	p.compress         = jsonBool(j, "compress", true);
	p.encrypt          = jsonBool(j, "encrypt", false);
	p.enableModSupport = jsonBool(j, "enableModSupport", false);
	p.startupScene     = jsonString(j, "startupScene");
	p.outputDir        = jsonString(j, "outputDir");
	p.incremental      = jsonBool(j, "incremental", true);
	p.targetPlatform   = jsonString(j, "targetPlatform", "Host");
	if (p.targetPlatform.empty()) p.targetPlatform = "Host";
	p.appBundle        = jsonBool(j, "appBundle", false);
	p.shaderBackends   = j.contains("shaderBackends") && j["shaderBackends"].is_number_unsigned()
	                     ? j["shaderBackends"].get<uint32_t>() : ((1u << 4) | (1u << 0));
	p.compileHorizonCode = jsonBool(j, "compileHorizonCode", false);
	if (auto it = j.find("excludePatterns"); it != j.end() && it->is_array())
		for (const auto& e : *it)
			if (e.is_string()) p.excludePatterns.push_back(e.get<std::string>());
	return p;
}

// Read profiles from a .heproj json; seeds the defaults when absent/empty so
// callers can rely on exportProfiles never being empty.
static void readProfiles(const json& j, ProjectData& data)
{
	data.exportProfiles.clear();
	if (auto it = j.find("exportProfiles"); it != j.end() && it->is_array())
		for (const auto& e : *it)
		{
			if (!e.is_object()) continue; // malformed entry: skip, don't throw
			ExportProfile p = profileFromJson(e);
			if (!p.name.empty()) data.exportProfiles.push_back(std::move(p));
		}
	if (data.exportProfiles.empty())
		data.exportProfiles = defaultExportProfiles();

	data.activeExportProfile = jsonString(j, "activeExportProfile");
	const bool known = std::any_of(data.exportProfiles.begin(), data.exportProfiles.end(),
		[&](const ExportProfile& p) { return p.name == data.activeExportProfile; });
	if (!known) data.activeExportProfile = data.exportProfiles.front().name;
}

namespace HE::tools
{

const char* toString(ProjectScriptLanguage lang)
{
	switch (lang)
	{
	case ProjectScriptLanguage::Lua:    return "Lua";
	case ProjectScriptLanguage::Python: return "Python";
	case ProjectScriptLanguage::Cpp:    return "Cpp";
	case ProjectScriptLanguage::HorizonCode:
	default:                            return "HorizonCode";
	}
}

ProjectScriptLanguage projectScriptLanguageFromString(const std::string& s)
{
	if (s == "Lua")    return ProjectScriptLanguage::Lua;
	if (s == "Python") return ProjectScriptLanguage::Python;
	if (s == "Cpp")    return ProjectScriptLanguage::Cpp;
	return ProjectScriptLanguage::HorizonCode; // unknown/missing → default
}

} // namespace HE::tools

// ─── Native C++ gameplay scaffolding ─────────────────────────────────────────

std::string HE::tools::cppIdentifier(const std::string& name)
{
	std::string id;
	id.reserve(name.size());
	for (char c : name)
	{
		unsigned char u = static_cast<unsigned char>(c);
		id.push_back((std::isalnum(u) || c == '_') ? c : '_');
	}
	if (id.empty()) return "Unnamed";
	if (std::isdigit(static_cast<unsigned char>(id.front()))) id.insert(id.begin(), '_');
	return id;
}

namespace
{
// Write text to a file, creating parent folders. Overwrites.
bool writeTextFile(const fs::path& path, const std::string& content)
{
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	std::ofstream out(path, std::ios::trunc | std::ios::binary);
	if (!out.is_open()) return false;
	out << content;
	return out.good();
}

// Write only if the file does not already exist (idempotent scaffolding). Missing
// → written; present → left as-is. Returns false only on a write failure.
bool writeTextFileIfAbsent(const fs::path& path, const std::string& content)
{
	if (fs::exists(path)) return true;
	return writeTextFile(path, content);
}
} // namespace

bool writeCppLevelScript(const std::string& projectRoot, const std::string& sceneName)
{
	const fs::path source = fs::path(projectRoot) / "Source";
	const std::string className = HE::tools::cppIdentifier(sceneName) + "LevelScript";
	const fs::path header = source / (className + ".h");
	const fs::path body   = source / (className + ".cpp");
	// Idempotent: an existing level script for this scene is left untouched.
	if (fs::exists(header) && fs::exists(body)) return true;
	if (!writeTextFileIfAbsent(header, CppScaffold::levelScriptHeader(className, sceneName))) return false;
	if (!writeTextFileIfAbsent(body,   CppScaffold::levelScriptSource(className, sceneName))) return false;
	return true;
}

bool writeCppClass(const std::string& projectRoot, const std::string& className,
                   std::string* outCreatedHeaderPath)
{
	const fs::path source = fs::path(projectRoot) / "Source";
	const std::string base = HE::tools::cppIdentifier(className);
	std::string name = base;
	int counter = 1;
	while (fs::exists(source / (name + ".h")) || fs::exists(source / (name + ".cpp")))
		name = base + std::to_string(counter++);
	const fs::path header = source / (name + ".h");
	const fs::path body   = source / (name + ".cpp");
	if (!writeTextFile(header, CppScaffold::gameplayClassHeader(name))) return false;
	if (!writeTextFile(body,   CppScaffold::gameplayClassSource(name))) return false;
	if (outCreatedHeaderPath) *outCreatedHeaderPath = header.string();
	return true;
}

bool scaffoldTutorialProject(const std::string& projectRoot,
                             const std::string& projectName)
{
	const std::string readme =
		"# " + projectName + "\n"
		"\n"
		"A sandbox created by the Horizon Engine editor for the interactive tutorial.\n"
		"Nothing in here is special — it is an ordinary project, and every change you\n"
		"make while following the tour is a change to a real project you can keep.\n"
		"\n"
		"## What is already in it\n"
		"\n"
		"`Content/StartupScene.hescene` opens with:\n"
		"\n"
		"- **Sky** and **Weather** entities (atmosphere, clouds, wind — all scene data)\n"
		"- **Ground**, a wide flattened cube to stand things on\n"
		"- **Cube**, a single box above the ground to select, move and re-material\n"
		"- **Point Light**, a local light next to it\n"
		"\n"
		"## Reopening the tour\n"
		"\n"
		"Help - Interactive Tutorial. Your place in it is remembered between sessions,\n"
		"and the Tutorial template in the Project Hub recreates this sandbox at any\n"
		"time if you want to start over without touching the project you built here.\n";

	std::error_code ec;
	fs::create_directories(fs::path(projectRoot), ec);
	return writeTextFileIfAbsent(fs::path(projectRoot) / "TUTORIAL.md", readme);
}

bool scaffoldCppProject(const std::string& projectRoot,
                        const std::string& projectName,
                        const std::string& startupSceneName)
{
	const fs::path source = fs::path(projectRoot) / "Source";
	std::error_code ec;
	fs::create_directories(source, ec);

	bool ok = true;
	ok &= writeTextFileIfAbsent(source / "GameLogicRuntime.h",  CppScaffold::runtimeHeader());
	ok &= writeTextFileIfAbsent(source / "GameLogicRuntime.cpp", CppScaffold::runtimeSource());
	ok &= writeTextFileIfAbsent(source / "GameInstance.h",      CppScaffold::gameInstanceHeader());
	ok &= writeTextFileIfAbsent(source / "GameInstance.cpp",    CppScaffold::gameInstanceSource());
	ok &= writeTextFileIfAbsent(source / "GameLogic.cpp",       CppScaffold::gameLogicSource(startupSceneName));
	ok &= writeTextFileIfAbsent(source / "CMakeLists.txt",      CppScaffold::cmakeLists(HE::tools::cppIdentifier(projectName)));
	ok &= writeTextFileIfAbsent(source / "README.md",          CppScaffold::readme(projectName));
	ok &= writeCppLevelScript(projectRoot, startupSceneName);
	return ok;
}

// ─── Startup scene ────────────────────────────────────────────────────────────
// The Content/StartupScene.hescene every new project gets. Root entity "World"
// (id 0, no parent); Game/Simulation/Tutorial additionally get the dedicated
// "Sky" (EnvironmentComponent) and "Weather" (WeatherComponent) entities, and
// Tutorial gets a furnished sandbox on top of that — the guided tour's first
// chapters are about selecting, moving and re-materialling something, which needs
// there to *be* something.
//
// An empty component object lets the scene loader fill every field from the
// struct defaults, so this stays decoupled from the components' exact field lists;
// the tutorial entities only spell out the fields whose defaults are wrong for
// them. Empty/Tool projects start with a bare world (add a Sky via the editor's
// Environment window).
static json startupSceneJson(ProjectPreset preset)
{
	constexpr uint32_t kNull = std::numeric_limits<uint32_t>::max();

	const bool seedEnvironment = (preset == ProjectPreset::Game ||
	                              preset == ProjectPreset::Simulation ||
	                              preset == ProjectPreset::Tutorial);
	const bool furnish = (preset == ProjectPreset::Tutorial);

	auto vec3 = [](float x, float y, float z) { return json::array({ x, y, z }); };
	// Matches SceneSerializer's uuidToJson: [hi, lo].
	auto uuid = [](const HE::UUID& id) { return json::array({ id.hi, id.lo }); };
	auto transform = [&](json position, json scale)
	{
		return json{ { "position", std::move(position) },
		             { "rotation", vec3(0.0f, 0.0f, 0.0f) },
		             { "scale",    std::move(scale) } };
	};

	json entities = json::array();
	json rootChildren = json::array();

	json rootEntity;
	rootEntity["id"]     = 0;
	rootEntity["name"]   = "World";
	rootEntity["parent"] = kNull;
	// Filled in below once the children are known — pushed last so the array is
	// complete, but kept at index 0 of `entities` for readability of the file.
	entities.push_back(rootEntity);

	auto addChild = [&](uint32_t id, const char* name, json components)
	{
		rootChildren.push_back(id);
		json e;
		e["id"]         = id;
		e["name"]       = name;
		e["parent"]     = 0;
		e["children"]   = json::array();
		e["components"] = std::move(components);
		entities.push_back(std::move(e));
	};

	if (seedEnvironment)
	{
		// The tutorial's Sky is the one seeded environment that is not left at the
		// struct defaults. Those are a dome sky with the day-night cycle OFF, which
		// means timeOfDay does nothing at all (the renderer then takes the sun from
		// the scene's own directional light — see EnvironmentSettings.h) and the
		// frame is flat and shadowless. Turning the cycle on is what makes the
		// tour's "drag the time-of-day slider" step actually move the sun.
		// Keys are EnvironmentComponent member names (HE_ENV_FIELDS_* /
		// SceneSerializer); anything left out falls back to the struct default.
		addChild(1, "Sky", json{ { "environment", furnish
			? json{ { "dayNightCycle", true  },   // without this timeOfDay is inert
			        { "timeOfDay",     0.32f },   // mid-morning: raking light, long shadows
			        { "cloudMode",     1     },   // volumetric clouds rather than the dome
			        { "cloudCoverage", 0.4f  } }
			: json::object() } });
		addChild(2, "Weather", json{ { "weather", json::object() } });
	}

	if (furnish)
	{
		// A wide, flattened cube rather than the Plane primitive: it has thickness,
		// so a box collider added during the physics chapter behaves sensibly and
		// the tutorial's falling cube lands on something instead of through it.
		// The neutral-grey terrain material, not the white default one — on the
		// default material the cube would be white geometry on a white floor.
		addChild(3, "Ground", json{
			{ "transform", transform(vec3(0.0f, -0.25f, 0.0f), vec3(24.0f, 0.5f, 24.0f)) },
			{ "mesh",      json{ { "asset", uuid(HE::kDefaultCubeMeshId) } } },
			{ "material",  json{ { "asset", uuid(HE::kDefaultTerrainMaterialId) } } },
		});
		addChild(4, "Cube", json{
			{ "transform", transform(vec3(0.0f, 1.0f, 0.0f), vec3(1.0f, 1.0f, 1.0f)) },
			{ "mesh",      json{ { "asset", uuid(HE::kDefaultCubeMeshId) } } },
			{ "material",  json{ { "asset", uuid(HE::kDefaultMaterialId) } } },
		});
		addChild(5, "Point Light", json{
			{ "transform", transform(vec3(2.5f, 3.0f, 2.5f), vec3(1.0f, 1.0f, 1.0f)) },
			{ "light",     json{ { "type",      static_cast<uint8_t>(HE::LightType::Point) },
			                     { "color",     vec3(1.0f, 0.85f, 0.7f) },
			                     { "intensity", 4.0f },
			                     { "range",     12.0f } } },
		});
	}

	entities[0]["children"] = std::move(rootChildren);

	json scene;
	scene["version"]  = "1.1";
	scene["entities"] = std::move(entities);
	return scene;
}

bool ProjectManager::createNewProject(const std::string& projectDir,
									  const std::string& projectName,
									  ProjectPreset preset,
									  ProjectScriptLanguage scriptLanguage)
{
	fs::path root(projectDir);
	if (!fs::exists(root))
	{
		if (!fs::create_directories(root))
			return false;
	}

	// ── Common sub-folders ────────────────────────────────────────────────────
	fs::create_directories(root / "Content");
	fs::create_directories(root / "Config");
	fs::create_directories(root / "Shaders");

	// ── Preset-specific sub-folders ───────────────────────────────────────────
	switch (preset)
	{
	// The tutorial sandbox is a Game project with a furnished scene — the tour
	// walks through scripts, audio, models, materials, prefabs and UI, so every
	// folder it mentions has to already be there to put things in.
	case ProjectPreset::Tutorial:
	case ProjectPreset::Game:
		fs::create_directories(root / "Content" / "Scripts");
		fs::create_directories(root / "Content" / "Audio");
		fs::create_directories(root / "Content" / "Scenes");
		fs::create_directories(root / "Content" / "Models");
		fs::create_directories(root / "Content" / "Textures");
		fs::create_directories(root / "Content" / "Materials");
		fs::create_directories(root / "Content" / "Prefabs");
		fs::create_directories(root / "Content" / "UI");
		break;
	case ProjectPreset::Simulation:
		fs::create_directories(root / "Content" / "Data");
		fs::create_directories(root / "Content" / "Materials");
		break;
	case ProjectPreset::Tool:
		fs::create_directories(root / "Content" / "Source");
		fs::create_directories(root / "Content" / "UI");
		break;
	case ProjectPreset::Empty:
	default:
		break;
	}

	// Text-scripting languages get a Scripts folder regardless of preset — the
	// natural home for the project's first .hasset script assets.
	if (scriptLanguage == ProjectScriptLanguage::Lua ||
	    scriptLanguage == ProjectScriptLanguage::Python)
		fs::create_directories(root / "Content" / "Scripts");

	// ── Write .heproj manifest ─────────────────────────────────────────────────
	// Default startup scene: Content/StartupScene.hescene
	fs::path scenePath = root / "Content" / "StartupScene.hescene";

	{
		std::ofstream sceneOut(scenePath);
		if (sceneOut.is_open())
			sceneOut << startupSceneJson(preset).dump(4);
	}

	json j;
	j["name"]           = projectName;
	j["version"]        = "1.0";
	j["preset"]         = static_cast<int>(preset);
	j["startupScene"]   = "Content/StartupScene.hescene";
	j["scriptLanguage"] = HE::tools::toString(scriptLanguage);

	// Seed the default packaging profiles so Build > Export works out of the box.
	const auto profiles = defaultExportProfiles();
	json jp = json::array();
	for (const auto& p : profiles) jp.push_back(profileToJson(p));
	j["exportProfiles"]      = std::move(jp);
	j["activeExportProfile"] = profiles.front().name;

	fs::path heprojPath = root / (projectName + ".heproj");
	std::ofstream out(heprojPath);
	if (!out.is_open())
		return false;
	out << j.dump(4);
	out.close();

	// A C++ project authors gameplay as a native GameLogic library, not engine
	// assets: lay down a compilable Source/ tree (runtime + GameInstance + a level
	// script for the startup scene + CMakeLists). Non-fatal on failure — the
	// project is still usable; the user can regenerate the scaffold.
	if (scriptLanguage == ProjectScriptLanguage::Cpp)
		scaffoldCppProject(root.string(), projectName, scenePath.stem().string());

	// Same deal for the tutorial sandbox's TUTORIAL.md: nice to have, never a
	// reason to fail a project that is otherwise complete on disk.
	if (preset == ProjectPreset::Tutorial)
		scaffoldTutorialProject(root.string(), projectName);

	m_currentProject.name                = projectName;
	m_currentProject.path                = heprojPath.string();
	m_currentProject.startupScene        = scenePath.string();
	m_currentProject.exportProfiles      = profiles;
	m_currentProject.activeExportProfile = profiles.front().name;
	m_currentProject.scriptLanguage      = scriptLanguage;
	HE_LOG_INFO(Config, "Created project '%s' at '%s': language %s, preset %d, "
	                    "%zu export profile(s), startup scene '%s'",
	            m_currentProject.name.c_str(), m_currentProject.path.c_str(),
	            HE::tools::toString(m_currentProject.scriptLanguage),
	            static_cast<int>(preset), m_currentProject.exportProfiles.size(),
	            m_currentProject.startupScene.c_str());

	if (m_onProjectLoaded)
		m_onProjectLoaded(m_currentProject.startupScene);
	return true;
}

bool ProjectManager::loadProject(const std::string& projectPath)
{
	// Opening a project is the first thing a user does; "nothing happened" with no
	// reason is the worst possible answer, so each rejection says which it was.
	if (!fs::exists(projectPath))
	{
		HE_LOG_ERROR(Config, "Cannot open project: '%s' does not exist", projectPath.c_str());
		return false;
	}

	std::ifstream in(projectPath);
	if (!in.is_open())
	{
		HE_LOG_ERROR(Config, "Cannot open project '%s' for reading (permissions?)",
		             projectPath.c_str());
		return false;
	}

	json j = json::parse(in, nullptr, false);
	if (j.is_discarded())
	{
		HE_LOG_ERROR(Config, "Project file '%s' is not valid JSON — it is corrupt or was "
		                     "written by a different tool", projectPath.c_str());
		return false;
	}

	m_currentProject.name = jsonString(j, "name", fs::path(projectPath).stem().string());
	m_currentProject.path = projectPath;

	// Resolve startup scene relative to the project root
	fs::path projectRoot = fs::path(projectPath).parent_path();
	std::string relScene = jsonString(j, "startupScene");
	if (!relScene.empty())
	{
		fs::path absScene = projectRoot / relScene;
		m_currentProject.startupScene = fs::exists(absScene) ? absScene.string() : "";
		if (m_currentProject.startupScene.empty())
			HE_LOG_WARN(Config, "Project '%s' names startup scene '%s', which does not "
			                    "exist — the editor will open with an empty scene",
			            m_currentProject.name.c_str(), relScene.c_str());
	}
	else
	{
		m_currentProject.startupScene = "";
	}

	readProfiles(j, m_currentProject);
	m_currentProject.scriptLanguage =
		HE::tools::projectScriptLanguageFromString(jsonString(j, "scriptLanguage"));

	HE_LOG_INFO(Config, "Project '%s' loaded from '%s': language %s, %zu export profile(s), "
	                    "startup scene '%s'",
	            m_currentProject.name.c_str(), projectPath.c_str(),
	            HE::tools::toString(m_currentProject.scriptLanguage),
	            m_currentProject.exportProfiles.size(),
	            m_currentProject.startupScene.empty() ? "(none)"
	                                                  : m_currentProject.startupScene.c_str());

	if (m_onProjectLoaded)
		m_onProjectLoaded(m_currentProject.startupScene);
	return true;
}

bool ProjectManager::saveProject(const std::string& projectPath)
{
	// Read-modify-write: preserve every key the manifest already has (preset,
	// startupScene, future fields) and only overwrite what ProjectData owns.
	json j;
	if (fs::exists(projectPath))
	{
		// An existing manifest we cannot read or parse must NOT be clobbered
		// with a fresh skeleton — it may be hand-recoverable.
		std::ifstream in(projectPath);
		if (!in.is_open())
		{
			HE_LOG_ERROR(Config, "Project save aborted: '%s' exists but cannot be read",
			             projectPath.c_str());
			return false;
		}
		json existing = json::parse(in, nullptr, false);
		if (existing.is_discarded() || !existing.is_object())
		{
			HE_LOG_ERROR(Config, "Project save aborted: '%s' is corrupt and would be "
			                     "overwritten — fix or remove it first, it may still be "
			                     "recoverable by hand", projectPath.c_str());
			return false;
		}
		j = std::move(existing);
	}

	j["name"]    = m_currentProject.name;
	if (!j.contains("version")) j["version"] = "1.0";

	json jp = json::array();
	for (const auto& p : m_currentProject.exportProfiles)
		jp.push_back(profileToJson(p));
	j["exportProfiles"]      = std::move(jp);
	j["activeExportProfile"] = m_currentProject.activeExportProfile;
	j["scriptLanguage"]      = HE::tools::toString(m_currentProject.scriptLanguage);

	// Write temp + rename: an in-place ofstream truncates the only copy before
	// the new content is durable, so disk-full/kill mid-write would leave an
	// empty manifest. rename() swaps atomically on POSIX.
	const std::string tmpPath = projectPath + ".tmp";
	{
		std::ofstream out(tmpPath, std::ios::trunc);
		if (!out.is_open())
		{
			HE_LOG_ERROR(Config, "Project save failed: cannot create '%s'", tmpPath.c_str());
			return false;
		}
		// "replace" error handler: default dump() throws type_error.316 on invalid
		// UTF-8 (e.g. a project name), which would abort the app instead of saving.
		out << j.dump(4, ' ', false, json::error_handler_t::replace);
		out.flush();
		if (!out.good())
		{
			out.close();
			std::error_code ec;
			fs::remove(tmpPath, ec);
			HE_LOG_ERROR(Config, "Project save failed while writing '%s' (disk full?) — "
			                     "the previous file is intact", tmpPath.c_str());
			return false;
		}
	}
	std::error_code ec;
	fs::rename(tmpPath, projectPath, ec);
	if (ec)
	{
		std::error_code ec2;
		fs::remove(tmpPath, ec2);
		HE_LOG_ERROR(Config, "Project save failed: could not replace '%s' (%s) — "
		                     "the previous file is intact",
		             projectPath.c_str(), ec.message().c_str());
		return false;
	}
	HE_LOG_INFO(Config, "Project saved to '%s'", projectPath.c_str());
	return true;
}

void ProjectManager::closeProject()
{
	if (!m_currentProject.name.empty())
		HE_LOG_INFO(Config, "Project '%s' closed", m_currentProject.name.c_str());
	m_currentProject = {};

}
