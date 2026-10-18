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
// Not header-only: the state is defined once, in HorizonCore (ProjectWriteGate.cpp).
// An inline variable in a header is one copy per DLL/exe on Windows, so the probe
// the editor exe installed was never seen by ContentManager::saveAsset in the DLL.

#include "Types/Defines.h"

namespace HE::ProjectWriteGate
{
	using Probe   = bool (*)();
	using Handler = void (*)(const char* what);

	// Editor only, once at start. Either may be null.
	HE_API void install(Probe probe, Handler handler);

	// Safe from any thread (the probe must be).
	HE_API bool locked();

	// For a writer about to change a file under the project. `what` completes
	// "You tried to ...": a short phrase such as "save this asset". Returns true
	// when the write must not happen. The handler runs on the calling thread and
	// decides how loud to be (it rate-limits, a save loop calls this per attempt).
	HE_API bool refuse(const char* what);
}
