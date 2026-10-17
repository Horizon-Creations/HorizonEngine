#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

// ── Entities in the host's in-tray ───────────────────────────────────────────
// Deleting or renaming an ASSET has always been the host's decision: it
// destroys or moves somebody else's work. An entity somebody else is editing is
// the same case — deleting it pulls it out from under their hands — and it used
// to be refused outright with "try again once they have deselected it", which
// left the requester nobody to ask.
//
// It now rides the asset-op channel unchanged (CollabSession::AssetOp Delete /
// Rename), under a key no file can have, the way C++ classes ride it under
// EditorApplication::kSourceKeyPrefix. The wire shape stays as it is, so the
// protocol version does not move: `path` carries this key, `newPath` the new
// NAME on a rename. Everything in the editor that reads `path` as a file
// (approveOne's occupied-name check, verifyAppliedAssetOp, the apply handler)
// branches on isEntityOpKey first.
//
// Only a CONFLICT is asked about: an entity nobody else holds is deleted and
// renamed on the spot, exactly as before (collab-sync-audit step 2). The host
// is asked only when a peer holds the entity or something under it.
namespace HE::Ed::CollabEntityOps
{
	inline constexpr const char* kKeyPrefix = "entity:";

	// The subject is the uuid-derived wire id (CollabController::subjectFor), so
	// every peer resolves the key to the same entity in its own world.
	inline std::string keyFor(std::uint64_t subject)
	{
		char buf[32];
		std::snprintf(buf, sizeof buf, "%s%016llx", kKeyPrefix,
		              static_cast<unsigned long long>(subject));
		return buf;
	}

	inline bool isKey(const std::string& path)
	{
		return path.rfind(kKeyPrefix, 0) == 0;
	}

	// 0 = not an entity key, or a malformed one. A subject is never 0 (the wire
	// id sets a bit to avoid exactly that), so 0 is free to mean "no".
	inline std::uint64_t subjectOf(const std::string& path)
	{
		if (!isKey(path)) return 0;
		const std::string hex = path.substr(std::char_traits<char>::length(kKeyPrefix));
		if (hex.empty() || hex.size() > 16) return 0;
		char* end = nullptr;
		const unsigned long long v = std::strtoull(hex.c_str(), &end, 16);
		if (!end || *end != '\0') return 0;
		return static_cast<std::uint64_t>(v);
	}
}
