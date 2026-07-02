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
	return j;
}

static ExportProfile profileFromJson(const json& j)
{
	ExportProfile p;
	p.name             = j.value("name", std::string{});
	p.compress         = j.value("compress", true);
	p.encrypt          = j.value("encrypt", false);
	p.enableModSupport = j.value("enableModSupport", false);
	p.startupScene     = j.value("startupScene", std::string{});
	p.outputDir        = j.value("outputDir", std::string{});
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
			ExportProfile p = profileFromJson(e);
			if (!p.name.empty()) data.exportProfiles.push_back(std::move(p));
		}
	if (data.exportProfiles.empty())
		data.exportProfiles = defaultExportProfiles();

	data.activeExportProfile = j.value("activeExportProfile", std::string{});
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

	m_currentProject.name = j.value("name", fs::path(projectPath).stem().string());
	m_currentProject.path = projectPath;

	// Resolve startup scene relative to the project root
	fs::path projectRoot = fs::path(projectPath).parent_path();
	std::string relScene = j.value("startupScene", "");
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
	if (std::ifstream in(projectPath); in.is_open())
	{
		json existing = json::parse(in, nullptr, false);
		if (!existing.is_discarded() && existing.is_object())
			j = std::move(existing);
	}

	j["name"]    = m_currentProject.name;
	if (!j.contains("version")) j["version"] = "1.0";

	json jp = json::array();
	for (const auto& p : m_currentProject.exportProfiles)
		jp.push_back(profileToJson(p));
	j["exportProfiles"]      = std::move(jp);
	j["activeExportProfile"] = m_currentProject.activeExportProfile;

	std::ofstream out(projectPath);
	if (!out.is_open())
		return false;
	out << j.dump(4);
	return true;
}

void ProjectManager::closeProject()
{
	m_currentProject = {};

}
