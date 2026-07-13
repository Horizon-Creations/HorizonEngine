#pragma once
#include "EditorUI.h"
#include <HorizonScene/HorizonWorld.h> // Entity = entt::entity
#include <imgui.h>
#include <string>

// The Animator State Machine graph editor — states as nodes, transitions as
// links, on the same shared node-graph canvas (GraphEditor) as Material/
// HorizonCode/ParticleGraph. Unlike those, AnimatorStateMachineComponent lives
// on an ECS entity, not a standalone .hasset — a scene can have several state
// machines (one per animated entity), so the tab is opened per-entity via a
// synthetic path (kTabPrefix + entity id) rather than a Content-Browser
// double-click, with its own "Open in State Machine Editor" button in the
// Inspector. Transition-rule fine-editing (from/to/param/op/threshold/duration)
// stays in a flat side-panel list (GraphEditor links carry no inline widgets —
// the graph gives topology + layout, the list gives parameter editing).
namespace AnimatorStateMachineEditorPanel
{
	constexpr const char* kTabPrefix = "::AnimStateMachine::";

	// Build/recognize the per-entity virtual tab path.
	std::string tabPathFor(Entity entity);
	bool        isStateMachineTab(const std::string& path);

	void render(AppContext& ctx, const std::string& tabPath, const ImVec2& pos, const ImVec2& size);
}
