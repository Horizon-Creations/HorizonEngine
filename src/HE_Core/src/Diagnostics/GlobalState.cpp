#include "Diagnostics/GlobalState.h"
#include "Diagnostics/Log.h"
#include "Diagnostics/Logger.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

// Clamp a (possibly stale, possibly from another OS) RHI choice to one that
// can actually be created on this platform.
static HE::RendererBackend sanitizeRHI(HE::RendererBackend rhi)
{
#ifdef __APPLE__
	if (rhi == HE::RendererBackend::D3D11 || rhi == HE::RendererBackend::D3D12)
		return HE::RendererBackend::Metal;
#else
	if (rhi == HE::RendererBackend::Metal)
		return HE::RendererBackend::OpenGL;
#ifndef _WIN32
	if (rhi == HE::RendererBackend::D3D11 || rhi == HE::RendererBackend::D3D12)
		return HE::RendererBackend::OpenGL;
#endif
#endif
	return rhi;
}

static HE::RendererBackend defaultRHI()
{
#ifdef __APPLE__
	return HE::RendererBackend::Metal;
#else
	return HE::RendererBackend::OpenGL;
#endif
}

void GlobalState::setLogFile(const std::string& exePath)
{
	m_engineStatus.startupPath = exePath;

	// Preferred location is next to the exe, which keeps a portable checkout or a
	// zipped build self-contained. That directory is not always writable though: a
	// .app running off a mounted DMG sits on a read-only volume, and one installed
	// in /Applications is code-signed — writing a log INTO the bundle invalidates
	// the signature. Both used to end with no log file at all and no hint why, so
	// an installed build had nothing to attach to a bug report.
	const fs::path exeDir = fs::path(exePath).parent_path();

	// One run = one file, but the three previous runs are rotated to
	// HorizonEngine.1/2/3.log rather than dropped: after a crash the interesting
	// log is the one from BEFORE the restart, and truncating on launch used to
	// destroy it before anyone could look.
	auto openIn = [](const fs::path& dir) {
		return !dir.empty()
		    && HE::Log::openLogFile((dir / "HorizonEngine.log").string(), /*keepBackups=*/3);
	};

	if (openIn(exeDir))
		m_diagnosticsDir = exeDir.string();
	else
	{
		// Same per-user directory config.json uses, so all writable state of a run
		// ends up in one place.
		const fs::path userDir = userDataDir();
		m_diagnosticsDir = openIn(userDir) ? userDir.string() : std::string();
	}

	// Verbosity comes from (lowest to highest priority) the built-in default,
	// the "logVerbosity" config entry, and the HE_LOG environment variable, so a
	// user can crank up one subsystem for a single run without editing anything:
	//     HE_LOG=Physics=Trace,RHI=Debug ./HorizonEngine
	if (const char* env = std::getenv("HE_LOG"))
		HE::Log::configureFromString(env);
	if (std::getenv("HE_LOG_NO_CONSOLE"))
		HE::Log::setConsoleEnabled(false);

	// After the sinks are configured, so this lands in the file it is describing.
	if (m_diagnosticsDir.empty())
		HE_LOG_WARN(Core, "No writable log location (tried %s and the per-user data "
		                  "directory) — this run logs to the console only",
		            exeDir.string().c_str());
	else if (m_diagnosticsDir != exeDir.string())
		HE_LOG_INFO(Core, "Log file: %s/HorizonEngine.log (%s is not writable)",
		            m_diagnosticsDir.c_str(), exeDir.string().c_str());
}

std::string GlobalState::getDumpsDir() const
{
	// Next to HorizonEngine.log, wherever setLogFile settled on — so a read-only
	// exe directory moves crash dumps and profiler captures along with the log
	// instead of dropping them. Before setLogFile has run (or with no writable
	// location at all) fall back to the exe-adjacent path: a caller needs a
	// directory to name even when writing there will fail.
	const fs::path base = m_diagnosticsDir.empty()
	                    ? fs::path(m_engineStatus.startupPath).parent_path()
	                    : fs::path(m_diagnosticsDir);
	fs::path dumps = base / "dumps";
	std::error_code ec;
	fs::create_directories(dumps, ec);
	return dumps.string();
}

// ─── Where per-user state lives ──────────────────────────────────────────────
std::filesystem::path GlobalState::userDataDir()
{
	static const fs::path resolved = [] {
		std::error_code ec;
		fs::path dir;
#if defined(_WIN32)
		if (const char* appdata = std::getenv("APPDATA")) dir = fs::path(appdata) / "HorizonEngine";
#elif defined(__APPLE__)
		if (const char* home = std::getenv("HOME"))
			dir = fs::path(home) / "Library" / "Application Support" / "HorizonEngine";
#else
		if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) dir = fs::path(xdg) / "HorizonEngine";
		else if (const char* home = std::getenv("HOME"))      dir = fs::path(home) / ".config" / "HorizonEngine";
#endif
		if (!dir.empty()) fs::create_directories(dir, ec);
		return dir;
	}();
	return resolved;
}

std::filesystem::path GlobalState::engineContentCacheDir()
{
	static const fs::path resolved = [] {
		const fs::path dir = userDataDir() / "EngineContentCache";
		std::error_code ec;
		if (!dir.empty()) fs::create_directories(dir, ec);
		return dir;
	}();
	return resolved;
}

// ─── Where the settings live ─────────────────────────────────────────────────
std::filesystem::path GlobalState::configFilePath()
{
	// Resolved once: the answer cannot change during a run, and every caller
	// would otherwise repeat the same filesystem probing.
	static const fs::path resolved = [] {
		std::error_code ec;

		// 1. A config.json sitting next to the executable wins. That keeps a
		//    portable checkout and every existing development setup behaving
		//    exactly as before, and means this change never relocates settings
		//    somebody already has.
		if (const char* base = std::getenv("HE_CONFIG_DIR"))
		{
			fs::path forced = fs::path(base) / "config.json";
			fs::create_directories(forced.parent_path(), ec);
			return forced;
		}
		if (fs::exists("config.json", ec))
			return fs::path("config.json");

		// 2. Otherwise the per-user application-data directory. A .app launched
		//    from Finder has a working directory of "/", so anything
		//    CWD-relative is unwritable — which is exactly how settings silently
		//    stopped persisting.
		const fs::path dir = userDataDir();

		// 3. No home directory at all (a stripped service account, a sandbox):
		//    fall back to the old behaviour rather than returning an empty path,
		//    so the caller still has something to try.
		if (dir.empty()) return fs::path("config.json");

		return dir / "config.json";
	}();
	return resolved;
}

void GlobalState::readConfig()
{
	const fs::path cfgPath = configFilePath();
	if (!fs::exists(cfgPath))
	{
		// Naming the path matters: with the location no longer tied to the working
		// directory, "no config found" is only diagnosable if it says where it looked.
		HE_LOG_WARN(Config, "No config file at %s — using defaults", cfgPath.string().c_str());
		m_engineStatus.selectedRHI = defaultRHI();
		m_engineStatus.lastProjectPath = "";
		m_engineStatus.knownProjects.clear();
		writeConfig();
		return;
	}
	std::ifstream configFile(cfgPath);
	if (!configFile.is_open())
	{
		HE_LOG_ERROR(Config, "%s", "Failed to open config file.");
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
		HE_LOG_WARN(Config, "%s",
			"config.json is corrupt or unreadable — resetting to defaults");
		m_engineStatus.selectedRHI     = defaultRHI();
		m_engineStatus.lastProjectPath = "";
		m_engineStatus.knownProjects.clear();
		m_customConfig.clear();
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
	m_engineStatus.selectedRHI      = sanitizeRHI(static_cast<HE::RendererBackend>(
		intField("RHI", static_cast<int>(defaultRHI()))));
	// A config written by an older editor still carries an "OS" key. It was never
	// read by anything (it was hard-coded to Windows on every platform), so it is
	// deliberately not looked up here: unknown keys are simply ignored, which keeps
	// an old config.json loadable instead of turning it into a startup failure.
	m_engineStatus.lastProjectPath  = strField("LastProjectPath");
	m_engineStatus.knownProjects.clear();
	if (j.contains("KnownProjects") && j["KnownProjects"].is_array())
	{
		for (const auto& entry : j["KnownProjects"])
		{
			if (!entry.is_string()) continue;
			std::string p = entry.get<std::string>();
			// Guard against corrupted entries (e.g. a settings string stored here by mistake)
			if (p.size() >= 7 && p.substr(p.size() - 7) == ".heproj")
				m_engineStatus.knownProjects.push_back(std::move(p));
		}
	}
	if (j.contains("CustomConfig") && j["CustomConfig"].is_array())
	{
		for (const auto& entry : j["CustomConfig"])
		{
			if (entry.contains("Key") && entry["Key"].is_string() &&
				entry.contains("Value"))
			{
				m_customConfig[entry["Key"].get<std::string>()] = entry["Value"];
			}
		}
	}

	// Persisted log verbosity ("Render=Debug,Physics=Trace"), then HE_LOG again so
	// the environment always wins over whatever the config happens to hold.
	if (const std::string spec = getCustomConfigString("logVerbosity"); !spec.empty())
		HE::Log::configureFromString(spec.c_str());
	if (const char* env = std::getenv("HE_LOG"))
		HE::Log::configureFromString(env);

	HE_LOG_INFO(Config, "Config loaded: RHI=%d, lastProject='%s', %zu known project(s), %zu custom entrie(s)",
	            static_cast<int>(m_engineStatus.selectedRHI),
	            m_engineStatus.lastProjectPath.c_str(),
	            m_engineStatus.knownProjects.size(),
	            m_customConfig.size());
}

bool GlobalState::writeConfig()
{
	json j;
	j["RHI"] = m_engineStatus.selectedRHI;
	j["LastProjectPath"] = m_engineStatus.lastProjectPath;
	j["KnownProjects"] = m_engineStatus.knownProjects;
	json::array_t customEntries;
	for (const auto& [key, value] : m_customConfig.items())
	{
		customEntries.push_back({ {"Key", key}, {"Value", value} });
	}
	j["CustomConfig"] = customEntries;

	// Write temp + atomic rename. A plain in-place ofstream truncates the only copy
	// before the new content is durable, so a crash or kill mid-write (e.g. an abort
	// during shutdown) leaves a half-written config that then crash-loops the next
	// startup. rename() swaps atomically on POSIX and the old config stays intact on
	// any failure. (Same pattern as ProjectManager::saveProject.)
	const fs::path cfgPath = configFilePath();
	const fs::path tmp     = cfgPath.string() + ".tmp";
	{
		std::ofstream out(tmp, std::ios::trunc);
		if (!out.is_open())
		{
			HE_LOG_ERROR(Config, "Failed to open the config file for writing: %s",
			             cfgPath.string().c_str());
			return false;
		}
		// Serialize with the "replace" error handler: nlohmann's default dump()
		// THROWS type_error.316 on any string holding invalid UTF-8, and this runs
		// at shutdown with no catch above it -> terminate() -> SIGABRT (code 134).
		// A single bad byte anywhere in the state (e.g. a per-project "openTabs:"
		// value or a path picked up from the filesystem) would abort the whole app.
		// The read side is already fully non-throwing; make the write side match by
		// substituting U+FFFD for invalid bytes instead of crashing.
		out << j.dump(4, ' ', false, json::error_handler_t::replace);
		out.flush();
		if (!out.good())
		{
			out.close();
			std::error_code ec;
			fs::remove(tmp, ec);
			HE_LOG_ERROR(Config, "%s", "Failed to write config file.");
			return false;
		}
	}
	std::error_code ec;
	fs::rename(tmp, cfgPath, ec);
	if (ec)
	{
		std::error_code ec2;
		fs::remove(tmp, ec2);
		HE_LOG_ERROR(Config, "%s", "Failed to commit config file.");
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
	auto& kp = m_engineStatus.knownProjects;
	// Remove existing occurrence to avoid duplicates
	kp.erase(std::remove(kp.begin(), kp.end(), path), kp.end());
	kp.insert(kp.begin(), path);
	if (kp.size() > 10)
		kp.resize(10);
	m_engineStatus.lastProjectPath = path;
}

void GlobalState::removeKnownProject(const std::string& path)
{
	auto& kp = m_engineStatus.knownProjects;
	kp.erase(std::remove(kp.begin(), kp.end(), path), kp.end());
}

static void clearFolder(HE::Folder* folder)
{
	for (HE::Folder* sub : folder->subfolders)
	{
		clearFolder(sub);
		delete sub;
	}
	folder->subfolders.clear();
	for (HE::File* file : folder->files)
		delete file;
	folder->files.clear();
}

// RAII owner for a Folder tree that is still being built on the stack.
// Folder/File children are raw `new`ed pointers and Folder has no destructor of
// its own (the content browser passes Folder*/File* around and must never own
// them), so anything that unwinds out of the build — an early return, or a
// bad_alloc from the string/vector work — would silently leak the whole
// half-built subtree. Handover is done by swapping the built tree into the
// member and leaving the (already emptied) member root behind here, so this
// guard can stay armed for the entire function and still be a no-op on success.
namespace
{
	struct ScopedFolderNodes
	{
		HE::Folder* folder;
		explicit ScopedFolderNodes(HE::Folder* f) : folder(f) {}
		~ScopedFolderNodes() { clearFolder(folder); }
		ScopedFolderNodes(const ScopedFolderNodes&)            = delete;
		ScopedFolderNodes& operator=(const ScopedFolderNodes&) = delete;
	};
}

static void populateFolder(HE::Folder* folder, const fs::path& path)
{
	// error_code overloads throughout: the throwing directory_iterator raises
	// filesystem_error on the very first unreadable entry (a permission-denied
	// directory, a directory deleted by another process mid-walk, a dead symlink
	// into an unmounted share). That exception escaped all the way out of
	// refreshContentFolder() — which has no handler above it — and additionally
	// abandoned the walk with a half-built tree. Now an unreadable directory just
	// contributes nothing and the rest of the tree still populates.
	std::error_code ec;
	fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
	if (ec)
		return;

	const fs::directory_iterator end;
	for (; it != end; it.increment(ec))
	{
		if (ec)
			return; // iteration broke down (unreadable dir) — keep what we have

		const fs::directory_entry& entry = *it;

		// Hide dotfiles/dotfolders (.gitkeep, .DS_Store, .git, …) — they are
		// VCS/OS bookkeeping, never browsable content. (A ".gitkeep" is how the
		// engine's empty category folders survive in git; it must not surface as
		// a fake asset in the Content Browser.)
		if (entry.path().filename().string().rfind('.', 0) == 0)
			continue;

		std::error_code typeEc;
		if (entry.is_directory(typeEc))
		{
			// Hold the fresh node in a unique_ptr until it is linked into the
			// tree: the node only gets an owner once push_back succeeded, and
			// from then on the root's ScopedFolderNodes covers it. Anything that
			// throws in between (bad_alloc) therefore frees exactly once.
			std::unique_ptr<HE::Folder> sub(new HE::Folder());
			sub->name     = entry.path().filename().string();
			sub->fullPath = entry.path().string();
			folder->subfolders.push_back(sub.get());
			populateFolder(sub.release(), entry.path());
		}
		else if (entry.is_regular_file(typeEc))
		{
			std::unique_ptr<HE::File> file(new HE::File());
			file->name      = entry.path().filename().string();
			file->fullPath  = entry.path().string();
			file->extension = entry.path().extension().string();
			folder->files.push_back(file.get());
			file.release();
		}
	}
}

bool GlobalState::refreshContentFolder()
{
	if (m_engineStatus.lastProjectPath.empty())
	{
		HE_LOG_WARN(Config, "%s", "No project loaded — cannot refresh content folder.");
		return false;
	}

	// error_code overloads: these probes run on a user-supplied project path, which
	// may live on an unmounted/unreachable share — the throwing overloads would take
	// the editor down instead of reporting "not found".
	std::error_code ec;
	fs::path projectPath = m_engineStatus.lastProjectPath;
	if (fs::is_regular_file(projectPath, ec))
		projectPath = projectPath.parent_path();

	fs::path contentFolderpath = projectPath / "Content";
	if (!fs::is_directory(contentFolderpath, ec))
	{
		HE_LOG_ERROR(Config, "%s", ("Content folder not found at " + contentFolderpath.string()).c_str());
		return false;
	}

	// Daten ausserhalb des Locks zusammenstellen, dann atomar eintauschen
	HE::Folder fresh;
	ScopedFolderNodes freshOwner(&fresh);
	fresh.name     = contentFolderpath.filename().string();
	fresh.fullPath = contentFolderpath.string();
	populateFolder(&fresh, contentFolderpath);

	{
		std::unique_lock lock(m_contentFolderMutex);
		clearFolder(&m_contentFolder);
		// swap, not move-assign: clearFolder just emptied the member's child
		// vectors, so after the swap `fresh` provably holds no live nodes and its
		// guard above becomes a no-op. A move-assign would leave the source in an
		// unspecified state that the guard might then double-free.
		std::swap(m_contentFolder, fresh);
	}
	// Old Folder/File nodes are gone — tell pointer-holders to re-resolve by path.
	contentFolderVersion.fetch_add(1, std::memory_order_release);

	HE_LOG_INFO(Config, "%s", "Content folder refreshed.");
	HE_LOG_INFO(Config, "%s", ("Number of folders: " + std::to_string(m_contentFolder.subfolders.size())).c_str());
	HE_LOG_INFO(Config, "%s", ("Number of files: " + std::to_string(m_contentFolder.files.size())).c_str());
	return true;
}

bool GlobalState::refreshSourceFolder()
{
	if (m_engineStatus.lastProjectPath.empty())
		return false;

	std::error_code ec;
	fs::path projectPath = m_engineStatus.lastProjectPath;
	if (fs::is_regular_file(projectPath, ec))
		projectPath = projectPath.parent_path();

	fs::path sourcePath = projectPath / "Source";

	// Build off-lock. An absent Source/ (non-C++ project, or not scaffolded yet)
	// is not an error — leave the tree empty; the root's fullPath is still set so
	// the browser's drop/create targets resolve.
	HE::Folder fresh;
	ScopedFolderNodes freshOwner(&fresh);
	fresh.name     = "Source";
	fresh.fullPath = sourcePath.string();
	if (fs::is_directory(sourcePath, ec))
		populateFolder(&fresh, sourcePath);

	{
		std::unique_lock lock(m_sourceFolderMutex);
		clearFolder(&m_sourceFolder);
		std::swap(m_sourceFolder, fresh); // see refreshContentFolder() for why swap
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
static void mergeOverrideInto(HE::Folder* base, const fs::path& overrideDir)
{
	// Same non-throwing iteration contract as populateFolder — an unreadable
	// override directory must degrade to "no overrides", never to an exception
	// escaping refreshEngineFolder().
	std::error_code ec;
	fs::directory_iterator it(overrideDir, fs::directory_options::skip_permission_denied, ec);
	if (ec)
		return;

	const fs::directory_iterator end;
	for (; it != end; it.increment(ec))
	{
		if (ec)
			return;

		const fs::directory_entry& entry = *it;

		if (entry.path().filename().string().rfind('.', 0) == 0)
			continue; // same dotfile filter as populateFolder

		std::error_code typeEc;
		if (entry.is_directory(typeEc))
		{
			HE::Folder* sub = nullptr;
			for (HE::Folder* s : base->subfolders)
				if (s->name == entry.path().filename().string()) { sub = s; break; }
			if (!sub)
			{
				// Owned by the unique_ptr until push_back linked it into the tree
				// (see populateFolder) — the root's ScopedFolderNodes takes over
				// from there.
				std::unique_ptr<HE::Folder> owned(new HE::Folder());
				owned->name     = entry.path().filename().string();
				owned->fullPath = entry.path().string();
				base->subfolders.push_back(owned.get());
				sub = owned.release();
			}
			mergeOverrideInto(sub, entry.path());
		}
		else if (entry.is_regular_file(typeEc))
		{
			HE::File* match = nullptr;
			for (HE::File* f : base->files)
				if (f->name == entry.path().filename().string()) { match = f; break; }
			if (match)
				match->fullPath = entry.path().string(); // override shadows the default
			else
			{
				std::unique_ptr<HE::File> file(new HE::File());
				file->name      = entry.path().filename().string();
				file->fullPath  = entry.path().string();
				file->extension = entry.path().extension().string();
				base->files.push_back(file.get());
				file.release();
			}
		}
	}
}

// Splits a manifest's forward-slash-separated relative path ("Materials/Foo.hasset")
// into folder segments + a leaf filename, walking/creating HE::Folder nodes along
// the way. Used only for entries that mergeManifestInto has already determined do
// not exist locally — an existing folder is reused (by name, same rule
// mergeOverrideInto uses), a missing one is created as an empty placeholder so the
// tree still has somewhere to hang the remote-only leaf.
// `cacheRoot` is where a synthesized folder's fullPath is rooted. It is NOT the
// engine content root: a folder that exists only remotely has no directory under
// the shipped defaults, and pointing it there would hand the Content Browser a
// path that does not exist (every create/import/drop target resolves against a
// folder's fullPath). The download cache is the location such a folder will
// actually acquire once anything inside it is fetched, so that is the honest
// answer. Folders that DO exist locally are found by name and keep their own
// real path — only genuinely new levels are synthesized here.
static HE::Folder* walkOrCreateFolderPath(HE::Folder* root, const std::vector<std::string>& segments,
                                           const fs::path& cacheRoot)
{
	HE::Folder* cur = root;
	fs::path    cacheSoFar = cacheRoot;
	for (const std::string& seg : segments)
	{
		cacheSoFar /= seg;

		HE::Folder* next = nullptr;
		for (HE::Folder* s : cur->subfolders)
			if (s->name == seg) { next = s; break; }
		if (!next)
		{
			std::unique_ptr<HE::Folder> owned(new HE::Folder());
			owned->name     = seg;
			owned->fullPath = cacheSoFar.string();
			cur->subfolders.push_back(owned.get());
			next = owned.release();
		}
		cur = next;
	}
	return cur;
}

// Adds a File node for every manifest entry not already present in `base` (the
// default tree + project override, both already merged in by the time this runs)
// — so the Content Browser's Engine folder shows the full catalogue of available
// defaults, not just what happens to be on this machine.
//
// Three outcomes per entry:
//   • already in the tree (shipped default or project override)  → skipped
//     entirely; a real local file always wins over anything remote.
//   • present in the download cache from an earlier session      → added as a
//     NORMAL node (isRemoteOnly=false) pointing at the cached file. Without this
//     check a downloaded asset would keep its "on the server" badge forever and
//     every double-click would re-ask to download a file already sitting on disk.
//   • not present anywhere                                        → added as a
//     remote-only placeholder whose fullPath is where the file WILL land once
//     fetched (sftpGetFile writes exactly there).
static void mergeManifestInto(HE::Folder* base, const std::vector<HE::RemoteEngineAsset>& remoteOnly)
{
	const fs::path cacheRoot = GlobalState::engineContentCacheDir();

	for (const HE::RemoteEngineAsset& asset : remoteOnly)
	{
		if (asset.relativePath.empty()) continue;

		const fs::path relPath(asset.relativePath);
		const std::string leafName = relPath.filename().string();
		if (leafName.empty()) continue;

		std::vector<std::string> segments;
		for (const auto& part : relPath.parent_path())
			segments.push_back(part.string());

		HE::Folder* parent = walkOrCreateFolderPath(base, segments, cacheRoot);

		bool existsLocally = false;
		for (HE::File* f : parent->files)
			if (f->name == leafName) { existsLocally = true; break; }
		if (existsLocally) continue; // a real local/override file always wins

		const fs::path cachePath = cacheRoot / relPath;
		std::error_code ec;
		const bool cached = fs::is_regular_file(cachePath, ec);

		std::unique_ptr<HE::File> file(new HE::File());
		file->name         = leafName;
		file->extension    = relPath.extension().string();
		file->isRemoteOnly = !cached;
		file->remoteUuid   = asset.uuid;
		file->fullPath     = cachePath.string();
		parent->files.push_back(file.get());
		file.release();
	}
}

void GlobalState::setEngineRemoteAssets(std::vector<HE::RemoteEngineAsset> assets)
{
	std::unique_lock lock(m_engineFolderMutex);
	m_engineRemoteAssets = std::move(assets);
}

bool GlobalState::refreshEngineFolder(const std::string& engineContentAbsPath,
                                       const std::string& projectContentRoot)
{
	// Copied out under the lock rather than held across the tree build: building
	// takes a while (a full recursive directory walk) and the SFTP worker may
	// replace the set meanwhile — and the swap at the end needs this same lock.
	std::vector<HE::RemoteEngineAsset> remoteOnly;
	{
		std::shared_lock lock(m_engineFolderMutex);
		remoteOnly = m_engineRemoteAssets;
	}

	std::error_code ec;
	fs::path enginePath = engineContentAbsPath;
	if (!fs::is_directory(enginePath, ec))
	{
		HE_LOG_WARN(Config, "%s", ("Engine content folder not found at " + enginePath.string()).c_str());
		return false;
	}

	HE::Folder fresh;
	ScopedFolderNodes freshOwner(&fresh);
	fresh.name     = "Engine";
	fresh.fullPath = enginePath.string();
	populateFolder(&fresh, enginePath);

	// Project-level overrides (Content/Engine/...) merge in on top, so the
	// Content Browser's "Engine" tree shows one unified, effective view —
	// same tree position as the default, just backed by the override file.
	if (!projectContentRoot.empty())
	{
		const fs::path overrideRoot = fs::path(projectContentRoot) / "Engine";
		if (fs::is_directory(overrideRoot, ec))
			mergeOverrideInto(&fresh, overrideRoot);
	}

	// Remote-only placeholders merge in LAST: they must see the fully-merged
	// default+override tree so a manifest entry that already exists in either
	// one is correctly skipped rather than shown as a redundant "remote" copy.
	if (!remoteOnly.empty())
		mergeManifestInto(&fresh, remoteOnly);

	{
		std::unique_lock lock(m_engineFolderMutex);
		clearFolder(&m_engineFolder);
		std::swap(m_engineFolder, fresh); // see refreshContentFolder() for why swap
	}
	engineFolderVersion.fetch_add(1, std::memory_order_release);

	HE_LOG_INFO(Config, "%s", "Engine content folder refreshed.");
	return true;
}

void GlobalState::setCustomConfigEntry(const std::string& key, const json& value)
{
	m_customConfig[key] = value;
}

int GlobalState::getCustomConfigInt(const std::string& key, int defaultValue) const
{
	if (m_customConfig.contains(key) && m_customConfig[key].is_number())
		return m_customConfig[key].get<int>();
	return defaultValue;
}

float GlobalState::getCustomConfigFloat(const std::string& key, float defaultValue) const
{
	if (m_customConfig.contains(key) && m_customConfig[key].is_number())
		return m_customConfig[key].get<float>();
	return defaultValue;
}

bool GlobalState::getCustomConfigBool(const std::string& key, bool defaultValue) const
{
	if (m_customConfig.contains(key) && m_customConfig[key].is_boolean())
		return m_customConfig[key].get<bool>();
	return defaultValue;
}

std::string GlobalState::getCustomConfigString(const std::string& key, const std::string& defaultValue) const
{
	if (m_customConfig.contains(key) && m_customConfig[key].is_string())
		return m_customConfig[key].get<std::string>();
	return defaultValue;
}