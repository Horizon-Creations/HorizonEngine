#pragma once
#include "EditorUI.h"
#include <imgui.h>
#include <string>
#include <vector>

// The particle emitter node graph editor (HE::ParticleGraph) — a top-level tab
// opened by double-clicking a ParticleSystem .hasset in the Content Browser.
// Shares the same node-graph canvas (GraphEditor) as the Material and
// HorizonCode editors. The graph (nodeGraphJson) is the source of truth stored
// in ParticleGraphAsset; ParticleSystemComponent::particleAssetId references it
// instead of embedding emitter config inline (the same move Material made for
// MaterialComponent). Live preview simulates a scratch particle pool with
// ParticleSystem::stepPool (HE_Scene) and renders it via
// IRenderer::RenderParticlePreview — the renderer itself never simulates.
namespace ParticleGraphEditorPanel
{
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// True if the graph has edits not yet saved to disk (drives the tab's dirty mark
	// and keeps the close-tab/exit unsaved-changes check from dropping the state).
	bool isDirty(const std::string& assetPath);
	// Paths of every unsaved tab this panel holds, open or already closed.
	// See AssetPanelState::appendDirtyPaths — a closed dirty tab keeps its
	// state but leaves the tab vector, so the quit guard must ask here.
	void appendDirtyPaths(std::vector<std::string>& out);

	// Write this tab's graph to disk, exactly like the header's Save button — so
	// the close/quit prompt can save this asset without the user having to walk
	// back into the tab. Returns true when nothing is left unsaved for this path
	// (including the "this panel never held it" case).
	bool save(AppContext& ctx, const std::string& assetPath);

	// Whether the .hasset at `path` is a particle-system asset (reads the HAsset
	// header type; cached per path — same convention as MaterialEditorPanel).
	bool isParticleAsset(const std::string& path);

	// Drop cached editor state for `path` (content-browser rename/delete).
	void forget(const std::string& assetPath);
}
