#include "Application/GameLogicLoader.h"
#include <HorizonGameServices.h>
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
		HE_LOG_WARN(GameLogic, "%s",
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
		HE_LOG_ERROR(GameLogic, "%s",
			("GameLogicLoader: failed to load " + loadPath.string()).c_str());
		return false;
	}

	auto createFn = reinterpret_cast<FnCreateGameLogic>(m_lib.getSymbol("HE_CreateGameLogic"));
	m_destroyFn    = reinterpret_cast<FnDestroyGameLogic>(m_lib.getSymbol("HE_DestroyGameLogic"));
	if (!createFn || !m_destroyFn)
	{
		HE_LOG_ERROR(GameLogic, "%s",
			("GameLogicLoader: missing HE_CreateGameLogic/HE_DestroyGameLogic exports in "
			 + loadPath.string()).c_str());
		m_destroyFn = nullptr;
		m_lib.unload();
		return false;
	}

	m_logic = createFn();
	if (!m_logic)
	{
		HE_LOG_ERROR(GameLogic, "%s", "GameLogicLoader: HE_CreateGameLogic returned null");
		m_destroyFn = nullptr;
		m_lib.unload();
		return false;
	}

	HE_LOG_INFO(GameLogic, "%s",
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

bool GameLogicLoader::loadAndStart(const std::filesystem::path& dllPath, HorizonWorld& world,
                                   const HeEngineServices* services)
{
	if (!load(dllPath)) return false;
	// BEFORE onStart, always: the tables are what onStart's first he::save or
	// he::physics call reads. Skipped for a null table set — a module that was
	// handed nothing is still a module.
	if (services) injectServices(services);
	m_logic->onStart(world);
	return true;
}

bool GameLogicLoader::reloadAndStart(const std::filesystem::path& dllPath, HorizonWorld& world,
                                     const HeEngineServices* services)
{
	unload(world);   // onStop on the outgoing image
	return loadAndStart(dllPath, world, services);
}

bool GameLogicLoader::isLoaded() const { return m_logic != nullptr; }
IGameLogic* GameLogicLoader::logic() const { return m_logic; }

bool GameLogicLoader::injectServices(const HeSaveServices* services)
{
	if (!isLoaded()) return false;
	auto setFn = reinterpret_cast<FnSetEngineServices>(m_lib.getSymbol("HE_SetEngineServices"));
	if (!setFn)
	{
		HE_LOG_INFO(GameLogic, "%s",
			"GameLogicLoader: library has no HE_SetEngineServices export "
			"(older scaffold) — he::save/he::entity read as unavailable in game code");
		return false;
	}
	setFn(services);
	HE_LOG_INFO(GameLogic, "%s", "GameLogicLoader: engine services injected");
	return true;
}

bool GameLogicLoader::injectServices(const HeEngineServices* services)
{
	if (!isLoaded()) return false;

	// The current export first: it hands over every table at once.
	auto setV2 = reinterpret_cast<FnSetEngineServicesV2>(m_lib.getSymbol("HE_SetEngineServicesV2"));
	if (setV2)
	{
		setV2(services);
		HE_LOG_INFO(GameLogic, "%s", "GameLogicLoader: engine services injected (v2)");
		return true;
	}

	// A library built before the umbrella existed still has the v1 export, and
	// its savegame API has to keep working — losing it because the engine grew
	// would be the opposite of compatible. Physics/input stay unavailable there.
	HE_LOG_INFO(GameLogic, "%s",
		"GameLogicLoader: library has no HE_SetEngineServicesV2 export (older scaffold) "
		"— falling back to the save-only v1 table; he::physics/he::input read as "
		"unavailable in game code");
	return injectServices(services ? services->save : nullptr);
}

} // namespace HE
