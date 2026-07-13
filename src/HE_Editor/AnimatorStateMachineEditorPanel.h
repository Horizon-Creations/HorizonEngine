#pragma once
#include "EditorUI.h"
#include <imgui.h>
#include <string>

// The Animator State Machine graph editor — states as nodes, transitions as
// links, on the same shared node-graph canvas (GraphEditor) as Material/
// HorizonCode/ParticleGraph. The graph (states/transitions/default params,
// HE::AnimatorStateMachineGraph) is the source of truth stored in
// AnimatorStateMachineAsset; AnimatorStateMachineComponent::stateMachineAssetId
// references it instead of embedding the graph inline (the same move Material
// and ParticleSystem made for their own components). Opened by double-clicking
// the .hasset in the Content Browser, same as Material/HorizonCode/Particle.
namespace AnimatorStateMachineEditorPanel
{
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// Whether the .hasset at `path` is an Animator State Machine asset (reads
	// the HAsset header type; cached per path — same convention as
	// MaterialEditorPanel::isMaterialAsset).
	bool isAnimatorStateMachineAsset(const std::string& path);

	// Drop cached editor state for `path` (content-browser rename/delete).
	void forget(const std::string& assetPath);
}
