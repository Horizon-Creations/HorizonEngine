#pragma once
#include <imgui.h>
#include <string>

struct AppContext;

// ── HorizonCode Class editor ─────────────────────────────────────────────────
// A tab editor for a standalone HorizonCode class asset (AssetType::
// HorizonCodeClass). Shares the graph window (canvas + variables + functions)
// with the Level Script / Game Instance editors; a class names its own events
// freely (no fixed catalog). State is cached per asset path across tab switches.
namespace HorizonCodeClassPanel
{
	// Fill the tab rect with the editor for the class asset at `assetPath`.
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// True if the .hasset at `path` is a HorizonCode class (header sniff).
	bool isClassAsset(const std::string& path);
	// True if the cached editor for `path` has unsaved edits (tab dirty mark).
	bool isDirty(const std::string& path);
	// Drop the cached editor state (tab closed without unsaved edits).
	void forget(const std::string& path);
}
