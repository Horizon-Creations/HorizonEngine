#pragma once
#ifdef HE_IMGUI_ENABLED
#include "EditorApplication.h"   // AppContext::EditorTab
#include <string>
#include <vector>

class ContentManager;

// ── The editor's top-level tabs: finding one, opening one, keeping them current ─
// Every way of opening an asset's editor (a double-click in the Content Browser,
// "Open Function" on a material node, the console's go-to-node, a finished
// download, the session restore) used to run its own find-or-push over
// `ctx.tabs`, each comparing the paths with `==` on the raw strings. Two routes
// to the SAME file rarely spell its path the same way: the browser's tile comes
// from a directory walk, a material node's reference from
// `ContentManager::resolveAbsolutePath` (a root joined with "/" to a relative
// path, so separators and a trailing slash differ on Windows); an "Engine/..."
// asset is the shipped default on one route and the project's override of it on
// another. The comparison missed, a second tab opened on the file that already
// had one, and the user ended up with two editors writing the same asset.
//
// All of them go through here now, so there is ONE answer to "is this asset
// already open", and it is the one place to improve. Deliberately ImGui-free
// (the editor shell owns the tab bar; this only edits the vector and the two
// ints beside it), so a test can drive it without a window.
namespace EditorTabs
{
	// "::Preferences::", "::LevelScript::" and the like — tabs with no file
	// behind them. They are compared by their exact name and nothing else.
	bool isVirtual(const std::string& assetPath);

	// The spelling two routes to one file agree on: lexically normal, generic
	// separators. Virtual paths and the empty path (the scene tab) come back
	// unchanged.
	std::string key(const std::string& assetPath);

	// Are these two paths the same asset?
	//  1. the same normalised spelling;
	//  2. the same file on disk (std::filesystem::equivalent: symlinks, and a
	//     case-insensitive volume where "Foo" and "foo" are one file);
	//  3. with a ContentManager: the same content-relative path. That is the
	//     asset's identity — the panels load through it (openPanelAsset) and save
	//     through it — so the shipped "Engine/..." default and the project's
	//     override of it are one asset, not two.
	// Paths of different files in different folders, or of different types with
	// one stem ("Rock.hasset" next to "Rock.png"), are never the same.
	bool sameAsset(const std::string& a, const std::string& b,
	               const ContentManager* cm = nullptr);

	// Index of the tab showing `assetPath`, or -1. The empty path (the scene
	// tab) never matches an asset: ask focusSceneTab for that one.
	int find(const std::vector<AppContext::EditorTab>& tabs, const std::string& assetPath,
	         const ContentManager* cm = nullptr);

	// Open `assetPath` in a tab, or bring the one that is already showing it
	// forward. Either way the tab becomes the active one and `selectRequest` (the
	// tab bar's one-shot "select this index next frame") points at it. A tab
	// whose close is still pending (`open == false`, erased by the tab bar on its
	// next pass) is the same tab, so it is kept rather than left to be erased
	// under the new selection. `label` is only used for a tab that has to be
	// created; empty = the file's stem. Returns the tab's index, -1 for an empty
	// path.
	int openOrFocus(std::vector<AppContext::EditorTab>& tabs, int& activeTab,
	                int& selectRequest, const std::string& assetPath,
	                const std::string& label = {}, const ContentManager* cm = nullptr);

	// Bring the scene tab (the one with no asset behind it) forward.
	bool focusSceneTab(const std::vector<AppContext::EditorTab>& tabs, int& activeTab,
	                   int& selectRequest);

	// An asset (or, with `folder`, everything under a folder) moved from `oldPath`
	// to `newPath`: point the tabs showing it at the new location and relabel them.
	// Compared by key only — the old path is gone from disk by now, so there is
	// nothing to ask the filesystem. Returns how many tabs followed.
	int retarget(std::vector<AppContext::EditorTab>& tabs, const std::string& oldPath,
	             const std::string& newPath, bool folder = false);

	// Close every duplicate of an earlier tab (same asset, per sameAsset). For a
	// list that was already saved with duplicates in it. Returns the index each
	// ORIGINAL position ended up at (the removed ones map to their survivor), so
	// the caller can carry an "active tab" index across.
	std::vector<int> dedupe(std::vector<AppContext::EditorTab>& tabs,
	                        const ContentManager* cm = nullptr);
}
#endif // HE_IMGUI_ENABLED
