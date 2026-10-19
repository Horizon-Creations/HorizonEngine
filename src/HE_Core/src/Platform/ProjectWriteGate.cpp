#include "Platform/ProjectWriteGate.h"

#include <atomic>

// The gate's one copy of its state. It lives here, inside HorizonCore, and not as
// inline variables in the header: on Windows every DLL and exe gets its own copy
// of a header's inline variable, so the editor's install() (HorizonEditor.exe)
// never reached ContentManager::saveAsset's refuse() (HorizonCore.dll) and an
// export did not lock a single asset save there.

namespace HE::ProjectWriteGate
{
	namespace
	{
		std::atomic<Probe>   g_probe{ nullptr };
		std::atomic<Handler> g_handler{ nullptr };
	}

	void install(Probe probe, Handler handler)
	{
		g_probe.store(probe, std::memory_order_release);
		g_handler.store(handler, std::memory_order_release);
	}

	bool locked()
	{
		const Probe p = g_probe.load(std::memory_order_acquire);
		return p && p();
	}

	bool refuse(const char* what)
	{
		if (!locked()) return false;
		if (const Handler h = g_handler.load(std::memory_order_acquire)) h(what);
		return true;
	}
}
