#pragma once

// ─── EngineContent manifest ────────────────────────────────────────────────────
// The one file that lets a consuming Editor know what EngineContent assets
// exist on the server WITHOUT downloading them — and lets the publish side know
// what actually changed since the last publish, so only new/modified files get
// re-uploaded.
//
// Deliberately flat JSON (nlohmann::json, same as every other config/asset
// metadata format in the engine) rather than reusing the .hpak manifest format:
// this describes loose files at paths, not pak entries.

#include "ContentSync/CsCommon.h"

#include <Types/UUID.h>

#include <cstdint>
#include <string>
#include <vector>

namespace HE::Cs {

struct EngineContentManifestEntry
{
	std::string relativePath;   // e.g. "Materials/DefaultCube.hasset" — relative to EngineContent root
	HE::UUID    uuid;           // the asset's own UUID (read from its META chunk at publish time)
	std::uint64_t contentHash = 0; // Hpak::hash64 of the file's bytes — same function incremental packing uses
	std::uint64_t size        = 0;
};

struct EngineContentManifest
{
	std::vector<EngineContentManifestEntry> entries;

	// Find by UUID / by path — linear scan is fine, this runs at most once per
	// Editor session per lookup context (a handful of times), never per frame.
	const EngineContentManifestEntry* findByUuid(HE::UUID id) const;
	const EngineContentManifestEntry* findByPath(const std::string& relativePath) const;
};

// Pure functions — no I/O, safe to unit-test without a real server or filesystem.
HE_CS_API std::string              serializeManifest(const EngineContentManifest& manifest);
HE_CS_API bool                     parseManifest(const std::string& json, EngineContentManifest& out);

} // namespace HE::Cs
