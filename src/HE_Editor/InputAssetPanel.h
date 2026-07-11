#pragma once
#include <imgui.h>
#include <string>

struct AppContext;

// ── Input asset editors ──────────────────────────────────────────────────────
// Tab editors for the two input asset types: an Input Action (Button/Axis
// value type) and an Input Mapping Context (key/axis bindings per action).
// Both edit the asset's JSON payload; state is cached per asset path across
// tab switches like the other tab editors.
namespace InputAssetPanel
{
	// Fill the tab rect with the editor for the input asset at `assetPath`.
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// Header sniffs (cached) for the double-click/tab dispatch chains.
	bool isInputActionAsset(const std::string& path);
	bool isInputMappingAsset(const std::string& path);
	bool isInputAsset(const std::string& path); // either of the two

	// True if the cached editor for `path` has unsaved edits (tab dirty mark).
	bool isDirty(const std::string& path);
	// Drop the cached editor state (tab closed without unsaved edits).
	void forget(const std::string& path);
}
