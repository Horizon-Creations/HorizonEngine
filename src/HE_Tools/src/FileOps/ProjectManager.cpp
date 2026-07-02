#include "ProjectManager.h"
#include <algorithm>
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

bool ProjectManager::createNewProject(const std::string& projectDir,
									  const std::string& projectName,
									  ProjectPreset preset)
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

	// ── Write .heproj manifest ─────────────────────────────────────────────────
	// Default startup scene: Content/StartupScene.hescene
	fs::path scenePath = root / "Content" / "StartupScene.hescene";

	// Write a minimal empty scene JSON (root entity "World", no children)
	{
		std::ofstream sceneOut(scenePath);
		if (sceneOut.is_open())
		{
			// Root entity — id 0, no parent (entt::null = UINT32_MAX), no children
			json rootEntity;
			rootEntity["id"]       = 0;
			rootEntity["name"]     = "World";
			rootEntity["parent"]   = std::numeric_limits<uint32_t>::max();
			rootEntity["children"] = json::array();

			json scene;
			scene["version"]  = "1.0";
			scene["entities"] = json::array({ rootEntity });

			sceneOut << scene.dump(4);
		}
	}

	json j;
	j["name"]         = projectName;
	j["version"]      = "1.0";
	j["preset"]       = static_cast<int>(preset);
	j["startupScene"] = "Content/StartupScene.hescene";

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

	m_currentProject.name                = projectName;
	m_currentProject.path                = heprojPath.string();
	m_currentProject.startupScene        = scenePath.string();
	m_currentProject.exportProfiles      = profiles;
	m_currentProject.activeExportProfile = profiles.front().name;
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
