#pragma once
#include "EditorUI.h"
#include <imgui.h>
#include <string>

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

	// Whether the .hasset at `path` is a particle-system asset (reads the HAsset
	// header type; cached per path — same convention as MaterialEditorPanel).
	bool isParticleAsset(const std::string& path);

	// Drop cached editor state for `path` (content-browser rename/delete).
	void forget(const std::string& assetPath);
}
