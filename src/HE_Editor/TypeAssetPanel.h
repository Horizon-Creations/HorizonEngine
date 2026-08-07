#pragma once
#include <imgui.h>
#include <string>
#include <vector>

struct AppContext;

// ── User-defined type editors (Struct / Enum assets) ─────────────────────────
// Tab editors for the two type-definition assets: a Struct (named typed fields
// with defaults) and an Enum (named int-backed entries). Both edit the asset's
// JSON payload (CHUNK_STDF / CHUNK_ENDF, decoded through HE::TypeRegistry's
// round-trip) and re-register the definition in the TypeRegistry on save, so
// type dropdowns across the editor update immediately. State is cached per
// asset path across tab switches like the other tab editors.
namespace TypeAssetPanel
{
	// Fill the tab rect with the editor for the type asset at `assetPath`.
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// Header sniffs (cached) for the double-click/tab dispatch chains.
	bool isStructAsset(const std::string& path);
	bool isEnumAsset(const std::string& path);
	bool isSaveTemplateAsset(const std::string& path);
	bool isTypeAsset(const std::string& path); // any of the three

	// True if the cached editor for `path` has unsaved edits (tab dirty mark).
	bool isDirty(const std::string& path);

	// Re-read the file on the next frame (collab: a peer's change landed).
	bool reloadFromDisk(const std::string& assetPath);
	// Paths of every unsaved tab this panel holds, open or already closed.
	void appendDirtyPaths(std::vector<std::string>& out);
	// Write this tab's edits back into the asset and to disk (the close/quit
	// prompt's Save All). True when nothing is left unsaved for this path.
	bool save(AppContext& ctx, const std::string& path);
	// Drop the cached editor state (tab closed without unsaved edits).
	void forget(const std::string& path);
}
