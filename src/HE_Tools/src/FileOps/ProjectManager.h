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
	bool createNewProject(const std::string& projectDir,
						  const std::string& projectName,
						  ProjectPreset preset = ProjectPreset::Empty);

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
