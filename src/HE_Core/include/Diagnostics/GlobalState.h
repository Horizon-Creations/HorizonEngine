#pragma once
#include <cstdint>
#include "DiagnosticsStructs.h"
#include "../Types/Defines.h"
#include <filesystem>
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

	// Opens HorizonEngine.log for HE::Log (rotating the previous runs) and applies
	// the HE_LOG environment overrides. The stream itself lives in HE::Log — see
	// Diagnostics/Log.h. Preferred location is next to the executable, which keeps
	// a portable checkout self-contained; if that directory cannot be written the
	// log moves to the per-user data directory (see diagnosticsDir()).
	void setLogFile(const std::string& path);

	// Directory the diagnostic output of this run goes to — the one setLogFile
	// settled on. Empty before setLogFile has run.
	const std::string& diagnosticsDir() const { return m_diagnosticsDir; }

	// "dumps/" directory next to HorizonEngine.log, created on demand. Used by the
	// profiler and crash handler for diagnostic output.
	std::string getDumpsDir() const;

	// The per-user application-data directory (…/Application Support/HorizonEngine,
	// %APPDATA%\HorizonEngine, $XDG_CONFIG_HOME/HorizonEngine). Empty when the
	// platform gives us no home directory at all. Created if it does not exist.
	static std::filesystem::path userDataDir();

	// Where EngineContent assets fetched on demand from the SFTP manifest (see
	// HE::Cs::EngineContentManifest, HE_ContentSync) land once downloaded. Shared
	// across every project on this machine — NOT per-project and NOT next to the
	// Editor executable (which is often not writable: a signed macOS .app bundle,
	// "Program Files"). A local default/override always wins over this cache; see
	// refreshEngineFolder()'s remoteOnly merge and ContentManager's read-side
	// resolution order.
	static std::filesystem::path engineContentCacheDir();

	// config
	const HE::RendererBackend&        getSelectedRHI()        const { return m_engineStatus.selectedRHI; }
	const std::string&                getLastProjectPath()    const { return m_engineStatus.lastProjectPath; }
	const std::vector<std::string>&   getKnownProjects()      const { return m_engineStatus.knownProjects; }
	void setSelectedRHI(HE::RendererBackend rhi)                    { m_engineStatus.selectedRHI = rhi; }
	void setLastProjectPath(const std::string& path)                { m_engineStatus.lastProjectPath = path; }
	// Adds path to front of known-projects list (deduplicates, max 10)
	void addKnownProject(const std::string& path);
	// Removes a path from the known-projects list
	void removeKnownProject(const std::string& path);

	//Config management
	void readConfig();
	bool writeConfig();

	// A packaged game turns this off. It lays the settings the export shipped
	// next to it over the in-memory config, and configFilePath() resolves to the
	// per-user file that is SHARED with the editor and every other Horizon game
	// on the machine — so the write-back in ~Application would quietly stamp one
	// game's shipped settings onto the developer's editor preferences. The game
	// has nothing of its own to persist today, so refusing the write is the whole
	// fix; a game that later grows a settings menu needs its own file, not this.
	void setConfigPersistent(bool on) { m_configPersistent = on; }
	bool configPersistent() const     { return m_configPersistent; }

	// Where config.json actually lives.
	//
	// It used to be the bare relative name, i.e. resolved against the working
	// directory. That is fine when the editor is started from its build folder
	// and silently broken everywhere else: a macOS .app launched from Finder has
	// a working directory of "/", so every write failed and no setting survived a
	// restart. The path is now a per-user application-data location, which is
	// both writable and the same one across launches however the app was started.
	//
	// A config.json next to the executable still wins if one is there, so a
	// portable/dev checkout keeps behaving as before and nobody's existing
	// settings move out from under them.
	static std::filesystem::path configFilePath();
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
	std::pair<const HE::Folder&, std::shared_lock<std::shared_mutex>> lockContentFolder() const
	{
		return { m_contentFolder, std::shared_lock<std::shared_mutex>(m_contentFolderMutex) };
	}

	// Engine-wide default-content tree (EditorDeps/EngineContent, next to the
	// editor executable — NOT project-specific). Mirrors m_contentFolder/
	// refreshContentFolder() exactly; the only difference is the root path is
	// passed in directly instead of derived from lastProjectPath, since the
	// engine content root never changes for the life of the process.
	// projectContentRoot (optional): the CURRENT project's Content root — any
	// project-level overrides sitting at "<projectContentRoot>/Engine/..."
	// (see ContentManager::resolveSavePath) are merged into the displayed
	// tree, shadowing the default they override. Pass empty to skip merging
	// (e.g. before any project is loaded).
	// The remotely-available EngineContent set (see setEngineRemoteAssets) is
	// merged in as well, so the Content Browser always shows the full catalogue
	// of defaults — including ones not downloaded yet.
	bool refreshEngineFolder(const std::string& engineContentAbsPath,
	                          const std::string& projectContentRoot = std::string());

	// EngineContent assets the SFTP manifest says exist (see
	// HE::Cs::EngineContentManifest, HE_ContentSync). Stored HERE, as tree state,
	// rather than passed to refreshEngineFolder per call — deliberately.
	//
	// It used to be a defaulted third parameter of refreshEngineFolder, which
	// meant every caller that did not know about the feature silently REBUILT THE
	// TREE WITHOUT THE CATALOGUE: the periodic content refresh (default every
	// 60 s), project load, and every create/rename refresh all wiped it, so the
	// remote assets vanished from the browser a minute after startup. A defaulted
	// "and here is all the state you must remember to re-supply" argument is a
	// trap; owning it is not.
	//
	// Callers pass the CURRENT manifest contents; each call replaces the previous
	// set (pass an empty vector to clear it, e.g. when the endpoint is
	// unreachable). Does not itself refresh the tree — call refreshEngineFolder
	// afterwards.
	void setEngineRemoteAssets(std::vector<HE::RemoteEngineAsset> assets);

	std::atomic<uint64_t> engineFolderVersion{0};

	std::pair<const HE::Folder&, std::shared_lock<std::shared_mutex>> lockEngineFolder() const
	{
		return { m_engineFolder, std::shared_lock<std::shared_mutex>(m_engineFolderMutex) };
	}

	// Project-local native C++ source tree (<projectRoot>/Source), a sibling of
	// Content that only C++ projects have. Mirrors m_contentFolder/
	// refreshContentFolder(); the root is derived from lastProjectPath. An absent
	// Source folder yields an empty tree (root fullPath still set) rather than an
	// error, so the browser can simply choose not to show the root.
	bool refreshSourceFolder();

	std::atomic<uint64_t> sourceFolderVersion{0};

	std::pair<const HE::Folder&, std::shared_lock<std::shared_mutex>> lockSourceFolder() const
	{
		return { m_sourceFolder, std::shared_lock<std::shared_mutex>(m_sourceFolderMutex) };
	}
private:
	// Private constructor to prevent instantiation
	GlobalState() {}

	//Structs
	EngineStatus m_engineStatus;
	std::string  m_diagnosticsDir;   // where the log (and dumps/) of this run live
	bool         m_configPersistent = true;   // see setConfigPersistent
	HE::Folder m_contentFolder;
	mutable std::shared_mutex m_contentFolderMutex;

	HE::Folder m_engineFolder;
	mutable std::shared_mutex m_engineFolderMutex;
	// Guarded by m_engineFolderMutex too: it is only ever read while building a
	// fresh tree, which already takes that lock, and written by
	// setEngineRemoteAssets — both off the frame thread (the SFTP worker).
	std::vector<HE::RemoteEngineAsset> m_engineRemoteAssets;

	HE::Folder m_sourceFolder;
	mutable std::shared_mutex m_sourceFolderMutex;

	//Custom config entries
	json m_customConfig;
};