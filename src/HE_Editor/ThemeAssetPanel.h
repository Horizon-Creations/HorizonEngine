#pragma once
#include <imgui.h>
#include <string>
#include <vector>

struct AppContext;

// ── The Theme editor ─────────────────────────────────────────────────────────
// A tab editor for a Theme asset (docs/he-apps-plan.md D1): the nine colour
// roles in their light and dark values side by side, the size steps, the text
// sizes and the two shadows.
//
// Side by side on purpose. A theme's whole job is that light and dark are two
// values of ONE decision, and an editor that shows one mode at a time is an
// editor in which the other mode is forgotten — which shows up as unreadable
// text for whoever switches.
namespace ThemeAssetPanel
{
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// Header sniff (cached) for the double-click / tab dispatch chains.
	bool isThemeAsset(const std::string& path);

	bool isDirty(const std::string& path);
	bool reloadFromDisk(const std::string& assetPath);
	void appendDirtyPaths(std::vector<std::string>& out);
	bool save(AppContext& ctx, const std::string& path);
	void forget(const std::string& path);
}
