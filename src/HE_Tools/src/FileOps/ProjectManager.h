#pragma once
#include <cstdint>
#include "HE_TOOLS_API.h"
#include <string>
#include <vector>
#include <functional>

// Persisted as an int in the .heproj manifest ("preset") — only ever append.
enum class ProjectPreset
{
	Empty,       // only folder skeleton, no extra content
	Game,        // Assets, Scenes, Scripts sub-folders + sample scene
	Simulation,  // Assets, Scenes, Data sub-folders
	Tool,        // Assets, Source sub-folders
	Tutorial,    // Game skeleton + a furnished sandbox scene for the guided tour
	// A project that can be played the moment it opens: ground, light, sky, and a
	// PlayerController + PlayerCharacter already wired to WASD, the mouse and
	// Space. Every other preset is a folder skeleton, and that is what made the
	// first hour of this engine a scavenger hunt — the pieces a player is made of
	// are five assets in four places, and nothing said so.
	//
	// HorizonCode only: PlayerHost discovers controllers by scanning HorizonCode
	// class assets, so the two graphs this writes ARE the mechanism. See
	// createNewProject, which forces the language rather than letting the Hub
	// hand out a template that cannot work.
	ThirdPerson,
	// An APPLICATION rather than a game (docs/he-apps-plan.md E1): UI, textures
	// and fonts, and deliberately NO startup scene — there is no world to put one
	// in. Choosing it is what sets ProjectData::appProject, so the two can never
	// disagree.
	//
	// AFTER ThirdPerson, not before it: both branches appended a row at the same
	// number, and this file says "only ever append" because the value is written
	// into the .heproj as a plain int. Main is the trunk and had already handed
	// that number to ThirdPerson, so the one that had not shipped is the one that
	// moves. Nobody's project changes template underneath them.
	Application,
	// Not a template: the number of them. The editor's picker is an array of
	// names indexed by this enum, and the two drifting apart is how choosing
	// "Application" silently created an Empty project.
	COUNT,
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

// `toString` and `cppIdentifier` are namespaced and the surrounding types are
// not, on purpose: an exported function called `toString` sitting in the global
// namespace is a hazard out of all proportion to its size — it is a candidate
// for every unqualified `toString(x)` in every translation unit that reaches
// this header. `ProjectManager`/`ProjectData`/`ExportProfile` are specific
// enough to stay put for now. See docs/coding-conventions.md §1.
namespace HE::tools
{
	// Manifest spelling of the language ("HorizonCode"/"Lua"/"Python"/"Cpp") and
	// its tolerant inverse (unknown/missing → HorizonCode). The returned strings
	// are the persisted .heproj "scriptLanguage" values — do not restyle them.
	HE_TOOLS_API const char*           toString(ProjectScriptLanguage lang);
	HE_TOOLS_API ProjectScriptLanguage projectScriptLanguageFromString(const std::string& s);
}

// ─── Native C++ gameplay scaffolding ─────────────────────────────────────────
// A Cpp project authors gameplay as a native GameLogic shared library instead of
// engine assets. These generators emit self-contained C++ source (under the
// project's Source/ folder) that compiles against only <IGameLogic.h> — every
// generated method is an empty stub, so the tree builds before the user writes a
// line. The CMakeLists globs Source/*.cpp into a GameLogic library, and per-level
// scripts self-register by scene name, so adding a class or a level needs no
// hand-editing of the existing files. projectRoot is the project folder (the
// parent of the .heproj), not the Content folder.

namespace HE::tools
{
	// Turn an arbitrary asset/scene name into a valid C++ identifier (leading
	// digit prefixed with '_', non-alnum → '_'); empty → "Unnamed".
	HE_TOOLS_API std::string cppIdentifier(const std::string& name);
}

// Create the whole Source/ tree for a freshly created Cpp project: the runtime
// header/impl, a GameInstance class, a LevelScript for the startup scene, the
// IGameLogic entry point, a CMakeLists and a README. Existing files are left
// untouched. Returns false only on a filesystem failure.
HE_TOOLS_API bool scaffoldCppProject(const std::string& projectRoot,
                                     const std::string& projectName,
                                     const std::string& startupSceneName);

// ─── Tutorial sandbox ────────────────────────────────────────────────────────
// Lay down the extra files a ProjectPreset::Tutorial project gets on top of the
// normal skeleton: a TUTORIAL.md that explains what the sandbox is for and how to
// reopen the guided tour. The furnished starter scene itself is part of the
// startup-scene JSON (see startupSceneJson in the .cpp), because every preset
// writes that same file. Existing files are left untouched, so re-running this on
// an existing project is safe. Returns false only on a write failure.
HE_TOOLS_API bool scaffoldTutorialProject(const std::string& projectRoot,
                                          const std::string& projectName);

// ─── Third-person starter ────────────────────────────────────────────────────
// The five assets a ProjectPreset::ThirdPerson project needs in order to be
// playable on first open, written straight as .hasset containers:
//
//   Content/Gameplay/PlayerController.hasset  Begin Play → Create Object → Possess
//   Content/Gameplay/PlayerCharacter.hasset   Move on the Move axis, Jump on Jump
//   Content/Input/Move.hasset                 Axis2D
//   Content/Input/Look.hasset                 Axis2D
//   Content/Input/Jump.hasset                 Button
//   Content/Input/DefaultMappings.hasset      WASD + mouse + Space + gamepad
//
// Neither class ships a component list. It does not need one: a class with no
// components of its own inherits its engine base's (EntityHost::
// inheritedComponents), which is where a PlayerCharacter's character controller,
// collider, movement component and camera child come from. Writing a blob here
// would freeze today's component set into every project ever made from this
// template.
//
// Existing files are left untouched, so re-running this on an existing project
// is safe. Returns false only on a write failure.
HE_TOOLS_API bool scaffoldThirdPersonProject(const std::string& projectRoot);

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
	// Compile HorizonCode graphs to native C++ in the packaged build (needs
	// cmake + a C++ toolchain on the exporting machine).
	bool compileHorizonCode = false;
	// What a graph that cannot be compiled means. false (default) = ship it
	// interpreted, so compiling is an optimization and never a gate. true = fail
	// the export instead, for a build in which every class really is native —
	// which is also what makes the direct cross-class call paths always hit.
	bool hcStopOnFailure = false;
};

// The two seeded defaults for projects that have no profiles yet (also used by
// tests): "Development" (uncompressed, mods on) and "Shipping" (zstd + AES).
HE_TOOLS_API std::vector<ExportProfile> defaultExportProfiles();

struct ProjectData
{
	std::string name;
	std::string path;
	// Stable project identity, persisted as the manifest's "id" and minted the
	// first time a project is created or loaded without one.
	//
	// The name cannot serve this purpose: two people routinely have differently
	// named copies of the same project, and identically named copies of
	// different ones. Collaboration compares this — joining a session whose host
	// has a DIFFERENT project open used to transfer the scene happily and leave
	// the joiner with a content browser full of unresolvable asset references,
	// because every uuid in that scene names something in someone else's project.
	std::string id;
	std::string startupScene; // absolute path to the startup .hescene file (empty = none)

	std::vector<ExportProfile> exportProfiles;      // never empty after load/create
	std::string                activeExportProfile; // name of the last-used profile

	// Primary gameplay scripting language (chosen in the new-project wizard).
	ProjectScriptLanguage scriptLanguage = ProjectScriptLanguage::HorizonCode;

	// Content-relative path of the SaveGameTemplate asset save.create() uses
	// (".heproj \"defaultSaveTemplate\""). Empty = the project's single template
	// if exactly one exists, else create() fails loud.
	std::string defaultSaveTemplate;

	// ── The theme this project's interface uses (docs/he-apps-plan.md D1) ────
	// Content-relative path of a Theme asset (empty = the engine's built-in
	// default) and the mode that was ASKED for ("System"/"Light"/"Dark"; empty
	// reads as System). Both travel into project.hcfg on export, so a shipped
	// application boots looking the way it was authored.
	std::string theme;
	std::string themeMode;

	// ── Application projects (docs/he-apps-plan.md A0/A1/E1b) ────────────────
	// appProject: this is an APPLICATION, not a game. No scene, no world, no
	// physics; the packaged build gets ProjectConfig::appMode and draws only when
	// something changed. Persisted as ".heproj \"appProject\"".
	bool appProject = false;
	// advancedShaderEffects: may this project author MATERIALS (the node graphs
	// that give a widget a custom shader, "Schicht 1" in the plan)? Off is the
	// same kind of HARD restriction as scriptLanguage above: the Content Browser
	// hides the material creators, the material editor stays shut, widgets offer
	// no material slot, and the packaged runtime can be built without a shader
	// compiler. Default TRUE, because every project that predates the flag has
	// them and a game always does.
	bool advancedShaderEffects = true;

	// ── What a script in this project may reach outside it (plan, Block C) ───
	// Three doors, all shut unless this project opens them, persisted as
	// ".heproj \"allowFiles\"/\"allowProcesses\"/\"allowNetwork\"" and carried
	// into the packaged build's project.hcfg.
	//
	// The plan calls the permission model a one-way street and says to settle it
	// before unlocking anything, so it is settled here: the permission is about
	// what a SCRIPT may name on its own, never about what a PERSON may choose.
	// A path picked in a file dialog is granted for that session whatever
	// allowFiles says — the choosing IS the permission. allowFiles is the
	// blanket that lets a script name an absolute path with nobody having picked
	// it.
	//
	// The EDITOR is gated by the same three as the shipped app, deliberately.
	// A preview that may delete a stranger's directory while the export may not
	// is the worse of the two failures: it happens on the author's machine,
	// before anything could have shipped.
	bool allowFiles     = false;
	bool allowProcesses = false;
	bool allowNetwork   = false;   // reserved for `http` (Welle 3)
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
	// appProject / advancedShaderEffects – see ProjectData. Defaulted so every
	// existing call site keeps creating exactly the game it created before.
	bool createNewProject(const std::string& projectDir,
						  const std::string& projectName,
						  ProjectPreset preset = ProjectPreset::Empty,
						  ProjectScriptLanguage scriptLanguage = ProjectScriptLanguage::HorizonCode,
						  bool appProject = false,
						  bool advancedShaderEffects = true);

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
