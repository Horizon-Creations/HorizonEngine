#pragma once
#include <imgui.h>
#include <string>
#include <vector>

struct AppContext;

// ── The Sequencer ────────────────────────────────────────────────────────────
// A tab editor for a Property Animation Clip: the asset a Property Animator
// component plays to move, turn, scale or recolour its entity over time. One
// row per animated property, keys on a shared time axis, a playhead you drag.
//
// The strip itself — tracks, ruler, scrubbing, the keys you drag and the curve
// view — is SequencerTimeline, kept free of AppContext so the headless test can
// drive it. This file is the tab around it: which asset, the toolbar (length,
// zoom, the view toggle, adding tracks and keys), the readout under the strip
// where the selected key's time and value are edited, the tab's own undo, and
// the same dirty/save/reload contract every other asset tab honours.
namespace SequencerPanel
{
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// Header sniff (cached) for the double-click / tab dispatch chains.
	bool isSequencerAsset(const std::string& path);

	bool isDirty(const std::string& path);
	bool reloadFromDisk(const std::string& assetPath);

	// The same two questions addressed CONTENT-RELATIVELY, which is how MCP
	// addresses an asset — the states above are keyed by the tab bar's absolute
	// path. Same pair, same reason and same shape as BlendSpacePanel's.
	bool isDirtyByContentPath(const std::string& contentPath);
	bool reloadByContentPath(const std::string& contentPath);
	void appendDirtyPaths(std::vector<std::string>& out);
	bool save(AppContext& ctx, const std::string& path);
	void forget(const std::string& path);
}
