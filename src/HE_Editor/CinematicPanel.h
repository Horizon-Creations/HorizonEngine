#pragma once
#include <imgui.h>
#include <string>
#include <vector>

struct AppContext;

// ── The Cinematic tab ────────────────────────────────────────────────────────
// The editor for a cinematic Sequence: several actors on one clock, camera
// cuts, skeletal clips, events and sound (docs/sequencer-cinematics-plan.md
// §3.6). Its own tab beside the Sequencer, which stays the editor for a
// Property Animation Clip — two assets, two tabs.
//
// The strip is CinematicTimeline (no AppContext, tested headless); this file
// is the tab around it: which asset, the transport and toolbar, the actors
// (binding the selection, rebinding, the missing ones in red), the readout
// where the selected cut, key, section, event or sound is edited field by
// field, the tab's own undo, and the same dirty/save/reload contract as every
// other asset tab.
//
// The preview picture is the tab's own (IRenderer::RenderWorldPreview, slot 0),
// through the editor camera or through the sequence's live cut. The asset tab
// covers the scene viewport while it is in front, so that picture is the only
// place the preview could show — and it is drawn inside a bracket that writes
// the sequence into the scene and puts every value back before the call
// returns (CinematicPreview.h). Save, play-in-editor, undo and the autosave
// therefore never see a cutscene frame in the level.
namespace CinematicPanel
{
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// Header sniff (cached) for the double-click / tab dispatch chains.
	bool isCinematicAsset(const std::string& path);

	bool isDirty(const std::string& path);
	bool reloadFromDisk(const std::string& assetPath);
	bool isDirtyByContentPath(const std::string& contentPath);
	bool reloadByContentPath(const std::string& contentPath);
	void appendDirtyPaths(std::vector<std::string>& out);
	bool save(AppContext& ctx, const std::string& path);
	void forget(const std::string& path);
}
