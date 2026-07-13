#pragma once
#include "HE_TOOLS_API.h"
#include <string>
#include <vector>
#include <functional>

enum class ProjectPreset
{
	Empty,       // only folder skeleton, no extra content
	Game,        // Assets, Scenes, Scripts sub-folders + sample scene
	Simulation,  // Assets, Scenes, Data sub-folders
	Tool,        // Assets, Source sub-folders
};

// The gameplay scripting language a project is authored in, chosen at creation
// and persisted in the .heproj manifest ("scriptLanguage"). Lua and Python map
// onto the IScriptBackend script routing, HorizonCode onto the visual graphs,
// Cpp onto a native GameLogic library. This is a HARD restriction: the editor
// only offers the matching logic-authoring assets (the Content Browser hides the
// other languages' creators), and a Cpp project is scaffolded with a compilable
// Source/ tree instead of engine assets.
enum class ProjectScriptLanguage
{
	HorizonCode, // default: visual scripting graphs
	Lua,
	Python,
	Cpp,
};

// Manifest spelling of the language ("HorizonCode"/"Lua"/"Python"/"Cpp") and
// its tolerant inverse (unknown/missing → HorizonCode).
HE_TOOLS_API const char*           toString(ProjectScriptLanguage lang);
HE_TOOLS_API ProjectScriptLanguage projectScriptLanguageFromString(const std::string& s);

// ─── Native C++ gameplay scaffolding ─────────────────────────────────────────
// A Cpp project authors gameplay as a native GameLogic shared library instead of
// engine assets. These generators emit self-contained C++ source (under the
// project's Source/ folder) that compiles against only <IGameLogic.h> — every
// generated method is an empty stub, so the tree builds before the user writes a
// line. The CMakeLists globs Source/*.cpp into a GameLogic library, and per-level
// scripts self-register by scene name, so adding a class or a level needs no
// hand-editing of the existing files. projectRoot is the project folder (the
// parent of the .heproj), not the Content folder.

// Turn an arbitrary asset/scene name into a valid C++ identifier (leading digit
// prefixed with '_', non-alnum → '_'); empty → "Unnamed".
HE_TOOLS_API std::string cppIdentifier(const std::string& name);

// Create the whole Source/ tree for a freshly created Cpp project: the runtime
// header/impl, a GameInstance class, a LevelScript for the startup scene, the
// IGameLogic entry point, a CMakeLists and a README. Existing files are left
// untouched. Returns false only on a filesystem failure.
HE_TOOLS_API bool scaffoldCppProject(const std::string& projectRoot,
                                     const std::string& projectName,
                                     const std::string& startupSceneName);

// Emit Source/<Scene>LevelScript.{h,cpp} with the level event stubs
// (OnLevelLoaded / OnLevelUnloaded / OnUpdate) and a REGISTER_LEVEL_SCRIPT for
// the scene. Called when a scene is created in a Cpp project. No-op (returns
// true) if the files already exist. Returns false only on a write failure.
HE_TOOLS_API bool writeCppLevelScript(const std::string& projectRoot,
                                      const std::string& sceneName);

// Emit Source/<Class>.{h,cpp} — a plain gameplay-class stub. Auto-uniquifies the
// file name if it exists. On success, outCreatedPath (when non-null) receives the
// absolute path of the generated header. Returns false only on a write failure.
HE_TOOLS_API bool writeCppClass(const std::string& projectRoot,
                                const std::string& className,
                                std::string* outCreatedHeaderPath = nullptr);

// A named, persisted packaging preset (Build > Export Project). Stored in the
// .heproj manifest so export settings survive editor restarts. The editor maps
// the selected profile onto ExportSettings (HE_Core) when exporting.
struct ExportProfile
{
	std::string name;
	bool        compress         = true;
	bool        encrypt          = false;
	bool        enableModSupport = false;
	// Project-relative path of the .hescene to ship as startup scene.
	// Empty = the scene currently open in the editor.
	std::string startupScene;
	// Export output directory. Empty = <ProjectRoot>/Export/<profile name>.
	std::string outputDir;
	// Glob patterns (Content-relative, forward slashes) excluded from the pak,
	// e.g. "Debug/*", "*_test.hasset".
	std::vector<std::string> excludePatterns;
	// Incremental packing: reuse unchanged assets from the previous export
	// (manifest-gated; falls back to a full pack automatically).
	bool incremental = true;
	// macOS: emit a .app bundle instead of a flat folder (only applied when the
	// target produces macOS binaries; ignored otherwise).
	bool appBundle = false;
	// Export target: "Host" (this machine, runtime from ../Game) or
	// "Windows"/"macOS"/"Linux" (prebuilt bundle from ../GameRuntimes/<name>,
	// output lands in a per-platform sub-folder).
	std::string targetPlatform = "Host";
	// Precompile node-graph material shaders into the pak for these graphics backends
	// (bitmask of 1u << HE::RendererBackend). Default = Metal | OpenGL (the runtime-
	// consumed backends). 0 → shaders cross-compile at runtime as before.
	uint32_t shaderBackends = (1u << 4) | (1u << 0); // Metal | OpenGL
	// Compile HorizonCode graphs to native C++ in the packaged build. NOT
	// IMPLEMENTED YET (see docs/horizoncode-cpp-codegen-plan.md) — the toggle is
	// plumbed so profiles can opt in ahead of time; today it logs a notice and
	// the export ships the interpreter + the HorizonCode assets as usual.
	bool compileHorizonCode = false;
};

// The two seeded defaults for projects that have no profiles yet (also used by
// tests): "Development" (uncompressed, mods on) and "Shipping" (zstd + AES).
HE_TOOLS_API std::vector<ExportProfile> defaultExportProfiles();

struct ProjectData
{
	std::string name;
	std::string path;
	std::string startupScene; // absolute path to the startup .hescene file (empty = none)

	std::vector<ExportProfile> exportProfiles;      // never empty after load/create
	std::string                activeExportProfile; // name of the last-used profile

	// Primary gameplay scripting language (chosen in the new-project wizard).
	ProjectScriptLanguage scriptLanguage = ProjectScriptLanguage::HorizonCode;
};

class HE_TOOLS_API ProjectManager
{
public:
	ProjectManager() = default;
	~ProjectManager() = default;

	// Creates the folder structure, writes a minimal .heproj file.
	// projectDir  – absolute path to the new project root folder
	// projectName – display name (also used as .heproj filename)
	// preset      – which folder template to apply
	// scriptLanguage – the project's primary gameplay scripting language
	bool createNewProject(const std::string& projectDir,
						  const std::string& projectName,
						  ProjectPreset preset = ProjectPreset::Empty,
						  ProjectScriptLanguage scriptLanguage = ProjectScriptLanguage::HorizonCode);

	bool loadProject(const std::string& projectPath);
	bool saveProject(const std::string& projectPath);
	void closeProject();

	ProjectData& currentProject() { return m_currentProject; }

	// Called after a project is successfully loaded or created.
	// Receives the absolute path to the startup scene (empty string if none).
	// Use this to initialise world / scene state without coupling HE_Tools to HorizonScene.
	void setOnProjectLoaded(std::function<void(const std::string&)> callback) { m_onProjectLoaded = std::move(callback); }
private:
	ProjectData m_currentProject;
	std::function<void(const std::string&)> m_onProjectLoaded;
};
