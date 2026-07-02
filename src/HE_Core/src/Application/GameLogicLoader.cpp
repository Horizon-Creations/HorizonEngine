#include "Application/GameLogicLoader.h"
#include "Diagnostics/Logger.h"
#include <cstdio>
#include <system_error>

namespace HE {

GameLogicLoader::GameLogicLoader()  = default;
GameLogicLoader::~GameLogicLoader() = default;

bool GameLogicLoader::load(const std::filesystem::path& dllPath)
{
	if (lib_.isLoaded())
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
	loadedCopyPath_ = (loadPath != dllPath) ? loadPath : std::filesystem::path{};

	if (!lib_.load(loadPath))
	{
		Logger::Log(Logger::LogLevel::Error,
			("GameLogicLoader: failed to load " + loadPath.string()).c_str());
		return false;
	}

	auto createFn = reinterpret_cast<FnCreateGameLogic>(lib_.getSymbol("HE_CreateGameLogic"));
	destroyFn_    = reinterpret_cast<FnDestroyGameLogic>(lib_.getSymbol("HE_DestroyGameLogic"));
	if (!createFn || !destroyFn_)
	{
		Logger::Log(Logger::LogLevel::Error,
			("GameLogicLoader: missing HE_CreateGameLogic/HE_DestroyGameLogic exports in "
			 + loadPath.string()).c_str());
		destroyFn_ = nullptr;
		lib_.unload();
		return false;
	}

	logic_ = createFn();
	if (!logic_)
	{
		Logger::Log(Logger::LogLevel::Error, "GameLogicLoader: HE_CreateGameLogic returned null");
		destroyFn_ = nullptr;
		lib_.unload();
		return false;
	}

	Logger::Log(Logger::LogLevel::Info,
		("GameLogicLoader: loaded " + dllPath.filename().string()).c_str());
	return true;
}

void GameLogicLoader::unload(HorizonWorld& world)
{
	if (logic_)
	{
		logic_->onStop(world);
		if (destroyFn_) destroyFn_(logic_);
		logic_ = nullptr;
	}
	destroyFn_ = nullptr;
	if (lib_.isLoaded())
		lib_.unload();   // best-effort on macOS; unique-name copies make staleness harmless

	// Remove the hot-copy we loaded from (best-effort; may fail while the OS
	// still has the image pinned — the numbered names avoid any collision).
	if (!loadedCopyPath_.empty())
	{
		std::error_code ec;
		std::filesystem::remove(loadedCopyPath_, ec);
		loadedCopyPath_.clear();
	}
}

bool GameLogicLoader::reload(const std::filesystem::path& dllPath, HorizonWorld& world)
{
	unload(world);
	return load(dllPath);
}

bool GameLogicLoader::isLoaded() const { return logic_ != nullptr; }
IGameLogic* GameLogicLoader::logic() const { return logic_; }

} // namespace HE
