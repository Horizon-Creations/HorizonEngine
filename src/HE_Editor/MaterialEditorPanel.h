#pragma once
#include "EditorUI.h"
#include <imgui.h>
#include <string>

// The material node-graph editor (M3) — a top-level editor tab opened by double-clicking
// a material asset in the Content Browser. Edits the HE::MaterialGraph stored in the
// MaterialAsset (nodeGraphJson = source of truth); every change regenerates the GLSL
// fragment (customShaderFragGlsl) through HE::generateFragmentGlsl, which the renderers
// pick up live (per-hash pipeline cache). Shaders are not a user-facing asset type — this
// graph IS the shader authoring surface.
//
// Panel state is keyed by asset path (survives tab switches / close+reopen), mirroring
// ScriptEditorPanel.
namespace MaterialEditorPanel
{
	// Render the editor for the material at `assetPath`, filling the given rect.
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// True if the graph has edits not yet saved to disk (drives the tab's dirty mark).
	bool isDirty(const std::string& assetPath);

	// Whether the .hasset at `path` is a material / material-function asset
	// (reads the HAsset header type; cached per path).
	bool isMaterialAsset(const std::string& path);
	bool isMaterialFunctionAsset(const std::string& path);

	// Drop cached editor state for `path`.
	void forget(const std::string& assetPath);
}
