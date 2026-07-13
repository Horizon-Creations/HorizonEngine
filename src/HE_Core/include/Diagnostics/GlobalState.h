#pragma once
#include "DiagnosticsStructs.h"
#include "../Types/Defines.h"
#include <string>
#include <vector>
#include <fstream>
#include <atomic>
#include <shared_mutex>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class HE_API GlobalState
{
public:
	static GlobalState& getInstance()
	{
		static GlobalState instance;
		return instance;
	}
	// Delete copy constructor and assignment operator to prevent copying
	GlobalState(const GlobalState&) = delete;
	void operator=(const GlobalState&) = delete;

	// Log file management
	void setLogFile(const std::string& path);
	std::ofstream& getLogFileStream();

	// Deploy-adjacent "dumps/" directory (next to HorizonEngine.log), created on
	// demand. Used by the profiler and crash handler for diagnostic output.
	std::string getDumpsDir() const;

	// config
	const HE::GraphicsAPI&            getSelectedRHI()       const { return engineStatus.selectedRHI; }
	const HE::OS&                     getCurrentOS()          const { return engineStatus.currentOS; }
	const std::string&                getLastProjectPath()    const { return engineStatus.lastProjectPath; }
	const std::vector<std::string>&   getKnownProjects()      const { return engineStatus.knownProjects; }
	void setSelectedRHI(HE::GraphicsAPI rhi)                        { engineStatus.selectedRHI = rhi; }
	void setLastProjectPath(const std::string& path)                { engineStatus.lastProjectPath = path; }
	// Adds path to front of known-projects list (deduplicates, max 10)
	void addKnownProject(const std::string& path);
	// Removes a path from the known-projects list
	void removeKnownProject(const std::string& path);

	//Config management
	void readConfig();
	bool writeConfig();
	void setCustomConfigEntry(const std::string& key, const json& value);

	int getCustomConfigInt(const std::string& key, int defaultValue = 0) const;
	float getCustomConfigFloat(const std::string& key, float defaultValue = 0.0f) const;
	bool getCustomConfigBool(const std::string& key, bool defaultValue = false) const;
	std::string getCustomConfigString(const std::string& key, const std::string& defaultValue = "") const;

	bool refreshContentFolder();

	// Bumped by every successful refreshContentFolder(). The refresh deletes and
	// rebuilds every Folder/File node, so ANY Folder*/File* held across frames
	// (e.g. the content browser's navigation statics) dangles afterwards.
	// Consumers compare this against their last-seen value and re-resolve their
	// pointers by path when it changed.
	std::atomic<uint64_t> contentFolderVersion{0};

	// Thread-safe read accessor: hält den shared_lock für die Lebensdauer des zurückgegebenen lock-Objekts.
	// Verwendung: auto [folder, lock] = globalState->lockContentFolder();
	std::pair<const Folder&, std::shared_lock<std::shared_mutex>> lockContentFolder() const
	{
		return { contentFolder, std::shared_lock<std::shared_mutex>(m_contentFolderMutex) };
	}

	// Engine-wide default-content tree (EditorDeps/EngineContent, next to the
	// editor executable — NOT project-specific). Mirrors contentFolder/
	// refreshContentFolder() exactly; the only difference is the root path is
	// passed in directly instead of derived from lastProjectPath, since the
	// engine content root never changes for the life of the process.
	bool refreshEngineFolder(const std::string& engineContentAbsPath);

	std::atomic<uint64_t> engineFolderVersion{0};

	std::pair<const Folder&, std::shared_lock<std::shared_mutex>> lockEngineFolder() const
	{
		return { engineFolder, std::shared_lock<std::shared_mutex>(m_engineFolderMutex) };
	}
private:
	// Private constructor to prevent instantiation
	GlobalState() {}

	//Structs
	EngineStatus engineStatus;
	Folder contentFolder;
	mutable std::shared_mutex m_contentFolderMutex;

	Folder engineFolder;
	mutable std::shared_mutex m_engineFolderMutex;

	//Custom config entries
	json customConfig;

	//logging
	std::ofstream logFileStream;
	std::string logfile;
};