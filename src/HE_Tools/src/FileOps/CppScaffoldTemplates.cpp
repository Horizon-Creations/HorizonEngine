#include "CppScaffoldTemplates.h"

namespace CppScaffold
{
// ── Templates ─────────────────────────────────────────────────────────────────
// Every generated method is an empty stub, so the whole tree compiles against
// only <IGameLogic.h> before the user writes any gameplay code.

std::string runtimeHeader()
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

std::string runtimeSource()
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

std::string gameInstanceHeader()
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

std::string gameInstanceSource()
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

std::string gameLogicSource(const std::string& startupScene)
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

std::string levelScriptHeader(const std::string& className, const std::string& sceneName)
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

std::string levelScriptSource(const std::string& className, const std::string& sceneName)
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

std::string gameplayClassHeader(const std::string& className)
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

std::string gameplayClassSource(const std::string& className)
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

std::string cmakeLists(const std::string& projectName)
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

std::string readme(const std::string& projectName)
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
} // namespace CppScaffold
