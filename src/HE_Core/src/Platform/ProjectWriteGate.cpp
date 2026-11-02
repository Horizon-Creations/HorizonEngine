#include "Platform/ProjectWriteGate.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

// The gate's one copy of its state. It lives here, inside HorizonCore, and not as
// inline variables in the header: on Windows every DLL and exe gets its own copy
// of a header's inline variable, so the editor's install() (HorizonEditor.exe)
// never reached ContentManager::saveAsset's refuse() (HorizonCore.dll) and an
// export did not lock a single asset save there.

namespace HE::ProjectWriteGate
{
	namespace
	{
		// probe and handler are published as one unit: a refuse() on a save thread
		// racing a re-install() must not pair the new probe with the old handler.
		struct Hooks
		{
			Probe   probe;
			Handler handler;
		};

		std::atomic<const Hooks*> g_hooks{ nullptr };

		// A reader may still hold a superseded Hooks, so none is ever freed. They
		// stay owned here (a handful per run) and the registry itself is leaked so
		// no static destructor races a late reader.
		const Hooks* retain(Probe probe, Handler handler)
		{
			static std::mutex mutex;
			static auto& all = *new std::vector<std::unique_ptr<Hooks>>();
			std::lock_guard<std::mutex> lock(mutex);
			all.push_back(std::make_unique<Hooks>(Hooks{ probe, handler }));
			return all.back().get();
		}
	}

	void install(Probe probe, Handler handler)
	{
		g_hooks.store(retain(probe, handler), std::memory_order_release);
	}

	bool locked()
	{
		const Hooks* hooks = g_hooks.load(std::memory_order_acquire);
		return hooks && hooks->probe && hooks->probe();
	}

	bool refuse(const char* what)
	{
		const Hooks* hooks = g_hooks.load(std::memory_order_acquire);
		if (!hooks || !hooks->probe || !hooks->probe()) return false;
		if (hooks->handler) hooks->handler(what);
		return true;
	}
}
