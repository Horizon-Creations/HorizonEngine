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
	// Draw the window when `open`. Clears `open` when the user closes it. No-op
	// without a world (no scene loaded).
	void render(AppContext& ctx, bool& open);
}
