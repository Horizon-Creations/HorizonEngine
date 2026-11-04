#pragma once

// ─── Is a cached EngineContent file still what the server lists? ──────────────────
// The sync downloads a file when it is MISSING from the cache. A file the server later replaces under
// the same name (new landscape textures) stayed where it was: the editor kept using the old copy for
// ever, "the new textures are not visible". After each manifest refresh the cache is now compared with
// the new manifest: a cached .hasset whose size or content hash differs from its entry is removed, so
// the normal on-demand download fetches the current one.
//
// The hash is the manifest's own (Hpak::fnv1a64Bytes of the file). Hashing a 300 MB array costs about
// half a second, so a file is hashed once and remembered (path -> size, mtime, hash in
// <cacheDir>/.cache-hashes.json); a download records the hash its manifest entry promised, and an
// unchanged size + mtime reuses the record.

#include "ContentSync/CsCommon.h"
#include "ContentSync/EngineContentManifest.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

namespace HE::Cs {

// Removes the stale cached files `manifest` knows better; returns how many. `isBusy(relativePath)` true =
// a download of that file is queued or running: left alone. Entries without a content hash (raw files)
// are only compared by size when the manifest has one. onLog gets one line per removal.
HE_CS_API std::size_t discardStaleCachedFiles(const std::filesystem::path& cacheDir,
                                              const EngineContentManifest& manifest,
                                              const std::function<bool(const std::string&)>& isBusy = {},
                                              const std::function<void(const std::string&)>& onLog = {});

// A download of `relativePath` just landed and the manifest promised `contentHash`: remember it so the
// next refresh does not have to hash the file. contentHash 0 forgets the record instead.
HE_CS_API void recordCachedFile(const std::filesystem::path& cacheDir, const std::string& relativePath,
                                std::uint64_t contentHash);

} // namespace HE::Cs
