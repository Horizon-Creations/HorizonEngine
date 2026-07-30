#pragma once
#include "EditorUI.h"
#include <imgui.h>
#include <string>

// Static Mesh viewer — a top-level tab opened by double-clicking a StaticMesh
// .hasset in the Content Browser (they used to fall through to the raw script
// editor, which showed a binary asset as text).
//
// Its centrepiece is the UV VIEW: the mesh's texture coordinates drawn as a 2D
// wireframe over the 0..1 tile, which is the only way to see how a mesh is
// unwrapped — whether it has UVs at all, whether they overflow the tile (so a
// texture repeats), and whether an import mirrored them. Plus the geometry stats
// (vertices/triangles/bounds) and a UV health summary.
namespace StaticMeshEditorPanel
{
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// Whether the .hasset at `path` is a static mesh (HAsset header sniff, cached
	// per path — same convention as the other asset panels).
	bool isStaticMeshAsset(const std::string& path);

	// Drop cached editor state for `path` (content-browser rename/delete).
	void forget(const std::string& assetPath);
}
