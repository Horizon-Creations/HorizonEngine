#pragma once
#include <Types/Enums.h>
#include <string>

// ── Editor-wide path → asset-type cache ──────────────────────────────────────
// Every asset panel dispatches on "is the .hasset at this path MY type?", and the
// active tab asks that question once per frame, so the HAsset header sniff has to
// be cached. Each panel used to keep its OWN static map and none of them was ever
// invalidated: deleting an asset and creating a DIFFERENT type at the same path
// kept answering with the dead asset's type, so double-clicking opened the wrong
// editor for the rest of the session. This single
// cache replaces those maps and can be invalidated — the Content Browser drops the
// affected paths on delete/rename/move, and every content-folder refresh (manual,
// quiet, or the periodic external-change poll) drops the whole map.
//
// UI-thread only (all callers run inside the ImGui frame) — deliberately no locking.
namespace EditorAssetTypeCache
{
	// Asset type from the HAsset header at `path`; HE::AssetType::Unknown when the
	// file is missing or is not an HAsset. Misses are cached too (so a non-asset
	// path costs one open, not one per frame) — hence the invalidation hooks.
	HE::AssetType assetTypeOf(const std::string& path);

	// Backing call for the panels' isXAsset() predicates.
	bool is(const std::string& path, HE::AssetType type);

	// Drop the cached entry for `path` — call whenever what lives at that path
	// changes: delete, rename/move (BOTH the old and the new path), create.
	void invalidate(const std::string& path);

	// Drop every entry — call after a content-folder refresh, where assets may have
	// appeared, vanished or been replaced behind the editor's back.
	void invalidateAll();
}
