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

	// uuid == HE::UUID{} means "not a .hasset asset" — a raw/loose file (e.g. a
	// source .wav) that ContentManager never registers by UUID. It still
	// downloads and shows up in the Content Browser just fine (both are
	// path-driven, see EngineContentSync::enqueueDownload / mergeManifestInto);
	// it simply can never be the target of a UUID-keyed scene reference. Set
	// from a .hasset's own META chunk when publishing FROM local content
	// (EngineContentPublish::publishEngineContentBlocking), left empty when
	// discovered by scanning the server directly (rebuildManifestFromServerBlocking).
	HE::UUID    uuid;
	// hash64 of the file's bytes (Hpak::hash64 — same function incremental
	// packing uses) — the change-detection key for .hasset entries. 0 for raw
	// entries: computing it would mean downloading every file just to hash it,
	// which defeats the point of a server-side scan (see RemoteFileInfo). Raw
	// entries use `mtime` for change detection instead.
	std::uint64_t contentHash = 0;
	std::uint64_t size        = 0;
	// Only meaningful (and only ever set) when uuid is empty — a raw entry's
	// server-reported modification time, Unix epoch seconds. 0 for .hasset
	// entries, which are change-tracked by contentHash instead.
	std::uint64_t mtime       = 0;
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
