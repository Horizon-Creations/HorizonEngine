#include "SequencerPanel.h"
#include "SequencerTimeline.h"     // the strip: tracks, ruler, scrubbing, keys, curves
#include "EditorApplication.h"     // AppContext
#include "EditorAssetTypeCache.h"  // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h"      // shared per-tab state map
#include "EditorToolbar.h"         // shared toolbar strip
#include "EditorHelp.h"            // Help::Scope — "Sequencer/<label>"
#include "EditorWidgets.h"

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/PropertyAnimatorComponent.h>
#include <HorizonScene/PropertyAnimationSystem.h> // sampleChannel — Add Key pins the curve; applyAt — the preview
#include <Types/Enums.h>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

// What undo remembers: the clip's editable part, whole. A clip is a few
// hundred floats at most, so a copy per edit is cheaper than a diff would be
// to get right.
struct Snapshot
{
	float                            duration = 0.0f;
	std::vector<PropertyAnimChannel> channels;
	bool operator==(const Snapshot& o) const
	{
		if (duration != o.duration || channels.size() != o.channels.size()) return false;
		for (size_t i = 0; i < channels.size(); ++i)
			if (channels[i].target != o.channels[i].target ||
			    channels[i].times  != o.channels[i].times  ||
			    channels[i].values != o.channels[i].values) return false;
		return true;
	}
};

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

	// This tab's own undo, like the UI Designer's: an asset editor with its
	// own dirty flag and save button has nothing to do with the world's undo
	// stack (BlendSpacePanel says the same). Position -1 means no baseline yet.
	std::vector<Snapshot> undo;
	int undoPos = -1;

	std::string lastSaveError;
};
AssetPanelState<PanelState> s_states;

// The loaded asset IS the edit buffer: what the strip shows and edits is what
// a Property Animator in the scene plays. The pointer is fetched per frame and
// never held, because any load moves it (ContentManager.h).
PropertyAnimClipAsset* clipOf(const PanelState& st, AppContext& ctx)
{
	return ctx.contentManager ? ctx.contentManager->getPropertyAnimClipMutable(st.assetId) : nullptr;
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

// ── Undo ─────────────────────────────────────────────────────────────────────
Snapshot snapshotOf(const PropertyAnimClipAsset& clip)
{
	return Snapshot{ clip.duration, clip.channels };
}

// The clip as it is now becomes the newest undo point. Same as the state
// already on top: nothing, so a click that changed nothing costs no step.
void pushUndo(PanelState& st, const PropertyAnimClipAsset& clip)
{
	Snapshot snap = snapshotOf(clip);
	if (st.undoPos >= 0 && st.undoPos < static_cast<int>(st.undo.size()) &&
	    st.undo[st.undoPos] == snap)
		return;
	st.undo.resize(static_cast<size_t>(st.undoPos + 1));
	st.undo.push_back(std::move(snap));
	if (st.undo.size() > 64) st.undo.erase(st.undo.begin());
	st.undoPos = static_cast<int>(st.undo.size()) - 1;
}

bool restoreSnapshot(PanelState& st, PropertyAnimClipAsset& clip, int pos)
{
	if (pos < 0 || pos >= static_cast<int>(st.undo.size())) return false;
	clip.duration = st.undo[pos].duration;
	clip.channels = st.undo[pos].channels;
	st.undoPos = pos;
	// The selection may point past what came back; the strip drops it on its
	// next frame, but a drag in flight must not carry on over a different key.
	st.view.keyArmed = false;
	st.view.dragging = false;
	st.dirty = true;
	return true;
}

// An edit that is done: dirty, and an undo point.
void commit(PanelState& st, const PropertyAnimClipAsset& clip)
{
	st.dirty = true;
	pushUndo(st, clip);
}

// Where we are, in the unit the clip is actually in — the strip's own rule.
void fmtTime(char* buf, size_t n, float t, float duration)
{
	if (duration <= 2.0f) std::snprintf(buf, n, "%.0f ms", t * 1000.0f);
	else                  std::snprintf(buf, n, "%.2f s", t);
}

// ── The toolbar row under the header ─────────────────────────────────────────
// Length, position, zoom, the view toggle, and the buttons that add and remove.
// Records the zoom intent for the strip; edits the clip directly otherwise.
void drawControls(PanelState& st, PropertyAnimClipAsset& clip, HE::Ed::Sequencer::Intent& intent)
{
	namespace Seq = HE::Ed::Sequencer;
	// render() has pushed this already; pushed again here because the help
	// audit reads the file top to bottom and this function stands above it.
	HE::Ed::Help::Scope helpScope("Sequencer");
	int transformTracks = 0, materialTracks = 0, otherTracks = 0;
	for (const PropertyAnimChannel& ch : clip.channels)
	{
		const char* group = Seq::targetGroup(ch.target);
		if (std::strcmp(group, "Transform") == 0)     ++transformTracks;
		else if (std::strcmp(group, "Material") == 0) ++materialTracks;
		else                                          ++otherTracks;   // camera FOV, visibility
	}

	// ── The transport ────────────────────────────────────────────────────────
	// Play runs the playhead at the clip's own pace and drives every entity in
	// the scene that plays this clip (see previewActors); Stop puts it back to
	// the start. The button says where it GOES, like the view toggle below.
	// The advance itself happens in render(), before this row, so the time
	// printed beside the buttons is this frame's.
	if (EditorWidgets::smallButton(st.view.playing ? "Pause##seq_play" : "Play##seq_play"))
		st.view.playing = !st.view.playing;
	EditorWidgets::helpForKey("sequencer.play");
	ImGui::SameLine();
	if (EditorWidgets::smallButton("Stop"))
	{
		st.view.playing  = false;
		st.view.playhead = 0.0f;
	}
	ImGui::SameLine();
	EditorWidgets::checkbox("Loop##seq_loop", &st.view.loop);
	ImGui::SameLine();
	ImGui::TextDisabled("·");
	ImGui::SameLine();

	char lenTxt[24], nowTxt[24];
	fmtTime(lenTxt, sizeof(lenTxt), clip.duration, clip.duration);
	fmtTime(nowTxt, sizeof(nowTxt), st.view.playhead, clip.duration);
	ImGui::TextDisabled("%s / %s", nowTxt, lenTxt);
	ImGui::SameLine();
	if (otherTracks > 0)
		ImGui::TextDisabled("·  %zu track%s (%d transform, %d material, %d other)",
		                    clip.channels.size(), clip.channels.size() == 1 ? "" : "s",
		                    transformTracks, materialTracks, otherTracks);
	else
		ImGui::TextDisabled("·  %zu track%s (%d transform, %d material)",
		                    clip.channels.size(), clip.channels.size() == 1 ? "" : "s",
		                    transformTracks, materialTracks);

	// The clip's length, editable. It cannot go under the last key — a key
	// past the end would be one nobody could reach (SequencerTimeline.h,
	// setDuration) — so the field's floor is that key.
	ImGui::SameLine();
	ImGui::TextDisabled("·");
	ImGui::SameLine();
	ImGui::Text("Length");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(80.0f);
	float len = clip.duration;
	const float floor = std::max(Seq::lastKeyTime(clip), 0.01f);
	if (ImGui::DragFloat("##seq_length", &len, 0.01f, floor, 3600.0f, "%.2f s"))
	{
		Seq::setDuration(clip, std::max(len, floor));
		st.dirty = true;
	}
	EditorWidgets::helpForKey("sequencer.length");
	if (ImGui::IsItemDeactivatedAfterEdit()) pushUndo(st, clip);

	// The zoom controls sit here, but they can only be APPLIED once the lane's
	// width is known inside the strip — zooming around a point needs the point
	// in pixels. So the bar records the intent and the strip acts on it.
	ImGui::SameLine();
	ImGui::TextDisabled("·");
	ImGui::SameLine();
	if (EditorWidgets::smallButton("Zoom Out")) intent.zoom = -1;
	ImGui::SameLine();
	if (EditorWidgets::smallButton("Zoom In")) intent.zoom = 1;
	ImGui::SameLine();
	if (EditorWidgets::smallButton("Fit")) intent.fit = true;
	if (st.view.zoom > 1.001f)
	{ ImGui::SameLine(); ImGui::TextDisabled("%.0f%%", st.view.zoom * 100.0f); }

	// The view toggle says where it GOES, not where it is: the strip already
	// shows which one you are in.
	ImGui::SameLine();
	ImGui::TextDisabled("·");
	ImGui::SameLine();
	if (EditorWidgets::smallButton(st.view.curves ? "Dope Sheet##seq_view" : "Curves##seq_view"))
		st.view.curves = !st.view.curves;
	EditorWidgets::helpForKey("sequencer.view");

	// ── Adding and removing ──────────────────────────────────────────────────
	const bool haveTrack = st.view.trackSel >= 0 &&
	                       st.view.trackSel < static_cast<int>(clip.channels.size());
	ImGui::SameLine();
	ImGui::TextDisabled("·");
	ImGui::SameLine();
	if (EditorWidgets::smallButton("Add Track")) ImGui::OpenPopup("##seq_addtrack");
	if (ImGui::BeginPopup("##seq_addtrack"))
	{
		// Every property, in the runtime's order, the two groups apart. One
		// already animated is shown but greyed: a second track for it would
		// only overwrite the first (SequencerTimeline.h, addTrack).
		const char* lastGroup = "";
		for (int i = 0; i < Seq::kTargetCount; ++i)
		{
			const auto t = static_cast<PropTarget>(i);
			const char* group = Seq::targetGroup(t);
			if (std::strcmp(group, lastGroup) != 0)
			{
				if (*lastGroup) ImGui::Separator();
				ImGui::TextDisabled("%s", group);
				lastGroup = group;
			}
			const bool have = Seq::findTrack(clip, t) >= 0;
			// The label is data — targetName() — so the row explains itself by
			// key, the way the strip's track rows do.
			const std::string label = std::string(Seq::targetName(t)) + "##seq_addtarget";
			ImGui::BeginDisabled(have);
			if (ImGui::Selectable(label.c_str(), false))
			{
				st.view.trackSel = Seq::addTrack(clip, t);
				st.view.keySel   = 0;
				commit(st, clip);
			}
			ImGui::EndDisabled();
			EditorWidgets::helpForKey("sequencer.add-target");
		}
		ImGui::EndPopup();
	}

	// A key on the selected track at the playhead, holding what the track is
	// there already — so adding a key pins the curve, it does not bend it.
	// On a track with no keys it holds the property's default.
	ImGui::SameLine();
	ImGui::BeginDisabled(!haveTrack);
	if (EditorWidgets::smallButton("Add Key"))
	{
		PropertyAnimChannel& ch = clip.channels[st.view.trackSel];
		const float v = ch.times.empty() ? Seq::defaultValue(ch.target)
		                                 : PropertyAnimationSystem::sampleChannel(ch, st.view.playhead);
		st.view.keySel = Seq::insertKey(ch, st.view.playhead, v);
		commit(st, clip);
	}
	ImGui::SameLine();
	if (EditorWidgets::dangerSmallButton("Remove Track"))
	{
		Seq::removeTrack(clip, st.view.trackSel);
		st.view.trackSel = -1;
		st.view.keySel   = -1;
		commit(st, clip);
	}
	ImGui::EndDisabled();
}

// The row under the strip: the selected key's numbers, editable, or what to
// click to get some.
void drawSelectionReadout(PanelState& st, PropertyAnimClipAsset& clip)
{
	namespace Seq = HE::Ed::Sequencer;
	ImGui::Separator();
	HE::Ed::Sequencer::View& v = st.view;
	const bool haveTrack = v.trackSel >= 0 && v.trackSel < static_cast<int>(clip.channels.size());
	if (!haveTrack)
	{
		ImGui::TextDisabled("Click a track to select it, a key to see its time and value.");
		ImGui::TextDisabled("Double-click a lane to add a key there; drag a key to move it.");
		return;
	}
	PropertyAnimChannel& ch = clip.channels[v.trackSel];
	const int keyCount = static_cast<int>(std::min(ch.times.size(), ch.values.size()));
	ImGui::Text("%s", Seq::targetName(ch.target));
	ImGui::SameLine();
	ImGui::TextDisabled("%s  ·  %d key%s", Seq::targetGroup(ch.target),
	                    keyCount, keyCount == 1 ? "" : "s");
	if (v.keySel < 0 || v.keySel >= keyCount)
	{
		ImGui::TextDisabled("Click a key on this track to edit its time and value, "
		                    "or double-click the lane to add one.");
		return;
	}

	// The key's time and value as fields. Time goes through moveKey so the
	// channel stays sorted and the key keeps being the selected one even when
	// it passes a neighbour; the undo point is hung on the field letting go,
	// not on every tick of a drag.
	ImGui::Text("Key %d of %d", v.keySel + 1, keyCount);
	ImGui::SameLine(); ImGui::TextDisabled("at");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(90.0f);
	float t = ch.times[v.keySel];
	if (ImGui::DragFloat("##seq_keytime", &t, 0.005f, 0.0f, clip.duration, "%.3f s"))
	{
		const int nk = Seq::moveKey(ch, v.keySel, std::clamp(t, 0.0f, clip.duration));
		if (nk >= 0) { v.keySel = nk; v.playhead = ch.times[nk]; st.dirty = true; }
	}
	EditorWidgets::helpForKey("sequencer.key-time");
	if (ImGui::IsItemDeactivatedAfterEdit()) pushUndo(st, clip);

	ImGui::SameLine(); ImGui::TextDisabled("holds");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(110.0f);
	const bool degrees = ch.target == PropTarget::RotX || ch.target == PropTarget::RotY ||
	                     ch.target == PropTarget::RotZ;
	float val = ch.values[v.keySel];
	if (ImGui::DragFloat("##seq_keyvalue", &val, degrees ? 0.5f : 0.01f, 0.0f, 0.0f,
	                     degrees ? "%.1f\xC2\xB0" : "%.3f"))
	{
		ch.values[v.keySel] = val;
		st.dirty = true;
	}
	EditorWidgets::helpForKey("sequencer.key-value");
	if (ImGui::IsItemDeactivatedAfterEdit()) pushUndo(st, clip);
}

// ── The actors ───────────────────────────────────────────────────────────────
// A clip is played by a Property Animator component, and any number of
// entities may carry one pointing at this clip. The Sequencer calls them the
// clip's actors: they are what the preview moves, and this row is where they
// are listed, picked and bound.

// The entities whose Property Animator plays this clip, in registry order.
std::vector<entt::entity> actorsOf(const AppContext& ctx, HE::UUID clipId)
{
	std::vector<entt::entity> out;
	if (!ctx.world || clipId == HE::UUID{}) return out;
	for (auto [e, pa] : ctx.world->registry().view<PropertyAnimatorComponent>().each())
		if (pa.clipId == clipId) out.push_back(e);
	return out;
}

const char* actorName(const AppContext& ctx, entt::entity e)
{
	const auto* n = ctx.world->registry().try_get<NameComponent>(e);
	return (n && !n->name.empty()) ? n->name.c_str() : "(unnamed)";
}

// The row above the strip: who plays this clip, and a way to add the selected
// entity to them.
void drawActors(PanelState& st, AppContext& ctx, const std::vector<entt::entity>& actors)
{
	HE::Ed::Help::Scope helpScope("Sequencer");
	if (!ctx.world) return;
	auto& reg = ctx.world->registry();

	ImGui::TextDisabled("Actors");
	ImGui::SameLine();
	if (actors.empty())
	{
		ImGui::TextDisabled("none — no entity in this scene plays this clip yet.");
	}
	for (size_t i = 0; i < actors.size(); ++i)
	{
		if (i > 0) { ImGui::SameLine(); ImGui::TextDisabled("·"); }
		ImGui::SameLine();
		// A name is data, so the row explains itself by key; the ##id keeps
		// two actors of the same name apart for ImGui.
		const std::string label = std::string(actorName(ctx, actors[i])) + "##seq_actor" +
		                          std::to_string(static_cast<unsigned>(entt::to_integral(actors[i])));
		const bool selected = ctx.selection.contains(actors[i]);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 1.0f));
		if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_None,
		                      ImVec2(ImGui::CalcTextSize(actorName(ctx, actors[i])).x + 8.0f, 0.0f)))
			ctx.selection.set(actors[i]);
		ImGui::PopStyleVar();
		EditorWidgets::helpForKey("sequencer.actor");
	}

	// The selected entity becomes an actor: a Property Animator with this
	// clip, or its existing one pointed here. Disabled when it already plays
	// this clip — there is nothing to do — and when nothing is selected.
	const entt::entity primary = ctx.selection.primary();
	const bool haveSel = primary != entt::null && reg.valid(primary);
	const auto* already = haveSel ? reg.try_get<PropertyAnimatorComponent>(primary) : nullptr;
	const bool bound = already && already->clipId == st.assetId;
	ImGui::SameLine();
	ImGui::TextDisabled("·");
	ImGui::SameLine();
	ImGui::BeginDisabled(!haveSel || bound);
	if (EditorWidgets::smallButton("Bind Selected"))
	{
		if (ctx.undoSys) ctx.undoSys->snapshotNow("Bind Clip");
		PropertyAnimatorComponent& pa = reg.get_or_emplace<PropertyAnimatorComponent>(primary);
		pa.clipId       = st.assetId;
		pa.playbackTime = 0.0f;
	}
	ImGui::EndDisabled();
}

// The preview: every actor driven to the playhead, the way the runtime would
// write it there. EVERY frame the tab is drawn, not only on the frames the
// playhead moved: a Property Animator's own Playing flag is on by default and
// the animation tick runs in the editor outside play mode too, so an actor
// written once and then left alone would run off on its own clock the moment
// a scrub ends, and the readout would say one thing while the viewport shows
// another. While this tab is the one in front, its playhead is the actors'
// clock — the tick runs before the panels (EditorApplication::OnRender), so
// this write is the one the next frame's transform propagation sees. The
// playback time is set as well, so an actor that plays itself carries on from
// the playhead once the tab is closed, not from wherever it had got to.
void previewActors(AppContext& ctx, const std::vector<entt::entity>& actors,
                   const PropertyAnimClipAsset& clip, float playhead)
{
	if (!ctx.world || !ctx.contentManager) return;
	auto& reg = ctx.world->registry();
	for (entt::entity e : actors)
	{
		if (auto* pa = reg.try_get<PropertyAnimatorComponent>(e)) pa->playbackTime = playhead;
		PropertyAnimationSystem::applyAt(*ctx.world, *ctx.contentManager, e, clip, playhead);
	}
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

void SequencerPanel::appendSnapshots(AppContext& ctx, std::vector<HE::Ed::AssetSnapshotSource>& out)
{
	ContentManager* cm = ctx.contentManager;
	if (!cm) return;
	s_states.forEach([&](const std::string&, PanelState& st) {
		if (!st.dirty || st.relPath.empty()) return;
		out.push_back({ cm->resolveSavePath(st.relPath), [cm, &st](const std::string& dest) {
			// The loaded clip IS the edit buffer (saveState writes it as it is),
			// so the copy is of the clip itself.
			const PropertyAnimClipAsset* a = cm->getPropertyAnimClip(st.assetId);
			if (!a) return false;
			PropertyAnimClipAsset copy = *a;
			return cm->writeAssetTo(copy, dest);
		} });
	});
}

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
		st.undo.clear();
		st.undoPos = -1;
		st.loaded  = true;
	}

	HE::Ed::Help::Scope helpScope("Sequencer");
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin(("##sequencer_" + assetPath).c_str(), nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

	PropertyAnimClipAsset* clip = clipOf(st, ctx);
	// The baseline: what undo goes back to before the first edit. Taken on
	// the first frame the clip is there rather than at load, because the load
	// can come back empty-handed (below) and a baseline of nothing is no baseline.
	if (clip && st.undoPos < 0) pushUndo(st, *clip);

	{
		namespace T = EditorToolbar;
		T::Bar bar;
		T::assetHeader(bar, st.name.c_str(), T::iconPlay, st.dirty);
		bar.group();
		bar.readout(nullptr, "Property Animation", T::kFgDim);
		bar.endGroup();
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

	// The transport's tick, before anything that prints or draws the playhead
	// this frame, so the time beside the buttons and the line in the strip are
	// this frame's. A scrub, a click on a key or the Stop button move the
	// playhead as well, further down; the actors are written after all of
	// them (previewActors, below the readout).
	HE::Ed::Sequencer::advancePlayhead(st.view, clip->duration, ImGui::GetIO().DeltaTime);

	HE::Ed::Sequencer::Intent intent;
	drawControls(st, *clip, intent);
	const std::vector<entt::entity> actors = actorsOf(ctx, st.assetId);
	drawActors(st, ctx, actors);

	// ── The strip, then the selection under it ───────────────────────────────
	// The readout's height is fixed and taken off the strip, so the strip is
	// always what is left and the readout never scrolls out from under it.
	// Two rows, the second of which may carry fields — so frame height, not
	// text height, or the fields are what the strip would eat.
	const float readoutH = ImGui::GetTextLineHeightWithSpacing() +
	                       ImGui::GetFrameHeightWithSpacing() +
	                       ImGui::GetStyle().ItemSpacing.y * 3.0f + 6.0f;
	const float stripH = std::max(80.0f, ImGui::GetContentRegionAvail().y - readoutH);
	const HE::Ed::Sequencer::Result r =
		HE::Ed::Sequencer::draw(*clip, st.view, ImVec2(0.0f, stripH), intent);
	if (r.edited)    st.dirty = true;
	if (r.committed) pushUndo(st, *clip);

	drawSelectionReadout(st, *clip);

	// The actors follow the playhead, wherever the transport, a scrub, a key
	// click, Stop or the key-time field above has put it this frame. After
	// the strip and the readout on purpose: both of them can still move it.
	previewActors(ctx, actors, *clip, st.view.playhead);

	// ── Keyboard shortcuts (skip while typing in a field) ────────────────────
	// WantTextInput as well as IsAnyItemActive: a number field that has
	// keyboard focus without being "active" this frame still owns the keys.
	// Ctrl or Cmd: on a Mac the undo chord is Cmd+Z, and ImGui reports Cmd as
	// KeySuper.
	const bool typing = ImGui::IsAnyItemActive() || ImGui::GetIO().WantTextInput;
	if (!typing && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
	{
		const bool ctrl = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
		if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) st.view.playing = !st.view.playing;
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) saveState(st, ctx);
		if (ctrl && !ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z))
			restoreSnapshot(st, *clip, st.undoPos - 1);
		if ((ctrl && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) ||
		    (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)))
			restoreSnapshot(st, *clip, st.undoPos + 1);
	}

	ImGui::End();
}
