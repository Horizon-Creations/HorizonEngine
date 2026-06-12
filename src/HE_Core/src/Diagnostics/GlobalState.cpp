#include "Diagnostics/GlobalState.h"
#include "Diagnostics/Logger.h"
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <shared_mutex>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

// Clamp a (possibly stale, possibly from another OS) RHI choice to one that
// can actually be created on this platform.
static HE::GraphicsAPI sanitizeRHI(HE::GraphicsAPI rhi)
{
#ifdef __APPLE__
	if (rhi == HE::GraphicsAPI::D3D11 || rhi == HE::GraphicsAPI::D3D12)
		return HE::GraphicsAPI::Metal;
#else
	if (rhi == HE::GraphicsAPI::Metal)
		return HE::GraphicsAPI::OpenGL;
#ifndef _WIN32
	if (rhi == HE::GraphicsAPI::D3D11 || rhi == HE::GraphicsAPI::D3D12)
		return HE::GraphicsAPI::OpenGL;
#endif
#endif
	return rhi;
}

static HE::GraphicsAPI defaultRHI()
{
#ifdef __APPLE__
	return HE::GraphicsAPI::Metal;
#else
	return HE::GraphicsAPI::OpenGL;
#endif
}

void GlobalState::setLogFile(const std::string& exePath)
{
	engineStatus.startupPath = exePath;
	// Derive log file next to the exe: <exeDir>/HorizonEngine.log
	fs::path logPath =
		fs::path(exePath).parent_path() / "HorizonEngine.log";
	if (fs::exists(logPath))
	{
		fs::remove(logPath);
	}
	logFileStream.open(logPath.string(), std::ios::out | std::ios::app);
}

std::ofstream& GlobalState::getLogFileStream()
{
	return logFileStream;
}

void GlobalState::readConfig()
{
	if (!fs::exists("config.json"))
	{
		Logger::Log(Logger::LogLevel::Warning, "No config file found — using defaults");
		engineStatus.selectedRHI = defaultRHI();
		engineStatus.currentOS = HE::OS::Windows;
		engineStatus.lastProjectPath = "";
		engineStatus.knownProjects.clear();
		writeConfig();
		return;
	}
	std::ifstream configFile("config.json");
	if (!configFile.is_open())
	{
		Logger::Log(Logger::LogLevel::Error, "Failed to open config file.");
		return;
	}
	json j = json::parse(configFile);
	engineStatus.selectedRHI      = sanitizeRHI(static_cast<HE::GraphicsAPI>(
		j.value("RHI", static_cast<int>(defaultRHI()))));
	engineStatus.currentOS        = static_cast<HE::OS>(j.value("OS",  static_cast<int>(HE::OS::Windows)));
	engineStatus.lastProjectPath  = j.value("LastProjectPath", std::string{});
	engineStatus.knownProjects.clear();
	if (j.contains("KnownProjects") && j["KnownProjects"].is_array())
	{
		for (const auto& entry : j["KnownProjects"])
			if (entry.is_string())
				engineStatus.knownProjects.push_back(entry.get<std::string>());
	}
	if (j.contains("CustomConfig") && j["CustomConfig"].is_array())
	{
		for (const auto& entry : j["CustomConfig"])
		{
			if (entry.contains("Key") && entry["Key"].is_string() &&
				entry.contains("Value"))
			{
				customConfig[entry["Key"].get<std::string>()] = entry["Value"];
			}
		}
	}
}

bool GlobalState::writeConfig()
{
	json j;
	j["RHI"] = engineStatus.selectedRHI;
	j["OS"] = engineStatus.currentOS;
	j["LastProjectPath"] = engineStatus.lastProjectPath;
	j["KnownProjects"] = engineStatus.knownProjects;
	json::array_t customEntries;
	for (const auto& [key, value] : customConfig.items())
	{
		customEntries.push_back({ {"Key", key}, {"Value", value} });
	}
	j["CustomConfig"] = customEntries;

	std::ofstream configFile("config.json");
	if (!configFile.is_open())
	{
		Logger::Log(Logger::LogLevel::Error, "Failed to open config file.");
	}
	configFile << j.dump(4);
	configFile.close();
	return true;
}

void GlobalState::addKnownProject(const std::string& path)
{
	auto& kp = engineStatus.knownProjects;
	// Remove existing occurrence to avoid duplicates
	kp.erase(std::remove(kp.begin(), kp.end(), path), kp.end());
	kp.insert(kp.begin(), path);
	if (kp.size() > 10)
		kp.resize(10);
	engineStatus.lastProjectPath = path;
}

void GlobalState::removeKnownProject(const std::string& path)
{
	auto& kp = engineStatus.knownProjects;
	kp.erase(std::remove(kp.begin(), kp.end(), path), kp.end());
}

static void clearFolder(Folder* folder)
{
	for (Folder* sub : folder->subfolders)
	{
		clearFolder(sub);
		delete sub;
	}
	folder->subfolders.clear();
	for (File* file : folder->files)
		delete file;
	folder->files.clear();
}

static void populateFolder(Folder* folder, const fs::path& path)
{
	for (const auto& entry : fs::directory_iterator(path))
	{
		if (entry.is_directory())
		{
			Folder* sub   = new Folder();
			sub->name     = entry.path().filename().string();
			sub->fullPath = entry.path().string();
			populateFolder(sub, entry.path());
			folder->subfolders.push_back(sub);
		}
		else if (entry.is_regular_file())
		{
			File* file      = new File();
			file->name      = entry.path().filename().string();
			file->fullPath  = entry.path().string();
			file->extension = entry.path().extension().string();
			folder->files.push_back(file);
		}
	}
}

bool GlobalState::refreshContentFolder()
{
	if (engineStatus.lastProjectPath.empty())
	{
		Logger::Log(Logger::LogLevel::Warning, "No project loaded — cannot refresh content folder.");
		return false;
	}

	fs::path projectPath = engineStatus.lastProjectPath;
	if (fs::is_regular_file(projectPath))
		projectPath = projectPath.parent_path();

	fs::path contentFolderpath = projectPath / "Content";
	if (!fs::exists(contentFolderpath) || !fs::is_directory(contentFolderpath))
	{
		Logger::Log(Logger::LogLevel::Error, ("Content folder not found at " + contentFolderpath.string()).c_str());
		return false;
	}

	// Daten ausserhalb des Locks zusammenstellen, dann atomar eintauschen
	Folder fresh;
	fresh.name     = contentFolderpath.filename().string();
	fresh.fullPath = contentFolderpath.string();
	populateFolder(&fresh, contentFolderpath);

	{
		std::unique_lock lock(m_contentFolderMutex);
		clearFolder(&contentFolder);
		contentFolder = std::move(fresh);
	}

	Logger::Log(Logger::LogLevel::Info, "Content folder refreshed.");
	Logger::Log(Logger::LogLevel::Info, ("Number of folders: " + std::to_string(contentFolder.subfolders.size())).c_str());
	Logger::Log(Logger::LogLevel::Info, ("Number of files: " + std::to_string(contentFolder.files.size())).c_str());
	return true;
}

void GlobalState::setCustomConfigEntry(const std::string& key, const json& value)
{
	customConfig[key] = value;
}

int GlobalState::getCustomConfigInt(const std::string& key, int defaultValue) const
{
	if (customConfig.contains(key) && customConfig[key].is_number())
		return customConfig[key].get<int>();
	return defaultValue;
}

float GlobalState::getCustomConfigFloat(const std::string& key, float defaultValue) const
{
	if (customConfig.contains(key) && customConfig[key].is_number())
		return customConfig[key].get<float>();
	return defaultValue;
}

bool GlobalState::getCustomConfigBool(const std::string& key, bool defaultValue) const
{
	if (customConfig.contains(key) && customConfig[key].is_boolean())
		return customConfig[key].get<bool>();
	return defaultValue;
}

std::string GlobalState::getCustomConfigString(const std::string& key, const std::string& defaultValue) const
{
	if (customConfig.contains(key) && customConfig[key].is_string())
		return customConfig[key].get<std::string>();
	return defaultValue;
}