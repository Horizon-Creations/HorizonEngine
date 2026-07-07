#pragma once
#include <imgui.h>

struct AppContext;

// ── Game Instance editor ─────────────────────────────────────────────────────
// A HorizonCode graph editor for the app-wide GameInstance (one per project).
// It shares the Level Script editor's graph window (same canvas + variables +
// functions), differing only in its event catalog (OnInit / OnShutdown /
// OnWindowFocusChanged) and in that the graph is a project-level asset rather
// than part of a scene. Editing re-registers it with the app runtime and
// persists it via AppContext::commitGameInstance. Togglable from the View menu.
namespace GameInstancePanel
{
	// Sentinel "asset path" identifying the Game Instance editor tab.
	constexpr const char* kTabPath = "::GameInstance::";

	// Fill the given tab rect with the editor (borderless).
	void render(AppContext& ctx, const ImVec2& pos, const ImVec2& size);
}
