#pragma once
#include <imgui.h>

struct AppContext;

// ── Level Script editor ──────────────────────────────────────────────────────
// A HorizonCode graph editor for the CURRENT scene's level script (one per
// level, like Unreal's Level Blueprint). Unlike the UI Widget editor it is not
// bound to a widget or its elements: the graph reacts to world events
// ("OnLevelLoaded" / "OnLevelUnloaded") and drives level-wide logic through
// variables, flow, math and Print (engine-system nodes come later). It is a
// togglable window (View menu), editing HorizonWorld::levelScript() in place;
// edits snapshot through the editor undo system so they save with the scene.
namespace LevelScriptPanel
{
	// Sentinel "asset path" identifying the Level Script editor tab (it isn't a
	// real .hasset — the graph lives in the scene).
	constexpr const char* kTabPath = "::LevelScript::";

	// Fill the given tab rect with the editor (borderless). No-op without a world.
	void render(AppContext& ctx, const ImVec2& pos, const ImVec2& size);
}
