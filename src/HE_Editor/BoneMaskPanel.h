#pragma once
#include <imgui.h>
#include <string>
#include <vector>

struct AppContext;

// ── The Bone Mask editor ─────────────────────────────────────────────────────
// A tab editor for a Bone Mask asset: which joints an animation layer may touch,
// and how strongly.
//
// A mask holds joint NAMES and no skeleton, so the panel needs one to show a
// tree at all. That reference mesh is EDITOR state, kept per open tab and never
// written into the asset — a mask is meant to work on every rig that spells its
// joints the same way, and baking one skeleton into it would quietly make it the
// only one it fits.
namespace BoneMaskPanel
{
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// Header sniff (cached) for the double-click / tab dispatch chains.
	bool isBoneMaskAsset(const std::string& path);

	bool isDirty(const std::string& path);
	bool reloadFromDisk(const std::string& assetPath);
	void appendDirtyPaths(std::vector<std::string>& out);
	bool save(AppContext& ctx, const std::string& path);
	void forget(const std::string& path);
}
