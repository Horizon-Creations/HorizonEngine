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
#include <vector>

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

// Uploads exactly `relPaths` (".hasset" files, relative to `engineContentRoot`) and MERGES their
// manifest entries into the remote manifest.json (an entry with the same path is replaced, every other
// entry is kept). publishEngineContentBlocking uploads the manifest of the LOCAL scan, so run on a root
// that holds only part of the library it would delist everything else; this is the call for adding or
// replacing a few files. Refuses (uploads nothing) when the remote manifest cannot be read, rather than
// treating it as empty. The manifest goes up last, like in the full publish.
HE_CS_API PublishResult publishEngineContentFilesBlocking(const std::string& engineContentRoot,
                                                            const std::vector<std::string>& relPaths,
                                                            std::function<void(const std::string&)> onLog = {});

// Checks that every `relPaths` file is on the server: remote size == local size and the remote manifest
// entry carries the local content hash. One line per file through onLog; ok only when all agree.
HE_CS_API PublishResult verifyEngineContentFilesBlocking(const std::string& engineContentRoot,
                                                           const std::vector<std::string>& relPaths,
                                                           std::function<void(const std::string&)> onLog = {});

struct RebuildManifestResult
{
	bool        ok = false;
	std::string error;              // empty when ok
	std::size_t hassetEntries = 0;  // downloaded to read their UUID + hash
	std::size_t rawEntries    = 0;  // size/mtime only, no download
};

// Bootstraps (or replaces) manifest.json from whatever is ALREADY on the
// server, rather than from a local EngineContent folder — for a server that
// already has content this tool never uploaded (e.g. a pre-existing archive of
// raw source audio). Lists the whole remote tree (see SftpClient::
// sftpListRemoteTree — metadata only, no download), then for each file:
// a ".hasset" is downloaded once to read its UUID and content hash (the same
// way publishEngineContentBlocking does it locally — there is no way around
// downloading it, the UUID lives inside); anything else becomes a raw manifest
// entry (empty UUID, size + mtime only, see EngineContentManifestEntry) with no
// download at all. Blocking — call on a worker thread.
HE_CS_API RebuildManifestResult rebuildManifestFromServerBlocking(
	std::function<void(const std::string&)> onLog = {});

} // namespace HE::Cs
