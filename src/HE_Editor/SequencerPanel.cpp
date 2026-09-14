#include "SequencerPanel.h"
#include "SequencerTimeline.h"     // the strip: tracks, ruler, scrubbing
#include "EditorApplication.h"     // AppContext
#include "EditorAssetTypeCache.h"  // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h"      // shared per-tab state map
#include "EditorToolbar.h"         // shared toolbar strip
#include "EditorHelp.h"            // Help::Scope — "Sequencer/<label>"
#include "EditorWidgets.h"

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Types/Enums.h>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

struct PanelState
{
	bool           loaded = false;
	bool           dirty  = false;
	std::string    relPath;
	std::string    name;
	HE::UUID       assetId;

	// Where the author is looking: playhead, zoom, selection. Per tab, never
	// written into the asset.
	HE::Ed::Sequencer::View view;

	std::string lastSaveError;
};
AssetPanelState<PanelState> s_states;

// The loaded asset IS the edit buffer: what the strip shows is what a Property
// Animator in the scene plays. Nothing in this step edits it — the keys are
// looked at and selected, not moved — so the pointer is fetched per frame and
// never held, because any load moves it (ContentManager.h).
const PropertyAnimClipAsset* clipOf(const PanelState& st, AppContext& ctx)
{
	return ctx.contentManager ? ctx.contentManager->getPropertyAnimClip(st.assetId) : nullptr;
}

bool saveState(PanelState& st, AppContext& ctx)
{
	if (!ctx.contentManager) return false;
	st.lastSaveError.clear();
	PropertyAnimClipAsset* a = ctx.contentManager->getPropertyAnimClipMutable(st.assetId);
	if (!a)
	{
		st.lastSaveError = "Not saved: this clip is no longer loaded.";
		return false;
	}
	if (!ctx.contentManager->saveAsset(*a))
	{
		st.lastSaveError = "Not saved: the file could not be written.";
		return false;
	}
	st.dirty = false;
	return true;
}

// Where we are, in the unit the clip is actually in — the strip's own rule.
void fmtTime(char* buf, size_t n, float t, float duration)
{
	if (duration <= 2.0f) std::snprintf(buf, n, "%.0f ms", t * 1000.0f);
	else                  std::snprintf(buf, n, "%.2f s", t);
}

// The row under the strip: the selected key's numbers, or what to click to
// get some. Read-only in this step; the authoring step turns it into the
// place a key is edited, the way the UI Designer's strip does it.
void drawSelectionReadout(const PanelState& st, const PropertyAnimClipAsset& clip)
{
	HE::Ed::Help::Scope helpScope("Sequencer");
	ImGui::Separator();
	const HE::Ed::Sequencer::View& v = st.view;
	const bool haveTrack = v.trackSel >= 0 && v.trackSel < static_cast<int>(clip.channels.size());
	if (!haveTrack)
	{
		ImGui::TextDisabled("Click a track to select it, a key to see its time and value.");
		return;
	}
	const PropertyAnimChannel& ch = clip.channels[v.trackSel];
	const int keyCount = static_cast<int>(std::min(ch.times.size(), ch.values.size()));
	ImGui::Text("%s", HE::Ed::Sequencer::targetName(ch.target));
	ImGui::SameLine();
	ImGui::TextDisabled("%s  ·  %d key%s", HE::Ed::Sequencer::targetGroup(ch.target),
	                    keyCount, keyCount == 1 ? "" : "s");
	if (v.keySel < 0 || v.keySel >= keyCount)
	{
		ImGui::TextDisabled("Click a key on this track to see when it happens and what it holds.");
		return;
	}
	char tb[24], vb[32];
	fmtTime(tb, sizeof(tb), ch.times[v.keySel], clip.duration);
	HE::Ed::Sequencer::formatValue(ch.target, ch.values[v.keySel], vb, sizeof(vb));
	ImGui::Text("Key %d of %d", v.keySel + 1, keyCount);
	ImGui::SameLine(); ImGui::TextDisabled("at");
	ImGui::SameLine(); ImGui::Text("%s", tb);
	ImGui::SameLine(); ImGui::TextDisabled("holds");
	ImGui::SameLine(); ImGui::Text("%s", vb);
}

} // namespace

bool SequencerPanel::isSequencerAsset(const std::string& path)
{ return EditorAssetTypeCache::is(path, HE::AssetType::PropertyAnimClip); }

bool SequencerPanel::isDirty(const std::string& path) { return s_states.dirty(path); }

bool SequencerPanel::reloadFromDisk(const std::string& assetPath)
{
	auto* st = s_states.find(assetPath);
	if (!st) return false;
	st->loaded = false;
	st->dirty  = false;
	return true;
}

// ── Addressed content-relatively ─────────────────────────────────────────────
// This panel's states are keyed by the tab bar's ABSOLUTE path; MCP addresses
// assets content-relatively. Same walk as BlendSpacePanel's, for the same
// reason, and deliberately NOT keyed on `loaded`.
namespace
{
PanelState* stateByContentPath(const std::string& contentPath)
{
	if (contentPath.empty()) return nullptr;
	PanelState* found = nullptr;
	s_states.forEach([&](const std::string&, PanelState& st) {
		if (!found && st.relPath == contentPath) found = &st;
	});
	return found;
}
} // namespace

bool SequencerPanel::isDirtyByContentPath(const std::string& contentPath)
{
	const PanelState* st = stateByContentPath(contentPath);
	return st && st->dirty;
}

bool SequencerPanel::reloadByContentPath(const std::string& contentPath)
{
	PanelState* st = stateByContentPath(contentPath);
	if (!st) return false;
	st->loaded = false;
	st->dirty  = false;
	return true;
}

void SequencerPanel::appendDirtyPaths(std::vector<std::string>& out)
{ s_states.appendDirtyPaths(out); }

bool SequencerPanel::save(AppContext& ctx, const std::string& path)
{
	PanelState* st = s_states.find(path);
	if (!st || !st->dirty) return true;   // "not mine" reads as success
	return saveState(*st, ctx);
}

void SequencerPanel::forget(const std::string& path) { s_states.forget(path); }

void SequencerPanel::render(AppContext& ctx, const std::string& assetPath,
                            const ImVec2& pos, const ImVec2& size)
{
	PanelState& st = s_states[assetPath];
	if (!st.loaded && ctx.contentManager)
	{
		st.relPath = ctx.contentManager->toContentRelativePath(assetPath);
		st.assetId = ctx.contentManager->loadAsset(st.relPath);
		st.name    = std::filesystem::path(assetPath).stem().string();
		st.view    = HE::Ed::Sequencer::View{};
		st.loaded  = true;
	}

	HE::Ed::Help::Scope helpScope("Sequencer");
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin(("##sequencer_" + assetPath).c_str(), nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

	const PropertyAnimClipAsset* clip = clipOf(st, ctx);

	{
		namespace T = EditorToolbar;
		T::Bar bar;
		T::assetHeader(bar, st.name.c_str(), T::iconPlay, st.dirty);
		bar.group();
		bar.readout(nullptr, "Property Animation", T::kFgDim);
		bar.endGroup();
		// Greyed until there is something to write: nothing in this step edits
		// the clip, and a Save that can only say "nothing to save" is better
		// shown disabled than pressed in vain.
		if (T::saveButton(bar, st.dirty && clip != nullptr)) saveState(st, ctx);
	}
	if (!st.lastSaveError.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220, 80, 80, 255));
		ImGui::TextWrapped("%s", st.lastSaveError.c_str());
		ImGui::PopStyleColor();
	}

	if (!clip)
	{
		// Said plainly. The file is a Property Animation Clip — the header says
		// so, which is how this tab opened — but nothing produced a clip from
		// it, so there is nothing to draw and no reason to pretend otherwise.
		EditorWidgets::WrapText wrap;
		ImGui::Spacing();
		ImGui::TextDisabled("This clip could not be loaded, so there is nothing to show.");
		ImGui::TextDisabled("The file is recognised as a Property Animation Clip, but no clip came");
		ImGui::TextDisabled("out of it — it may be empty, damaged, or written by a newer editor.");
		ImGui::End();
		return;
	}

	// ── The bar under the toolbar: length, position, zoom ────────────────────
	int transformTracks = 0, materialTracks = 0;
	for (const PropertyAnimChannel& ch : clip->channels)
		(HE::Ed::Sequencer::targetGroup(ch.target)[0] == 'T' ? transformTracks : materialTracks)++;

	char lenTxt[24], nowTxt[24];
	fmtTime(lenTxt, sizeof(lenTxt), clip->duration, clip->duration);
	fmtTime(nowTxt, sizeof(nowTxt), st.view.playhead, clip->duration);
	ImGui::TextDisabled("%s / %s", nowTxt, lenTxt);
	ImGui::SameLine();
	ImGui::TextDisabled("·  %zu track%s (%d transform, %d material)",
	                    clip->channels.size(), clip->channels.size() == 1 ? "" : "s",
	                    transformTracks, materialTracks);

	// The zoom controls sit here, but they can only be APPLIED once the lane's
	// width is known inside the strip — zooming around a point needs the point
	// in pixels. So the bar records the intent and the strip acts on it.
	HE::Ed::Sequencer::Intent intent;
	ImGui::SameLine();
	if (EditorWidgets::smallButton("Zoom Out")) intent.zoom = -1;
	ImGui::SameLine();
	if (EditorWidgets::smallButton("Zoom In")) intent.zoom = 1;
	ImGui::SameLine();
	if (EditorWidgets::smallButton("Fit")) intent.fit = true;
	if (st.view.zoom > 1.001f)
	{ ImGui::SameLine(); ImGui::TextDisabled("%.0f%%", st.view.zoom * 100.0f); }

	// ── The strip, then the selection under it ───────────────────────────────
	// The readout's height is fixed and taken off the strip, so the strip is
	// always what is left and the readout never scrolls out from under it.
	const float readoutH = ImGui::GetTextLineHeightWithSpacing() * 2.0f +
	                       ImGui::GetStyle().ItemSpacing.y * 3.0f + 6.0f;
	const float stripH = std::max(80.0f, ImGui::GetContentRegionAvail().y - readoutH);
	HE::Ed::Sequencer::draw(*clip, st.view, ImVec2(0.0f, stripH), intent);

	drawSelectionReadout(st, *clip);

	ImGui::End();
}
