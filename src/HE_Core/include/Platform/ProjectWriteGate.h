#pragma once

// ── "The project's files are being read; do not change them now" ─────────────
// An export packs Content/ and serializes scenes on a worker thread. A save, an
// import or a rename in the middle of that does not fail loudly — it produces a
// package that holds half of the old file and half of the new one, or an asset
// that is missing from the pack for no visible reason. The Build window used to
// prevent this by being a modal the user could not leave. It can be put away now
// (BuildProgressDialog "Hide"), so the protection has to be a rule the writers
// follow instead of a window in the way.
//
// This is that rule, in the one library every writer already links: a probe the
// editor installs ("is an export running?") and a handler it installs ("tell the
// user"). Writers ask refuse(what) at the point where they would change a file
// under the project; true means "do not", and the user has been told. Nothing is
// installed in the tools and the game, so there locked() is simply false.
//
// Header-only on purpose (inline state), so HE_Tools' importer can use it without
// a new link dependency.

#include <atomic>

namespace HE::ProjectWriteGate
{
	using Probe   = bool (*)();
	using Handler = void (*)(const char* what);

	namespace detail
	{
		inline std::atomic<Probe>   probe{ nullptr };
		inline std::atomic<Handler> handler{ nullptr };
	}

	// Editor only, once at start. Either may be null.
	inline void install(Probe probe, Handler handler)
	{
		detail::probe.store(probe, std::memory_order_release);
		detail::handler.store(handler, std::memory_order_release);
	}

	// Safe from any thread (the probe must be).
	inline bool locked()
	{
		const Probe p = detail::probe.load(std::memory_order_acquire);
		return p && p();
	}

	// For a writer about to change a file under the project. `what` completes
	// "You tried to ...": a short phrase such as "save this asset". Returns true
	// when the write must not happen. The handler runs on the calling thread and
	// decides how loud to be (it rate-limits, a save loop calls this per attempt).
	inline bool refuse(const char* what)
	{
		if (!locked()) return false;
		if (const Handler h = detail::handler.load(std::memory_order_acquire)) h(what);
		return true;
	}
}
