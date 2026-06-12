#include "ProjectManager.h"
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json   = nlohmann::json;

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

	fs::path heprojPath = root / (projectName + ".heproj");
	std::ofstream out(heprojPath);
	if (!out.is_open())
		return false;
	out << j.dump(4);
	out.close();

	m_currentProject.name         = projectName;
	m_currentProject.path         = heprojPath.string();
	m_currentProject.startupScene = scenePath.string();
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

	if (m_onProjectLoaded)
		m_onProjectLoaded(m_currentProject.startupScene);
	return true;
}

bool ProjectManager::saveProject(const std::string& projectPath)
{
	json j;
	j["name"]    = m_currentProject.name;
	j["version"] = "1.0";

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
