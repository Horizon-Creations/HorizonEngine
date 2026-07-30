#pragma once
#include "HE_TOOLS_API.h"
#include <string>

// ─── C++ scaffold templates ──────────────────────────────────────────────────
// The literal SOURCE a generated C++ project compiles from: ProjectManager's
// scaffolding writers (scaffoldCppProject / writeCppLevelScript / writeCppClass)
// only pick a file name and drop these strings on disk. A stray character in one
// of them produces a project that does not build, so they live in their own
// translation unit — away from the manifest/IO logic that used to bury them.
namespace CppScaffold
{
	// GameLogicRuntime.h/.cpp — the base classes (GameInstanceBase, LevelScript)
	// and the self-registering level-script registry. One pair per project.
	HE_TOOLS_API std::string runtimeHeader();
	HE_TOOLS_API std::string runtimeSource();

	// GameInstance.h/.cpp — the app-wide lifecycle class. One pair per project.
	HE_TOOLS_API std::string gameInstanceHeader();
	HE_TOOLS_API std::string gameInstanceSource();

	// GameLogic.cpp — the IGameLogic entry point, with the project's startup
	// scene name baked in as kStartupScene.
	HE_TOOLS_API std::string gameLogicSource(const std::string& startupScene);

	// <Scene>LevelScript.h/.cpp — one pair per scene. `className` is the
	// cppIdentifier() form of the scene name; `sceneName` is the scene's REAL
	// name, which is what REGISTER_LEVEL_SCRIPT keys the script under.
	HE_TOOLS_API std::string levelScriptHeader(const std::string& className,
	                                           const std::string& sceneName);
	HE_TOOLS_API std::string levelScriptSource(const std::string& className,
	                                           const std::string& sceneName);

	// <Class>.h/.cpp — a plain gameplay-class stub.
	HE_TOOLS_API std::string gameplayClassHeader(const std::string& className);
	HE_TOOLS_API std::string gameplayClassSource(const std::string& className);

	// Source/CMakeLists.txt (globs every *.cpp into the GameLogic library) and
	// Source/README.md.
	HE_TOOLS_API std::string cmakeLists(const std::string& projectName);
	HE_TOOLS_API std::string readme(const std::string& projectName);
}
