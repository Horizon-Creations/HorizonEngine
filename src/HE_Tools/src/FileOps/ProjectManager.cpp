#include "ProjectManager.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

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

// ─── Native C++ gameplay scaffolding ─────────────────────────────────────────

std::string cppIdentifier(const std::string& name)
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

// ── Templates ─────────────────────────────────────────────────────────────────
// Every generated method is an empty stub, so the whole tree compiles against
// only <IGameLogic.h> before the user writes any gameplay code.

std::string tmplRuntimeHeader()
{
	return
R"(#pragma once
// ── HorizonEngine C++ GameLogic runtime ──────────────────────────────────────
// Shared base types for a native gameplay project. Generated once; safe to
// extend. Depends only on the standard library — HorizonWorld is forward-declared
// and merely passed through, so this header pulls in no engine sources.
#include <functional>
#include <memory>
#include <string>

class HorizonWorld; // the engine world, injected into every gameplay hook

// One GameInstance per project: app-wide lifecycle, before/after everything.
class GameInstanceBase
{
public:
	virtual ~GameInstanceBase() = default;
	virtual void onInit(HorizonWorld& /*world*/) {}
	virtual void onShutdown(HorizonWorld& /*world*/) {}
	virtual void onWindowFocusChanged(HorizonWorld& /*world*/, bool /*focused*/) {}
};

// One LevelScript per scene: the level's own lifecycle + per-frame tick.
class LevelScript
{
public:
	virtual ~LevelScript() = default;
	virtual void onLevelLoaded(HorizonWorld& /*world*/) {}
	virtual void onLevelUnloaded(HorizonWorld& /*world*/) {}
	virtual void onUpdate(HorizonWorld& /*world*/, float /*dt*/) {}
};

// ── Level-script registry ─────────────────────────────────────────────────────
// Each <Scene>LevelScript.cpp self-registers by scene name via
// REGISTER_LEVEL_SCRIPT, so GameLogic can instantiate the right script for the
// active scene and adding a level never means editing GameLogic.cpp.
using LevelScriptFactory = std::function<std::unique_ptr<LevelScript>()>;
void registerLevelScript(const std::string& sceneName, LevelScriptFactory factory);
std::unique_ptr<LevelScript> createLevelScript(const std::string& sceneName);

struct LevelScriptAutoRegister
{
	LevelScriptAutoRegister(const std::string& sceneName, LevelScriptFactory f)
	{ registerLevelScript(sceneName, std::move(f)); }
};

#define REGISTER_LEVEL_SCRIPT(SCENE_NAME, CLASS)              \
	static LevelScriptAutoRegister s_autoRegister_##CLASS(    \
		SCENE_NAME, [] { return std::make_unique<CLASS>(); })
)";
}

std::string tmplRuntimeSource()
{
	return
R"(#include "GameLogicRuntime.h"
#include <unordered_map>

// A function-local static keeps registration order-independent: level scripts
// register during static init, before main().
static std::unordered_map<std::string, LevelScriptFactory>& registry()
{
	static std::unordered_map<std::string, LevelScriptFactory> r;
	return r;
}

void registerLevelScript(const std::string& sceneName, LevelScriptFactory factory)
{
	registry()[sceneName] = std::move(factory);
}

std::unique_ptr<LevelScript> createLevelScript(const std::string& sceneName)
{
	auto it = registry().find(sceneName);
	return it != registry().end() ? it->second() : nullptr;
}
)";
}

std::string tmplGameInstanceHeader()
{
	return
R"(#pragma once
#include "GameLogicRuntime.h"

// App-wide game instance. Exactly one exists for the whole run; its hooks fire
// before any level loads and after the last one unloads.
class GameInstance : public GameInstanceBase
{
public:
	void onInit(HorizonWorld& world) override;
	void onShutdown(HorizonWorld& world) override;
	void onWindowFocusChanged(HorizonWorld& world, bool focused) override;
};
)";
}

std::string tmplGameInstanceSource()
{
	return
R"(#include "GameInstance.h"

void GameInstance::onInit(HorizonWorld& world)
{
	(void)world;
	// Runs once at startup, before the first level loads.
}

void GameInstance::onShutdown(HorizonWorld& world)
{
	(void)world;
	// Runs once at shutdown, after the last level unloads.
}

void GameInstance::onWindowFocusChanged(HorizonWorld& world, bool focused)
{
	(void)world; (void)focused;
	// Runs whenever the game window gains or loses focus.
}
)";
}

std::string tmplGameLogicSource(const std::string& startupScene)
{
	std::string s =
R"(// ── GameLogic entry point ─────────────────────────────────────────────────────
// The engine loads this shared library (named GameLogic.{dll,dylib,so}) next to
// the game executable and calls the two exported factory functions below. This
// default implementation drives the app-wide GameInstance and the active level's
// LevelScript. Call setActiveScene() to switch levels (see the <Scene>LevelScript
// files, which self-register their scene names).
#include <IGameLogic.h>
#include "GameLogicRuntime.h"
#include "GameInstance.h"

#include <memory>
#include <string>

namespace
{
// The scene active at startup (the project's startup scene). Drive level changes
// at runtime via GameLogicImpl::setActiveScene.
const char* kStartupScene = ")" + startupScene + R"(";

class GameLogicImpl : public IGameLogic
{
public:
	void onStart(HorizonWorld& world) override
	{
		m_world = &world;
		m_gameInstance.onInit(world);
		setActiveScene(kStartupScene);
	}

	void onUpdate(HorizonWorld& world, float deltaTime) override
	{
		if (m_activeLevel) m_activeLevel->onUpdate(world, deltaTime);
	}

	void onStop(HorizonWorld& world) override
	{
		if (m_activeLevel) { m_activeLevel->onLevelUnloaded(world); m_activeLevel.reset(); }
		m_gameInstance.onShutdown(world);
		m_world = nullptr;
	}

	// Swap the active level script: unload the current one, load the named one.
	void setActiveScene(const std::string& sceneName)
	{
		if (!m_world) return;
		if (m_activeLevel) m_activeLevel->onLevelUnloaded(*m_world);
		m_activeLevel = createLevelScript(sceneName);
		if (m_activeLevel) m_activeLevel->onLevelLoaded(*m_world);
	}

private:
	GameInstance                 m_gameInstance;
	std::unique_ptr<LevelScript> m_activeLevel;
	HorizonWorld*                m_world = nullptr;
};
} // namespace

extern "C" HE_GAME_API IGameLogic* HE_CreateGameLogic()
{
	return new GameLogicImpl();
}

extern "C" HE_GAME_API void HE_DestroyGameLogic(IGameLogic* logic)
{
	delete logic;
}
)";
	return s;
}

std::string tmplLevelScriptHeader(const std::string& className, const std::string& sceneName)
{
	return
"#pragma once\n"
"#include \"GameLogicRuntime.h\"\n"
"\n"
"// Level script for the \"" + sceneName + "\" scene. One is generated per scene;\n"
"// the active scene's script is created and driven by GameLogic.\n"
"class " + className + " : public LevelScript\n"
"{\n"
"public:\n"
"\tvoid onLevelLoaded(HorizonWorld& world) override;\n"
"\tvoid onLevelUnloaded(HorizonWorld& world) override;\n"
"\tvoid onUpdate(HorizonWorld& world, float dt) override;\n"
"};\n";
}

std::string tmplLevelScriptSource(const std::string& className, const std::string& sceneName)
{
	return
"#include \"" + className + ".h\"\n"
"\n"
"// Self-register under the scene's name so GameLogic finds this script for the\n"
"// \"" + sceneName + "\" scene. Adding a new level generates another of these; no\n"
"// other file needs editing.\n"
"REGISTER_LEVEL_SCRIPT(\"" + sceneName + "\", " + className + ");\n"
"\n"
"void " + className + "::onLevelLoaded(HorizonWorld& world)\n"
"{\n"
"\t(void)world;\n"
"\t// Runs when this level finishes loading.\n"
"}\n"
"\n"
"void " + className + "::onLevelUnloaded(HorizonWorld& world)\n"
"{\n"
"\t(void)world;\n"
"\t// Runs just before this level unloads.\n"
"}\n"
"\n"
"void " + className + "::onUpdate(HorizonWorld& world, float dt)\n"
"{\n"
"\t(void)world; (void)dt;\n"
"\t// Runs every frame while this level is active. dt is in seconds.\n"
"}\n";
}

std::string tmplGameplayClassHeader(const std::string& className)
{
	return
"#pragma once\n"
"#include \"GameLogicRuntime.h\"\n"
"\n"
"// A plain gameplay class. Instantiate and drive it from a LevelScript, the\n"
"// GameInstance, or another class of your own.\n"
"class " + className + "\n"
"{\n"
"public:\n"
"\t" + className + "() = default;\n"
"\n"
"\tvoid update(HorizonWorld& world, float dt);\n"
"};\n";
}

std::string tmplGameplayClassSource(const std::string& className)
{
	return
"#include \"" + className + ".h\"\n"
"\n"
"void " + className + "::update(HorizonWorld& world, float dt)\n"
"{\n"
"\t(void)world; (void)dt;\n"
"\t// Your per-frame logic.\n"
"}\n";
}

std::string tmplCMakeLists(const std::string& projectName)
{
	return
"cmake_minimum_required(VERSION 3.16)\n"
"project(" + projectName + "GameLogic LANGUAGES CXX)\n"
"\n"
"set(CMAKE_CXX_STANDARD 17)\n"
"set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
"\n"
"# Point this at your HorizonEngine checkout / SDK — the folder that contains\n"
"# src/HE_Core/include/IGameLogic.h. Override on the command line with\n"
"#   -DHORIZON_ENGINE_DIR=/path/to/HorizonEngine\n"
"if(NOT DEFINED HORIZON_ENGINE_DIR)\n"
"\tset(HORIZON_ENGINE_DIR \"$ENV{HORIZON_ENGINE_DIR}\" CACHE PATH \"HorizonEngine root\")\n"
"endif()\n"
"\n"
"# Every .cpp in this folder compiles into the library — drop in a new file (e.g.\n"
"# from the editor's 'C++ Class' action or a new scene's level script) and re-run\n"
"# CMake; it is picked up automatically.\n"
"file(GLOB GAMELOGIC_SOURCES CONFIGURE_DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp\")\n"
"\n"
"add_library(GameLogic SHARED ${GAMELOGIC_SOURCES})\n"
"\n"
"# The engine loads a library literally named GameLogic.{dll,dylib,so} next to the\n"
"# game executable, so strip any platform 'lib' prefix.\n"
"set_target_properties(GameLogic PROPERTIES PREFIX \"\")\n"
"\n"
"if(HORIZON_ENGINE_DIR)\n"
"\ttarget_include_directories(GameLogic PRIVATE \"${HORIZON_ENGINE_DIR}/src/HE_Core/include\")\n"
"else()\n"
"\tmessage(WARNING\n"
"\t\t\"HORIZON_ENGINE_DIR is not set; <IGameLogic.h> will not be found. \"\n"
"\t\t\"Configure with -DHORIZON_ENGINE_DIR=/path/to/HorizonEngine.\")\n"
"endif()\n";
}

std::string tmplReadme(const std::string& projectName)
{
	return
"# " + projectName + " — C++ GameLogic\n"
"\n"
"This project is authored in native C++. Gameplay lives here in `Source/` and\n"
"compiles into a `GameLogic` shared library the engine loads at runtime.\n"
"\n"
"## Files\n"
"\n"
"- `GameLogicRuntime.h/.cpp` — base classes (`GameInstanceBase`, `LevelScript`) and\n"
"  the level-script registry. Rarely edited.\n"
"- `GameInstance.h/.cpp` — app-wide lifecycle (`onInit` / `onShutdown` /\n"
"  `onWindowFocusChanged`). One per project.\n"
"- `<Scene>LevelScript.h/.cpp` — one per scene, with `onLevelLoaded` /\n"
"  `onLevelUnloaded` / `onUpdate` stubs. Generated automatically when you create a\n"
"  scene in the editor; each self-registers under its scene name.\n"
"- `GameLogic.cpp` — the `IGameLogic` entry point. Drives the GameInstance and the\n"
"  active level's script; call `setActiveScene(\"...\")` to switch levels.\n"
"- `CMakeLists.txt` — globs every `*.cpp` here into the `GameLogic` library.\n"
"\n"
"## Build\n"
"\n"
"```sh\n"
"cd Source\n"
"cmake -B build -DHORIZON_ENGINE_DIR=/path/to/HorizonEngine\n"
"cmake --build build\n"
"```\n"
"\n"
"Copy the resulting `GameLogic.{dll,dylib,so}` next to the exported game\n"
"executable. Adding a new class or scene just adds a `.cpp` here — re-run the\n"
"build and it is compiled and linked in automatically.\n";
}
} // namespace

bool writeCppLevelScript(const std::string& projectRoot, const std::string& sceneName)
{
	const fs::path source = fs::path(projectRoot) / "Source";
	const std::string className = cppIdentifier(sceneName) + "LevelScript";
	const fs::path header = source / (className + ".h");
	const fs::path body   = source / (className + ".cpp");
	// Idempotent: an existing level script for this scene is left untouched.
	if (fs::exists(header) && fs::exists(body)) return true;
	if (!writeTextFileIfAbsent(header, tmplLevelScriptHeader(className, sceneName))) return false;
	if (!writeTextFileIfAbsent(body,   tmplLevelScriptSource(className, sceneName))) return false;
	return true;
}

bool writeCppClass(const std::string& projectRoot, const std::string& className,
                   std::string* outCreatedHeaderPath)
{
	const fs::path source = fs::path(projectRoot) / "Source";
	const std::string base = cppIdentifier(className);
	std::string name = base;
	int counter = 1;
	while (fs::exists(source / (name + ".h")) || fs::exists(source / (name + ".cpp")))
		name = base + std::to_string(counter++);
	const fs::path header = source / (name + ".h");
	const fs::path body   = source / (name + ".cpp");
	if (!writeTextFile(header, tmplGameplayClassHeader(name))) return false;
	if (!writeTextFile(body,   tmplGameplayClassSource(name))) return false;
	if (outCreatedHeaderPath) *outCreatedHeaderPath = header.string();
	return true;
}

bool scaffoldCppProject(const std::string& projectRoot,
                        const std::string& projectName,
                        const std::string& startupSceneName)
{
	const fs::path source = fs::path(projectRoot) / "Source";
	std::error_code ec;
	fs::create_directories(source, ec);

	bool ok = true;
	ok &= writeTextFileIfAbsent(source / "GameLogicRuntime.h",  tmplRuntimeHeader());
	ok &= writeTextFileIfAbsent(source / "GameLogicRuntime.cpp", tmplRuntimeSource());
	ok &= writeTextFileIfAbsent(source / "GameInstance.h",      tmplGameInstanceHeader());
	ok &= writeTextFileIfAbsent(source / "GameInstance.cpp",    tmplGameInstanceSource());
	ok &= writeTextFileIfAbsent(source / "GameLogic.cpp",       tmplGameLogicSource(startupSceneName));
	ok &= writeTextFileIfAbsent(source / "CMakeLists.txt",      tmplCMakeLists(cppIdentifier(projectName)));
	ok &= writeTextFileIfAbsent(source / "README.md",          tmplReadme(projectName));
	ok &= writeCppLevelScript(projectRoot, startupSceneName);
	return ok;
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
	case ProjectPreset::Game:
		fs::create_directories(root / "Content" / "Scripts");
		fs::create_directories(root / "Content" / "Audio");
		fs::create_directories(root / "Content" / "Scenes");
		fs::create_directories(root / "Content" / "Models");
		fs::create_directories(root / "Content" / "Textures");
		fs::create_directories(root / "Content" / "Materials");
		fs::create_directories(root / "Content" / "Prefabs");
		break;
	case ProjectPreset::Simulation:
		fs::create_directories(root / "Content" / "Data");
		break;
	case ProjectPreset::Tool:
		fs::create_directories(root / "Content" / "Source");
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

	// Game/Simulation projects ship with a sky & weather out of the box; Empty/Tool
	// projects start with a bare world (add a Sky via the editor's Environment window).
	const bool seedEnvironment = (preset == ProjectPreset::Game ||
	                              preset == ProjectPreset::Simulation);

	// Write the startup scene JSON. Root entity "World" (id 0, no parent); when seeding,
	// add dedicated "Sky" (EnvironmentComponent) and "Weather" (WeatherComponent) child
	// entities. An empty component object lets the scene loader fill every field from the
	// struct defaults, so this stays decoupled from the components' exact field lists.
	{
		std::ofstream sceneOut(scenePath);
		if (sceneOut.is_open())
		{
			constexpr uint32_t kNull = std::numeric_limits<uint32_t>::max();
			json entities = json::array();

			json rootEntity;
			rootEntity["id"]     = 0;
			rootEntity["name"]   = "World";
			rootEntity["parent"] = kNull;
			json rootChildren = json::array();
			if (seedEnvironment) { rootChildren.push_back(1); rootChildren.push_back(2); }
			rootEntity["children"] = rootChildren;
			entities.push_back(rootEntity);

			if (seedEnvironment)
			{
				json sky;
				sky["id"]         = 1;
				sky["name"]       = "Sky";
				sky["parent"]     = 0;
				sky["children"]   = json::array();
				sky["components"] = { { "environment", json::object() } };
				entities.push_back(sky);

				json weather;
				weather["id"]         = 2;
				weather["name"]       = "Weather";
				weather["parent"]     = 0;
				weather["children"]   = json::array();
				weather["components"] = { { "weather", json::object() } };
				entities.push_back(weather);
			}

			json scene;
			scene["version"]  = "1.1";
			scene["entities"] = entities;

			sceneOut << scene.dump(4);
		}
	}

	json j;
	j["name"]           = projectName;
	j["version"]        = "1.0";
	j["preset"]         = static_cast<int>(preset);
	j["startupScene"]   = "Content/StartupScene.hescene";
	j["scriptLanguage"] = toString(scriptLanguage);

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

	m_currentProject.name                = projectName;
	m_currentProject.path                = heprojPath.string();
	m_currentProject.startupScene        = scenePath.string();
	m_currentProject.exportProfiles      = profiles;
	m_currentProject.activeExportProfile = profiles.front().name;
	m_currentProject.scriptLanguage      = scriptLanguage;
	if (m_onProjectLoaded)
		m_onProjectLoaded(m_currentProject.startupScene);
	return true;
}

bool ProjectManager::loadProject(const std::string& projectPath)
{
	if (!fs::exists(projectPath))
		return false;

	std::ifstream in(projectPath);
	if (!in.is_open())
		return false;

	json j = json::parse(in, nullptr, false);
	if (j.is_discarded())
		return false;

	m_currentProject.name = jsonString(j, "name", fs::path(projectPath).stem().string());
	m_currentProject.path = projectPath;

	// Resolve startup scene relative to the project root
	fs::path projectRoot = fs::path(projectPath).parent_path();
	std::string relScene = jsonString(j, "startupScene");
	if (!relScene.empty())
	{
		fs::path absScene = projectRoot / relScene;
		m_currentProject.startupScene = fs::exists(absScene) ? absScene.string() : "";
	}
	else
	{
		m_currentProject.startupScene = "";
	}

	readProfiles(j, m_currentProject);
	m_currentProject.scriptLanguage =
		projectScriptLanguageFromString(jsonString(j, "scriptLanguage"));

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
		if (!in.is_open()) return false;
		json existing = json::parse(in, nullptr, false);
		if (existing.is_discarded() || !existing.is_object()) return false;
		j = std::move(existing);
	}

	j["name"]    = m_currentProject.name;
	if (!j.contains("version")) j["version"] = "1.0";

	json jp = json::array();
	for (const auto& p : m_currentProject.exportProfiles)
		jp.push_back(profileToJson(p));
	j["exportProfiles"]      = std::move(jp);
	j["activeExportProfile"] = m_currentProject.activeExportProfile;
	j["scriptLanguage"]      = toString(m_currentProject.scriptLanguage);

	// Write temp + rename: an in-place ofstream truncates the only copy before
	// the new content is durable, so disk-full/kill mid-write would leave an
	// empty manifest. rename() swaps atomically on POSIX.
	const std::string tmpPath = projectPath + ".tmp";
	{
		std::ofstream out(tmpPath, std::ios::trunc);
		if (!out.is_open()) return false;
		out << j.dump(4);
		out.flush();
		if (!out.good())
		{
			out.close();
			std::error_code ec;
			fs::remove(tmpPath, ec);
			return false;
		}
	}
	std::error_code ec;
	fs::rename(tmpPath, projectPath, ec);
	if (ec)
	{
		std::error_code ec2;
		fs::remove(tmpPath, ec2);
		return false;
	}
	return true;
}

void ProjectManager::closeProject()
{
	m_currentProject = {};

}
