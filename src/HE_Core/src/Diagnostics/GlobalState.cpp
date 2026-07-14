#include "Diagnostics/GlobalState.h"
#include "Diagnostics/Logger.h"
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <mutex>
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
	// Truncate on each launch so every run starts with a fresh log (one run = one file).
	logFileStream.open(logPath.string(), std::ios::out | std::ios::trunc);
}

std::ofstream& GlobalState::getLogFileStream()
{
	return logFileStream;
}

std::string GlobalState::getDumpsDir() const
{
	// Mirror setLogFile's exe-adjacent derivation: <exeDir>/dumps. startupPath is
	// argv[0] (the executable), so parent_path() is the deploy directory — the same
	// folder that holds HorizonEngine.log.
	fs::path dumps = fs::path(engineStatus.startupPath).parent_path() / "dumps";
	std::error_code ec;
	fs::create_directories(dumps, ec);
	return dumps.string();
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
	// config.json is written on many events (project open, RHI change, …); a crash
	// or kill mid-write, a disk issue, or a hand-edit can leave it truncated or
	// malformed. json::parse would then THROW, and since this runs in the
	// Application constructor with no handler, it aborts the whole editor at
	// startup — a crash-loop, because the bad file is still there next launch.
	// Parse non-throwing and, on a corrupt file, reset to defaults and rewrite a
	// clean config so the next start is clean (mirrors ProjectManager::loadProject).
	json j = json::parse(configFile, nullptr, /*allow_exceptions=*/false);
	if (j.is_discarded() || !j.is_object())
	{
		Logger::Log(Logger::LogLevel::Warning,
			"config.json is corrupt or unreadable — resetting to defaults");
		engineStatus.selectedRHI     = defaultRHI();
		engineStatus.currentOS       = HE::OS::Windows;
		engineStatus.lastProjectPath = "";
		engineStatus.knownProjects.clear();
		customConfig.clear();
		writeConfig();
		return;
	}
	// Type-checked scalar reads: value<>() throws type_error when a key exists with
	// the wrong type, which would defeat the guard above — so never let it throw.
	auto intField = [&](const char* key, int def) -> int
	{
		auto it = j.find(key);
		return (it != j.end() && it->is_number_integer()) ? it->get<int>() : def;
	};
	auto strField = [&](const char* key) -> std::string
	{
		auto it = j.find(key);
		return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
	};
	engineStatus.selectedRHI      = sanitizeRHI(static_cast<HE::GraphicsAPI>(
		intField("RHI", static_cast<int>(defaultRHI()))));
	engineStatus.currentOS        = static_cast<HE::OS>(intField("OS", static_cast<int>(HE::OS::Windows)));
	engineStatus.lastProjectPath  = strField("LastProjectPath");
	engineStatus.knownProjects.clear();
	if (j.contains("KnownProjects") && j["KnownProjects"].is_array())
	{
		for (const auto& entry : j["KnownProjects"])
		{
			if (!entry.is_string()) continue;
			std::string p = entry.get<std::string>();
			// Guard against corrupted entries (e.g. a settings string stored here by mistake)
			if (p.size() >= 7 && p.substr(p.size() - 7) == ".heproj")
				engineStatus.knownProjects.push_back(std::move(p));
		}
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

	// Write temp + atomic rename. A plain in-place ofstream truncates the only copy
	// before the new content is durable, so a crash or kill mid-write (e.g. an abort
	// during shutdown) leaves a half-written config that then crash-loops the next
	// startup. rename() swaps atomically on POSIX and the old config stays intact on
	// any failure. (Same pattern as ProjectManager::saveProject.)
	const std::string tmp = "config.json.tmp";
	{
		std::ofstream out(tmp, std::ios::trunc);
		if (!out.is_open())
		{
			Logger::Log(Logger::LogLevel::Error, "Failed to open config file for writing.");
			return false;
		}
		out << j.dump(4);
		out.flush();
		if (!out.good())
		{
			out.close();
			std::error_code ec;
			fs::remove(tmp, ec);
			Logger::Log(Logger::LogLevel::Error, "Failed to write config file.");
			return false;
		}
	}
	std::error_code ec;
	fs::rename(tmp, "config.json", ec);
	if (ec)
	{
		std::error_code ec2;
		fs::remove(tmp, ec2);
		Logger::Log(Logger::LogLevel::Error, "Failed to commit config file.");
		return false;
	}
	return true;
}

void GlobalState::addKnownProject(const std::string& path)
{
	// Reject anything that isn't a .heproj path (guards against settings strings
	// or other values being accidentally passed here).
	if (path.size() < 7 || path.substr(path.size() - 7) != ".heproj")
		return;
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
		// Hide dotfiles/dotfolders (.gitkeep, .DS_Store, .git, …) — they are
		// VCS/OS bookkeeping, never browsable content. (A ".gitkeep" is how the
		// engine's empty category folders survive in git; it must not surface as
		// a fake asset in the Content Browser.)
		if (entry.path().filename().string().rfind('.', 0) == 0)
			continue;

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
	// Old Folder/File nodes are gone — tell pointer-holders to re-resolve by path.
	contentFolderVersion.fetch_add(1, std::memory_order_release);

	Logger::Log(Logger::LogLevel::Info, "Content folder refreshed.");
	Logger::Log(Logger::LogLevel::Info, ("Number of folders: " + std::to_string(contentFolder.subfolders.size())).c_str());
	Logger::Log(Logger::LogLevel::Info, ("Number of files: " + std::to_string(contentFolder.files.size())).c_str());
	return true;
}

bool GlobalState::refreshSourceFolder()
{
	if (engineStatus.lastProjectPath.empty())
		return false;

	fs::path projectPath = engineStatus.lastProjectPath;
	if (fs::is_regular_file(projectPath))
		projectPath = projectPath.parent_path();

	fs::path sourcePath = projectPath / "Source";

	// Build off-lock. An absent Source/ (non-C++ project, or not scaffolded yet)
	// is not an error — leave the tree empty; the root's fullPath is still set so
	// the browser's drop/create targets resolve.
	Folder fresh;
	fresh.name     = "Source";
	fresh.fullPath = sourcePath.string();
	if (fs::exists(sourcePath) && fs::is_directory(sourcePath))
		populateFolder(&fresh, sourcePath);

	{
		std::unique_lock lock(m_sourceFolderMutex);
		clearFolder(&sourceFolder);
		sourceFolder = std::move(fresh);
	}
	sourceFolderVersion.fetch_add(1, std::memory_order_release);
	return true;
}

// Merges a project's per-asset overrides (<contentRoot>/Engine/<rest>, see
// ContentManager::resolveSavePath) into the displayed default tree: a file
// that already exists in `base` at the same relative position has its
// fullPath swapped to the override's (so opening/loading it picks up the
// override transparently — see ContentManager::resolveAbsolutePath, which
// makes the same override-preferred decision independently); an override
// with no matching default is simply added. Folder nodes that already exist
// keep pointing at the default directory — only LEAF files are ever
// "overridden" here, never the folder's own identity.
static void mergeOverrideInto(Folder* base, const fs::path& overrideDir)
{
	for (const auto& entry : fs::directory_iterator(overrideDir))
	{
		if (entry.path().filename().string().rfind('.', 0) == 0)
			continue; // same dotfile filter as populateFolder

		if (entry.is_directory())
		{
			Folder* sub = nullptr;
			for (Folder* s : base->subfolders)
				if (s->name == entry.path().filename().string()) { sub = s; break; }
			if (!sub)
			{
				sub = new Folder();
				sub->name     = entry.path().filename().string();
				sub->fullPath = entry.path().string();
				base->subfolders.push_back(sub);
			}
			mergeOverrideInto(sub, entry.path());
		}
		else if (entry.is_regular_file())
		{
			File* match = nullptr;
			for (File* f : base->files)
				if (f->name == entry.path().filename().string()) { match = f; break; }
			if (match)
				match->fullPath = entry.path().string(); // override shadows the default
			else
			{
				File* file      = new File();
				file->name      = entry.path().filename().string();
				file->fullPath  = entry.path().string();
				file->extension = entry.path().extension().string();
				base->files.push_back(file);
			}
		}
	}
}

bool GlobalState::refreshEngineFolder(const std::string& engineContentAbsPath,
                                       const std::string& projectContentRoot)
{
	fs::path enginePath = engineContentAbsPath;
	if (!fs::exists(enginePath) || !fs::is_directory(enginePath))
	{
		Logger::Log(Logger::LogLevel::Warning, ("Engine content folder not found at " + enginePath.string()).c_str());
		return false;
	}

	Folder fresh;
	fresh.name     = "Engine";
	fresh.fullPath = enginePath.string();
	populateFolder(&fresh, enginePath);

	// Project-level overrides (Content/Engine/...) merge in on top, so the
	// Content Browser's "Engine" tree shows one unified, effective view —
	// same tree position as the default, just backed by the override file.
	if (!projectContentRoot.empty())
	{
		const fs::path overrideRoot = fs::path(projectContentRoot) / "Engine";
		std::error_code ec;
		if (fs::is_directory(overrideRoot, ec))
			mergeOverrideInto(&fresh, overrideRoot);
	}

	{
		std::unique_lock lock(m_engineFolderMutex);
		clearFolder(&engineFolder);
		engineFolder = std::move(fresh);
	}
	engineFolderVersion.fetch_add(1, std::memory_order_release);

	Logger::Log(Logger::LogLevel::Info, "Engine content folder refreshed.");
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