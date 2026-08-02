#include "Application/GameLogicLoader.h"
#include "Diagnostics/Logger.h"
#include <cstdio>
#include <system_error>

namespace HE {

GameLogicLoader::GameLogicLoader()  = default;
GameLogicLoader::~GameLogicLoader() = default;

bool GameLogicLoader::load(const std::filesystem::path& dllPath)
{
	if (m_lib.isLoaded())
	{
		Logger::Log(Logger::LogLevel::Warning,
			"GameLogicLoader: load() called while a library is loaded — ignoring");
		return false;
	}

	std::error_code ec;
	if (!std::filesystem::exists(dllPath, ec) || ec) return false;

	// Never dlopen the watched path directly: on macOS dyld caches by path/inode
	// (and can hand back a previously loaded image on reload), on Windows the
	// loader locks the file so the next build would fail. Copy to a uniquely
	// numbered sibling (GameLogic.hot-0001.dylib, …) and load THAT; unload is
	// best-effort (TLS/Obj-C can pin the old image), so a name is never reused.
	static unsigned s_loadCounter = 0;
	std::filesystem::path loadPath = dllPath;
	{
		++s_loadCounter;
		char suffix[32];
		std::snprintf(suffix, sizeof(suffix), ".hot-%04u", s_loadCounter);
		std::filesystem::path copy = dllPath;
		copy.replace_extension();                        // strip .dylib/.dll/.so
		copy += suffix;
		copy += dllPath.extension();
		std::filesystem::copy_file(dllPath, copy,
			std::filesystem::copy_options::overwrite_existing, ec);
		if (!ec) loadPath = copy;                        // copy failure → load the original directly
	}
	m_loadedCopyPath = (loadPath != dllPath) ? loadPath : std::filesystem::path{};

	if (!m_lib.load(loadPath))
	{
		Logger::Log(Logger::LogLevel::Error,
			("GameLogicLoader: failed to load " + loadPath.string()).c_str());
		return false;
	}

	auto createFn = reinterpret_cast<FnCreateGameLogic>(m_lib.getSymbol("HE_CreateGameLogic"));
	m_destroyFn    = reinterpret_cast<FnDestroyGameLogic>(m_lib.getSymbol("HE_DestroyGameLogic"));
	if (!createFn || !m_destroyFn)
	{
		Logger::Log(Logger::LogLevel::Error,
			("GameLogicLoader: missing HE_CreateGameLogic/HE_DestroyGameLogic exports in "
			 + loadPath.string()).c_str());
		m_destroyFn = nullptr;
		m_lib.unload();
		return false;
	}

	m_logic = createFn();
	if (!m_logic)
	{
		Logger::Log(Logger::LogLevel::Error, "GameLogicLoader: HE_CreateGameLogic returned null");
		m_destroyFn = nullptr;
		m_lib.unload();
		return false;
	}

	Logger::Log(Logger::LogLevel::Info,
		("GameLogicLoader: loaded " + dllPath.filename().string()).c_str());
	return true;
}

void GameLogicLoader::unload(HorizonWorld& world)
{
	if (m_logic)
	{
		m_logic->onStop(world);
		if (m_destroyFn) m_destroyFn(m_logic);
		m_logic = nullptr;
	}
	m_destroyFn = nullptr;
	if (m_lib.isLoaded())
		m_lib.unload();   // best-effort on macOS; unique-name copies make staleness harmless

	// Remove the hot-copy we loaded from (best-effort; may fail while the OS
	// still has the image pinned — the numbered names avoid any collision).
	if (!m_loadedCopyPath.empty())
	{
		std::error_code ec;
		std::filesystem::remove(m_loadedCopyPath, ec);
		m_loadedCopyPath.clear();
	}
}

bool GameLogicLoader::reload(const std::filesystem::path& dllPath, HorizonWorld& world)
{
	unload(world);
	return load(dllPath);
}

bool GameLogicLoader::isLoaded() const { return m_logic != nullptr; }
IGameLogic* GameLogicLoader::logic() const { return m_logic; }

} // namespace HE
