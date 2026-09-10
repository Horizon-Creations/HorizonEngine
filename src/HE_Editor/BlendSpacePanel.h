#pragma once
#include <imgui.h>
#include <string>
#include <vector>

struct AppContext;

// ── The Blend Space editor ───────────────────────────────────────────────────
// A tab editor for a Blend Space asset: N animation clips placed in a 1D or 2D
// parameter space, mixed by where the parameters stand.
//
// A diagram with draggable points and not a node graph, deliberately. The
// animation half of this editor already decided against the GraphEditor node
// model (AnimatorStateMachineGraph.h says so in its header), and a blend space
// is not a graph anyway: it is a point cloud in a parameter space. Its natural
// editor is the picture of that space, with a cursor you drag to see the mix.
namespace BlendSpacePanel
{
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// Header sniff (cached) for the double-click / tab dispatch chains.
	bool isBlendSpaceAsset(const std::string& path);

	bool isDirty(const std::string& path);
	bool reloadFromDisk(const std::string& assetPath);

	// The same two questions addressed CONTENT-RELATIVELY, which is how MCP
	// addresses an asset — the states above are keyed by the tab bar's absolute
	// path. Same pair, same reason and same shape as InputAssetPanel's.
	bool isDirtyByContentPath(const std::string& contentPath);
	bool reloadByContentPath(const std::string& contentPath);
	void appendDirtyPaths(std::vector<std::string>& out);
	bool save(AppContext& ctx, const std::string& path);
	void forget(const std::string& path);
}
