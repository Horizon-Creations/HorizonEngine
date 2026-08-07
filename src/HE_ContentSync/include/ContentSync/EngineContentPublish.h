#pragma once

// ─── EngineContent publish (dev-side) ──────────────────────────────────────────
// Walks a local EngineContent root, builds a manifest, diffs it against the
// remote manifest.json, and uploads whatever changed. The counterpart to
// EngineContentSync's consume side — this is the tool that puts assets ON the
// server in the first place. Gated in the Editor UI behind
// ContentManager::isEngineContentDevMode(); nothing here enforces that itself
// (it is a pure "do the publish" function, not a permission check).

#include "ContentSync/CsCommon.h"

#include <functional>
#include <string>

namespace HE::Cs {

struct PublishResult
{
	bool        ok = false;
	std::string error;         // empty when ok
	std::size_t filesUploaded  = 0;
	std::size_t filesUnchanged = 0; // same content hash as the remote manifest already had
};

// Blocking — call on a worker thread. onLog (optional) receives human-readable
// progress lines ("Uploaded Materials/Foo.hasset", …) for a UI to display live.
HE_CS_API PublishResult publishEngineContentBlocking(const std::string& engineContentRoot,
                                                       std::function<void(const std::string&)> onLog = {});

} // namespace HE::Cs
