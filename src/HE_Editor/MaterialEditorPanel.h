#pragma once
#include "CollabDocSync.h"
#include "EditorUI.h"
#include <imgui.h>
#include <string>
#include <vector>

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

	// Re-read the file on the next frame (collab: a peer's change landed).
	bool reloadFromDisk(const std::string& assetPath);
	// Paths of every unsaved tab this panel holds, open or already closed.
	// See AssetPanelState::appendDirtyPaths — a closed dirty tab keeps its
	// state but leaves the tab vector, so the quit guard must ask here.
	void appendDirtyPaths(std::vector<std::string>& out);

	// Write this tab's graph/overrides to disk, exactly like the header's Save
	// button — so the close/quit prompt can save this asset without the user
	// having to walk back into the tab. Returns true when nothing is left unsaved
	// for this path (including the "this panel never held it" case).
	bool save(AppContext& ctx, const std::string& assetPath);

	// Whether the .hasset at `path` is a material / material-function asset
	// (reads the HAsset header type; cached per path).
	bool isMaterialAsset(const std::string& path);
	bool isMaterialFunctionAsset(const std::string& path);

	// Drop cached editor state for `path`.
	void forget(const std::string& assetPath);

	// Hand back the assets this tab's PREVIEW pulled into memory — a picked static
	// mesh that was not resident before. Call when the tab closes, before forget()
	// and regardless of whether the state itself is kept (a dirty tab keeps its
	// graph, but the megabytes behind its preview mesh have no reason to stay).
	// A mesh the scene or another material tab still references is left alone.
	void releasePreviewAssets(AppContext& ctx, const std::string& assetPath);

	// The live documents behind this tab, for collaboration's item-level sync.
	// Empty when this panel does not hold `assetPath` — same "ask everyone, the
	// owner answers" dispatch as save() and reloadFromDisk().
	CollabDocSync::DocBindings collabDocs(const std::string& assetPath);


	// One-shot "open this asset in an editor tab" request, set by the canvas (e.g.
	// double-clicking a Material Function node) and consumed by EditorUI's tab bar.
	// Returns the absolute asset path and clears the request; "" when none pending.
	std::string takeOpenRequest();
}
