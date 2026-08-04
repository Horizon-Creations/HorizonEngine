#pragma once

#include "CollabDocSync.h"

#include <string>
#include <vector>

struct AppContext;

class EditorUI
{
public:
	static void render(AppContext& ctx, float dt);

	// Blocks until a project export running on the worker thread has finished.
	// Must be called on editor shutdown — destroying a joinable std::thread
	// terminates the process.
	static void joinPendingExport();

	// True if the editor tab for `assetPath` holds edits that were never written
	// to disk (see the definition in EditorUI.cpp for the panel list). Exposed so
	// EditorApplication's OS-close veto (window X / Cmd+Q / app quit) can veto for
	// dirty asset tabs too — those never touch the world undo revision, so the
	// scene-dirty test alone lets them be lost without a prompt.
	static bool tabHasUnsavedEdits(const std::string& assetPath);
	// Every unsaved asset, including ones whose tab was already closed — the
	// quit guard needs that wider view (a closed dirty tab keeps its panel
	// state but leaves ctx.tabs).
	static std::vector<std::string> unsavedAssetPaths();
	// Write the asset at `assetPath` through whichever panel is holding its edits
	// (the counterpart of tabHasUnsavedEdits). Lets the close/quit prompt save an
	// asset tab itself instead of sending the user back into the tab — which for a
	// tab the user already CLOSED meant reopening it first. Returns true when
	// nothing is left unsaved for that path.
	static bool saveAsset(AppContext& ctx, const std::string& assetPath);

	// Drop the in-memory content of whichever panel holds this tab and reload it
	// from disk on its next frame, keeping the panel's view state (pan/zoom,
	// camera). Used when a collaboration peer's change just landed in the file —
	// the open tab must show it, not the state from before. Returns true if some
	// panel was actually holding the path.
	static bool reloadAssetTabFromDisk(const std::string& assetPath);

	// The live documents behind an open tab, for collaboration's item-level sync
	// (CollabDocSync). Same dispatch as the three above. Empty for tabs with no
	// syncable document. The adapters are views: they must not outlive the frame,
	// and the mirrors they point at belong to the panel.
	static CollabDocSync::DocBindings collabDocsFor(const std::string& assetPath);

private:
	static void renderEditor(AppContext& ctx, float dt);
	// Floating tool windows (Preferences, Profiler, Environment) and the guided
	// tour. Deliberately NOT part of renderEditor: that returns early when an asset
	// tab is active, and all of these must stay usable — and the tour must keep
	// sampling — while the user is inside a material graph or a HorizonCode tab.
	static void renderOverlays(AppContext& ctx, float dt);
};
